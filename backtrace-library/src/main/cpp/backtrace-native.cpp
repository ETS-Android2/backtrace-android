#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include <jni.h>
#include <string>
#include <android/log.h>
#include <unordered_map>
#include <libgen.h>

#include "base/logging.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/crash_report_database.h"
#include "client/settings.h"

#include "bun/bun.h"
#include "bun/stream.h"
#include "bun/utils.h"
#include "libbun/src/bun_internal.h"
#include "bcd.h"

#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/system_properties.h>

using namespace base;

// supported JNI version
static jint JNI_VERSION = JNI_VERSION_1_6;

// Java VM
static JavaVM *javaVm;

// crashpad client
static crashpad::CrashpadClient *client;

// check if crashpad client is already initialized
static std::atomic_bool initialized;
static std::mutex attribute_synchronization;
static std::string thread_id;

/* Default size of bun buffer. */
#define BUFFER_SIZE	65536

// bun handle
struct bun_handle handle;
// bun buffer
//char buf[BUFFER_SIZE];

/* This descriptor is shared between the child and parent. */
static int buffer_fd;

static char *buffer_child;

static bcd_t bcd;
static struct bun_buffer buf;

// is bun initialized
static std::atomic_bool bun_initialized;

JNIEXPORT jint JNI_OnLoad(JavaVM *jvm, void *reserved) {
    JNIEnv *env;
    if (jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION) != JNI_OK) {

        __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android", "%s",
                            "Cannot load the JNI env");
        return JNI_ERR;
    }
    javaVm = jvm;
    thread_id = std::to_string(gettid());
    return JNI_VERSION_1_4;
}

namespace /* anonymous */
{
    static void *
    buffer_reader(int fd, int flags)
    {
        void *r;

        r = mmap(NULL, BUFFER_SIZE, flags, MAP_SHARED, fd, 0);
        if (r == MAP_FAILED)
            abort();

        return r;
    }

    static void *
    buffer_create(void)
    {
        void *r;
        int fd;

        fd = bun_memfd_create("_backtrace_buffer", MFD_CLOEXEC);
        if (fd == -1)
            abort();

        if (ftruncate(fd, BUFFER_SIZE) == -1)
            abort();

        buffer_fd = fd;

        r = buffer_reader(buffer_fd, PROT_READ|PROT_WRITE);

        return r;
    }

    static int
    request_handler(pid_t tid)
    {
        bun_handle handle;

        bool bun_initialized = bun_handle_init(&handle, BUN_BACKEND_LIBUNWINDSTACK);
        __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android", "Bun initialized? %d", bun_initialized);
        if (!bun_initialized) {
            return -1;
        }

        bun_buffer buf = { buffer_child, BUFFER_SIZE };

        auto written = bun_unwind_remote(&handle, &buf, tid);
        __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android", "Bun bytes written %d", written);
        (void) written;

        return -1;
    }

    static void
    monitor_init(void)
    {
        /*
         * This is called after the parent process has set buffer_fd. Set
         * a memory mapping to the same descriptor.
         */
        buffer_child = (char *)buffer_reader(buffer_fd, PROT_READ | PROT_WRITE);
        if (buffer_child == NULL)
            abort();

        pid_t child_pid = getpid();
        memcpy(buffer_child, &child_pid, sizeof(child_pid));
        return;
    }

    /*
     * Signal handler executed by CrashpadClient::SetFirstChanceExceptionHandler.
     */
    bool FirstChanceHandler(int signum, siginfo_t *info, ucontext_t *context)
    {
        (void) signum;
        (void) info;
        (void) context;

        bcd_emit(&bcd, "1");

        return false;
    }

