package backtraceio.library.models.database;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

import backtraceio.library.common.BacktraceSerializeHelper;
import backtraceio.library.interfaces.IBacktraceDatabaseRecordWriter;

public class BacktraceDatabaseRecordWriter implements IBacktraceDatabaseRecordWriter {

    /**
     * Path to destination directory
     */
    private final String _destinationPath;

    /**
     * Initialize new database record writer
     *
     * @param path path to destination folder
     */
    BacktraceDatabaseRecordWriter(String path) {
        this._destinationPath = path;
    }

    public String write(Object data, String prefix) throws IOException {
        String json = toJsonFile(data);

        byte[] file = json.getBytes(StandardCharsets.UTF_8);
        return write(file, prefix);
    }

    public String write(byte[] data, String prefix) throws IOException {
        String filename = String.format("%s.json", prefix);
        String tempFilePath = new File(this._destinationPath, String.format("temp_%s", filename)).getAbsolutePath();
        saveTemporaryFile(tempFilePath, data);

        String destFilePath = new File(this._destinationPath, filename).getAbsolutePath();
        this.saveValidRecord(tempFilePath, destFilePath);
        return destFilePath;
    }

    /**
     * Serialize object
     *
     * @param data object that will be serialized
     * @return serialized object in JSON string
     */
    public String toJsonFile(Object data) {
        if (data == null) {
            return "";
        }
        return BacktraceSerializeHelper.toJson(data);
    }

    /**
     * Save valid diagnostic data from temporary file
     *
     * @param sourcePath      temporary file path
     * @param destinationPath destination path
     * @throws IOException
     */
    public void saveValidRecord(String sourcePath, String destinationPath) throws IOException {
        File fromFile = new File(sourcePath);
        File toFile = new File(destinationPath);
        boolean renameResult = fromFile.renameTo(toFile);
        if (!renameResult) {
            throw new IOException(String.format("Can not rename file. Source path: %s, destination path: %s", sourcePath, destinationPath));
        }
    }

    /**
     * Save temporary file to hard drive
     *
     * @param path path to temporary file
     * @param file current file
     * @throws IOException
     */
    public void saveTemporaryFile(String path, byte[] file) throws IOException {
        FileOutputStream out = new FileOutputStream(path);
        out.write(file);
        out.close();
    }
}
