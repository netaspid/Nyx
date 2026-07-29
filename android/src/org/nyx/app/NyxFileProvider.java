package org.nyx.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;

public final class NyxFileProvider extends ContentProvider {
    private static final String AUTHORITY = "org.nyx.app.fileprovider";

    public static Uri uriForFile(Context context, File file) {
        final String path = file.getAbsolutePath();
        return Uri.parse("content://" + AUTHORITY + path);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public void attachInfo(Context context, ProviderInfo info) {
        super.attachInfo(context, info);
        if (info.exported) {
            throw new SecurityException("NyxFileProvider must not be exported");
        }
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        final File file = fileForUri(uri);
        final String[] columns = projection != null ? projection
                : new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        final MatrixCursor cursor = new MatrixCursor(columns, 1);
        final Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; ++i) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) {
                row[i] = file.getName();
            } else if (OpenableColumns.SIZE.equals(columns[i])) {
                row[i] = file.length();
            }
        }
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        final File file = fileForUri(uri);
        final String name = file.getName();
        final int dot = name.lastIndexOf('.');
        if (dot >= 0) {
            final String ext = name.substring(dot + 1).toLowerCase();
            final String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
            if (mime != null) return mime;
        }
        return "application/octet-stream";
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        final File file = fileForUri(uri);
        final int modeBits = ParcelFileDescriptor.parseMode(mode == null ? "r" : mode);
        return ParcelFileDescriptor.open(file, modeBits);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection,
                      String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }

    private static File fileForUri(Uri uri) {
        String path = uri.getPath();
        if (path == null || path.isEmpty()) {
            throw new IllegalArgumentException("Empty path");
        }
        final File file = new File(path);
        if (!file.isFile()) {
            throw new IllegalArgumentException("Not a file: " + path);
        }
        return file;
    }
}