    bool sdkSupportsClientSideUnwinding() {
        char sdk_ver_str[PROP_VALUE_MAX];
        int sdk_ver = -1;
        if (__system_property_get("ro.build.version.sdk", sdk_ver_str)) {
            sdk_ver = atoi(sdk_ver_str);
            if (sdk_ver < 23) {
                __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android",
                                    "Client side unwinding not supported on SDK version %d",
                                    sdk_ver);
                return false;
            }
        } else {
            __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android",
                                "Could not get SDK version, cannot safely enable client side unwinding");
            return false;
        }
        return true;
    }

    /**
     * Get JNI Environment pointer
     * Executing JNIEnv methods on background thread might cause a crash - by default we attached
     * JNIEnv to main thread - background thread wasn't available. This method allows to return
     * safety JNIEnv and detach it once we done with all native operations.
     */
    JNIEnv *GetJniEnv() {
        JNIEnv *m_pJniEnv;
        int nEnvStat = javaVm->GetEnv(reinterpret_cast<void **>(&m_pJniEnv), JNI_VERSION_1_6);

        if (nEnvStat == JNI_EDETACHED) {
            JavaVMAttachArgs args;
            args.version = JNI_VERSION_1_6;

            if (javaVm->AttachCurrentThread(&m_pJniEnv, &args) != 0) {
                return nullptr;
            }

            thread_local struct DetachJniOnExit {
                ~DetachJniOnExit() {
                    javaVm->DetachCurrentThread();
                }
            };
        } else if (nEnvStat == JNI_EVERSION) {
            return nullptr;
        }
        return m_pJniEnv;
    }

    bool InitializeImpl(jstring url,
                        jstring database_path,
                        jstring handler_path,
                        jobjectArray attributeKeys,
                        jobjectArray attributeValues,
                        jobjectArray attachmentPaths = nullptr,
                        jboolean enableClientSideUnwinding = false) {
        using namespace crashpad;
        // avoid multi initialization
        if (initialized) {
            return true;
        }

        JNIEnv *env = GetJniEnv();
        if (env == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android", "Cannot initialize JNIEnv");
            return false;
        }

        std::map<std::string, std::string> attributes;
        attributes["format"] = "minidump";
        // save native main thread id
        if(!thread_id.empty()) {
            attributes["thread.main"] = thread_id;
        }

        jint keyLength = env->GetArrayLength(attributeKeys);
        jint valueLength = env->GetArrayLength(attributeValues);
        if (keyLength == valueLength) {
            for (int attributeIndex = 0; attributeIndex < keyLength; ++attributeIndex) {
                jstring jstringKey = (jstring) env->GetObjectArrayElement(attributeKeys,
                                                                          attributeIndex);
                jboolean isCopy;
                const char *convertedKey = (env)->GetStringUTFChars(jstringKey, &isCopy);

                jstring stringValue = (jstring) env->GetObjectArrayElement(attributeValues,
                                                                           attributeIndex);
                const char *convertedValue = (env)->GetStringUTFChars(stringValue, &isCopy);

                if (!convertedKey || !convertedValue)
                    continue;

                attributes[convertedKey] = convertedValue;

                env->ReleaseStringUTFChars(jstringKey, convertedKey);
                env->ReleaseStringUTFChars(stringValue, convertedValue);
            }
        } else {
            __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android",
                                "Attribute array length doesn't match. Attributes won't be available in the Crashpad integration");
        }

        std::vector<std::string> arguments;
        arguments.push_back("--no-rate-limit");

        // Backtrace url
        const char *backtraceUrl = env->GetStringUTFChars(url, 0);

        // path to crash handler executable
        const char *handlerPath = env->GetStringUTFChars(handler_path, 0);
        FilePath handler(handlerPath);

        // path to crashpad database
        const char *filePath = env->GetStringUTFChars(database_path, 0);
        FilePath db(filePath);

        // paths to file attachments
        if (attachmentPaths != nullptr) {
            jint attachmentsLength = env->GetArrayLength(attachmentPaths);
            for (int attachmentIndex = 0; attachmentIndex < attachmentsLength; ++attachmentIndex) {
                jstring jstringAttachmentPath = (jstring) env->GetObjectArrayElement(
                        attachmentPaths,
                        attachmentIndex);
                jboolean isCopy;
                const char *convertedAttachmentPath = (env)->GetStringUTFChars(
                        jstringAttachmentPath, &isCopy);

                if (!convertedAttachmentPath)
                    continue;

                std::string attachmentBaseName = basename(convertedAttachmentPath);

                std::string attachmentArgumentString("--attachment=");
                attachmentArgumentString += attachmentBaseName;
                attachmentArgumentString += "=";
                attachmentArgumentString += convertedAttachmentPath;
                arguments.push_back(attachmentArgumentString);

                env->ReleaseStringUTFChars(jstringAttachmentPath, convertedAttachmentPath);
            }
        }

        std::unique_ptr<CrashReportDatabase> database = CrashReportDatabase::Initialize(db);
        if (database == nullptr || database->GetSettings() == NULL) {
            return false;
        }

        /* Enable automated uploads. */
        database->GetSettings()->SetUploadsEnabled(true);

        // Start crash handler
        client = new CrashpadClient();

        initialized = client->StartHandlerAtCrash(handler, db, db, backtraceUrl, attributes,
                                                  arguments);

        env->ReleaseStringUTFChars(url, backtraceUrl);
        env->ReleaseStringUTFChars(handler_path, handlerPath);
        env->ReleaseStringUTFChars(database_path, filePath);

        if (initialized && bun_initialized && sdkSupportsClientSideUnwinding()) {
            /* Set the Minidump data stream to our buffer. */
            crashpad::CrashpadInfo::GetCrashpadInfo()->AddUserDataMinidumpStream(
                                                          BUN_STREAM_ID, buf.data, buf.size);

            /*
             * Set signal/exception handler for the libbun stream.
             */
            if (FirstChanceHandler)
                crashpad::CrashpadClient::SetFirstChanceExceptionHandler(FirstChanceHandler);
            __android_log_print(ANDROID_LOG_DEBUG, "Backtrace-Android",
                                "Client side unwinding enabled in crashpad");
        } else if (!initialized) {
            __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android",
                                "Crashpad not initialized properly, cannot enable client side unwinding");
        }

        return initialized;
    }
}

