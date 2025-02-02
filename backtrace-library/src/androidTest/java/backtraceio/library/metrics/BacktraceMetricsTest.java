package backtraceio.library.metrics;

import static org.junit.Assert.assertEquals;

import android.content.Context;
import android.support.test.InstrumentationRegistry;
import android.support.test.runner.AndroidJUnit4;

import junit.framework.TestCase;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.HashMap;
import java.util.Map;

import backtraceio.library.BacktraceClient;
import backtraceio.library.BacktraceCredentials;
import backtraceio.library.BacktraceDatabase;
import backtraceio.library.common.BacktraceTimeHelper;
import backtraceio.library.logger.BacktraceLogger;
import backtraceio.library.logger.LogLevel;
import backtraceio.library.models.BacktraceMetricsSettings;
import backtraceio.library.models.metrics.SummedEvent;
import backtraceio.library.models.metrics.UniqueEvent;
import backtraceio.library.services.BacktraceMetrics;

@RunWith(AndroidJUnit4.class)
public class BacktraceMetricsTest {
    public Context context;
    public BacktraceClient backtraceClient;
    public BacktraceCredentials credentials;
    private final String summedEventName = "activity-changed";
    // existing attribute name in Backtrace
    private final String[] uniqueAttributeName = {"uname.version", "cpu.boottime", "screen.orientation", "battery.state", "device.airplane_mode", "device.sdk", "device.brand", "system.memory.total", "uname.sysname", "application.package"};

    private final String token = "aaaaabbbbbccccf82668682e69f59b38e0a853bed941e08e85f4bf5eb2c5458";
    private final String universeName = "testing-universe-name";

    /**
     * NOTE: Some of these tests are very time-sensitive so you may occasionally get false negative results.
     * For best results run under low CPU load and low memory utilization conditions.
     */

    @Before
    public void setUp() {
        context = InstrumentationRegistry.getContext();
        credentials = new BacktraceCredentials("https://universe.sp.backtrace.io:6098", token);
        BacktraceDatabase database = new BacktraceDatabase(context, context.getFilesDir().getAbsolutePath());

        backtraceClient = new BacktraceClient(context, credentials, database);

        BacktraceLogger.setLevel(LogLevel.DEBUG);
    }

    @Test
    public void addAttributesSummedEvent() {
        SummedEvent summedEvent = new SummedEvent(summedEventName, null);
        Map<String, Object> attributes = new HashMap<String, Object>() {{
            put("foo", "bar");
        }};
        summedEvent.addAttributes(attributes);
        assertEquals("bar", summedEvent.getAttributes().get("foo"));
    }

    @Test
    public void addAttributesUniqueEvent() {
        UniqueEvent uniqueEvent = new UniqueEvent(uniqueAttributeName[0], null);
        Map<String, Object> attributes = new HashMap<String, Object>() {{
            put("foo", "bar");
        }};
        uniqueEvent.update(BacktraceTimeHelper.getTimestampSeconds(), attributes);
        assertEquals("bar", uniqueEvent.getAttributes().get("foo"));
    }

    @Test
    public void testDefaultUrl() {
        BacktraceMetrics metrics = new BacktraceMetrics(context, new HashMap<String, Object>(), null);
        metrics.enable(new BacktraceMetricsSettings(credentials));
        TestCase.assertEquals(BacktraceMetrics.defaultBaseUrl, metrics.getBaseUrl());
    }

    @Test
    public void testCustomUrl() {
        String customUrl = "https://my.custom.url";
        BacktraceMetrics metrics = new BacktraceMetrics(context, new HashMap<String, Object>(), null);
        metrics.enable(new BacktraceMetricsSettings(credentials, customUrl));
        TestCase.assertEquals(customUrl, metrics.getBaseUrl());
    }
}
