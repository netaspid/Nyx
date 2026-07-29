package org.nyx.app;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public final class NyxStorageBridge {
    private NyxStorageBridge() {}

    public static String describe(Context context, String uriText) {
        String name = "";
        String mime = "";
        long size = -1;
        try {
            Uri uri = Uri.parse(uriText);
            ContentResolver resolver = context.getContentResolver();
            mime = resolver.getType(uri);
            try (Cursor cursor = resolver.query(uri, null, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    int nameColumn = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    int sizeColumn = cursor.getColumnIndex(OpenableColumns.SIZE);
                    if (nameColumn >= 0) name = cursor.getString(nameColumn);
                    if (sizeColumn >= 0 && !cursor.isNull(sizeColumn)) {
                        size = cursor.getLong(sizeColumn);
                    }
                }
            }
        } catch (Exception ignored) {
        }
        if (name == null) name = "";
        if (mime == null || mime.isEmpty()) mime = "application/octet-stream";
        return name + '\u001f' + mime + '\u001f' + size;
    }

    public static boolean copyToFile(Context context, String uriText, String destination) {
        File target = new File(destination);
        File parent = target.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) return false;
        try (InputStream input = context.getContentResolver().openInputStream(Uri.parse(uriText));
             OutputStream output = new FileOutputStream(target)) {
            if (input == null) return false;
            byte[] buffer = new byte[128 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) {
                if (count > 0) output.write(buffer, 0, count);
            }
            output.flush();
            return true;
        } catch (Exception ignored) {
            target.delete();
            return false;
        }
    }

    public static boolean exportToDownloads(Context context, String sourcePath,
                                            String displayName, String mime) {
        if (Build.VERSION.SDK_INT < 29) return false;
        ContentResolver resolver = context.getContentResolver();
        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, displayName);
        values.put(MediaStore.Downloads.MIME_TYPE,
                   mime == null || mime.isEmpty() ? "application/octet-stream" : mime);
        values.put(MediaStore.Downloads.RELATIVE_PATH,
                   Environment.DIRECTORY_DOWNLOADS + "/Nyx");
        values.put(MediaStore.Downloads.IS_PENDING, 1);
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (uri == null) return false;
        try (InputStream input = new FileInputStream(sourcePath);
             OutputStream output = resolver.openOutputStream(uri, "w")) {
            if (output == null) throw new IllegalStateException("No output stream");
            byte[] buffer = new byte[128 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) {
                if (count > 0) output.write(buffer, 0, count);
            }
            output.flush();
            values.clear();
            values.put(MediaStore.Downloads.IS_PENDING, 0);
            resolver.update(uri, values, null, null);
            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType(mime);
            share.putExtra(Intent.EXTRA_STREAM, uri);
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                           Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(Intent.createChooser(share, "Поделиться файлом")
                                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            return true;
        } catch (Exception ignored) {
            resolver.delete(uri, null, null);
            return false;
        }
    }


    public static boolean openFile(Context context, String sourcePath, String mime) {
        try {
            File file = new File(sourcePath);
            if (!file.isFile()) return false;
            Uri uri = NyxFileProvider.uriForFile(context, file);
            String type = (mime == null || mime.isEmpty())
                    ? "application/octet-stream" : mime;
            Intent view = new Intent(Intent.ACTION_VIEW);
            view.setDataAndType(uri, type);
            view.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                          Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(Intent.createChooser(view, "Открыть файл")
                                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            return true;
        } catch (Exception ignored) {
            return false;
        }
    }
}