extern "C" {
void Crash() {
    *(volatile int *) 0 = 0;
}

void DumpWithoutCrash(jstring message, jboolean set_main_thread_as_faulting_thread) {
    crashpad::NativeCPUContext context;
    crashpad::CaptureContext(&context);

    // set dump message for single report
    crashpad::SimpleStringDictionary *annotations = NULL;
    bool did_attach = false;

    if (message != NULL || set_main_thread_as_faulting_thread == true) {
        JNIEnv *env = GetJniEnv();
        if (env == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android", "Cannot initialize JNIEnv");
            return;
        }
        const std::lock_guard<std::mutex> lock(attribute_synchronization);
        crashpad::CrashpadInfo *info = crashpad::CrashpadInfo::GetCrashpadInfo();
        annotations = info->simple_annotations();
        if (!annotations) {
            annotations = new crashpad::SimpleStringDictionary();
            info->set_simple_annotations(annotations);
        }
        if(set_main_thread_as_faulting_thread == true) {
            annotations->SetKeyValue("_mod_faulting_tid", thread_id);
        }
        if(message != NULL) {
            // user can't override error.message - exception message that Crashpad/crash-reporting tool
            // will set to tell user about error message. This code will set error.message only for single
            // report and after creating a dump, method will clean up this attribute.
            jboolean isCopy;
            const char *rawMessage = env->GetStringUTFChars(message, &isCopy);
            annotations->SetKeyValue("error.message", rawMessage);
            env->ReleaseStringUTFChars(message, rawMessage);
        }
    }
    client->DumpWithoutCrash(&context);

    if (annotations != NULL) {
        annotations->RemoveKey("error.message");
    }
}

JNIEXPORT void JNICALL Java_backtraceio_library_base_BacktraceBase_crash(
        JNIEnv *env,
        jobject /* this */) {
    Crash();
}

bool Initialize(jstring url,
                jstring database_path,
                jstring handler_path,
                jobjectArray attributeKeys,
                jobjectArray attributeValues,
                jobjectArray attachmentPaths = nullptr,
                jboolean enableClientSideUnwinding = false) {
    static std::once_flag initialize_flag;

    std::call_once(initialize_flag, [&] {
        initialized = InitializeImpl(url,
                                     database_path, handler_path,
                                     attributeKeys, attributeValues,
                                     attachmentPaths, enableClientSideUnwinding);
    });
    return initialized;
}

JNIEXPORT jboolean JNICALL
Java_backtraceio_library_BacktraceDatabase_initialize(JNIEnv *env,
                                                      jobject thiz,
                                                      jstring url,
                                                      jstring database_path,
                                                      jstring handler_path,
                                                      jobjectArray attributeKeys,
                                                      jobjectArray attributeValues,
                                                      jobjectArray attachmentPaths = nullptr,
                                                      jboolean enableClientSideUnwinding = false) {
    return Initialize(url, database_path, handler_path, attributeKeys,
                      attributeValues, attachmentPaths, enableClientSideUnwinding);
}


void AddAttribute(jstring key, jstring value) {
    if (initialized == false) {
        __android_log_print(ANDROID_LOG_WARN, "Backtrace-Android",
                            "Crashpad integration isn't available. Please initialize the Crashpad integration first.");
        return;
    }
    JNIEnv *env = GetJniEnv();
    if (env == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android", "Cannot initialize JNIEnv");
        return;
    }

    const std::lock_guard<std::mutex> lock(attribute_synchronization);
    crashpad::CrashpadInfo *info = crashpad::CrashpadInfo::GetCrashpadInfo();
    crashpad::SimpleStringDictionary *annotations = info->simple_annotations();
    if (!annotations) {
        annotations = new crashpad::SimpleStringDictionary();
        info->set_simple_annotations(annotations);
    }

    jboolean isCopy;
    const char *crashpadKey = env->GetStringUTFChars(key, &isCopy);
    const char *crashpadValue = env->GetStringUTFChars(value, &isCopy);
    if (crashpadKey && crashpadValue)
        annotations->SetKeyValue(crashpadKey, crashpadValue);

    env->ReleaseStringUTFChars(key, crashpadKey);
    env->ReleaseStringUTFChars(value, crashpadValue);
}

JNIEXPORT void JNICALL
Java_backtraceio_library_BacktraceDatabase_addAttribute(JNIEnv *env, jobject thiz,
                                                        jstring name, jstring value) {
    AddAttribute(name, value);
}

JNIEXPORT void JNICALL
Java_backtraceio_library_base_BacktraceBase_dumpWithoutCrash__Ljava_lang_String_2(JNIEnv *env,
                                                                                  jobject thiz,
                                                                                  jstring message) {
    DumpWithoutCrash(message, false);
}
JNIEXPORT void JNICALL
Java_backtraceio_library_base_BacktraceBase_dumpWithoutCrash__Ljava_lang_String_2Z(JNIEnv *env,
                                                                                   jobject thiz,
                                                                                   jstring message,
                                                                                   jboolean set_main_thread_as_faulting_thread) {
    DumpWithoutCrash(message, set_main_thread_as_faulting_thread);
}

bool EnableClientSideUnwinding(const char* path) {
    if (!sdkSupportsClientSideUnwinding()) {
        return false;
    }

    if (initialized) {
        __android_log_print(ANDROID_LOG_ERROR, "Backtrace-Android",
                            "Client side unwinding needs to be enabled BEFORE crashpad initialization");
        return false;
    }

    struct bcd_config cf;
    bcd_error_t e;

    /* Initialize a shared memory region. */
    static const char *buffer = static_cast<char *>(buffer_create());

    /* Initialize the BCD configuration file. */
    if (bcd_config_init(&cf, &e) == -1)
        abort();

    /* Request handler to be called when processing errors by BCD worker. */
    cf.request_handler = request_handler;

    /* Set a function to be called by the child for setting permissions. */
    cf.monitor_init = monitor_init;

    /* Create the fully resolved path to the bcd socket file */
    const char* file_name = "/bcd.socket";
    char* full_path = (char*) malloc((strlen(path) + strlen(file_name) + 1) * sizeof(char));
    strcpy(full_path, path);
    strcat(full_path, file_name);

    cf.ipc.us.path = full_path;

    if (bcd_init(&cf, &e) == -1) {
        abort();
    }

    /* Initialize the BCD handler. */
    if (bcd_attach(&bcd, &e) == -1)
        abort();

    buf.data = (char *)buffer;
    buf.size = BUFFER_SIZE;

    pid_t child_pid;
    memcpy(&child_pid, buffer, sizeof(child_pid));

    prctl(PR_SET_PTRACER, child_pid, 0, 0, 0);
    prctl(PR_SET_DUMPABLE, 1);

    bun_initialized = true;
    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_backtraceio_library_BacktraceDatabase_enableClientSideUnwinding(JNIEnv *env, jobject thiz, jstring path) {
    const char *path_cstr = env->GetStringUTFChars(path, 0);
    bool enabled = EnableClientSideUnwinding(path_cstr);
    env->ReleaseStringUTFChars(path, path_cstr);
    return enabled;
}
}