package com.jrickey.battleship;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;
import java.util.Locale;

/**
 * BattleShipDX: imports replacement music picked in the Audio Editor.
 *  - .mp3 / .ogg / .wav  -> mods/music/<name>   (used as loose tracks)
 *  - .zip / .o2r         -> mods/<name>.zip      (a pack with music/*.mp3|ogg|wav inside; validated as a zip)
 */
final class MusicImporter {
    private static final String TAG = "ssb64.music";
    private static final long MAX_BYTES = 256L * 1024L * 1024L;

    private MusicImporter() { }

    /** Heavy I/O: call from a background thread. Returns how many files were imported. */
    static int importAll(Context ctx, List<Uri> uris) {
        int ok = 0;
        for (Uri u : uris) {
            String name = displayName(ctx, u);
            String lower = name == null ? "" : name.toLowerCase(Locale.ROOT);
            try {
                if (lower.endsWith(".zip") || lower.endsWith(".o2r")) {
                    if (PackImporter.importPack(ctx, u) != null) ok++;
                } else if (lower.endsWith(".mp3") || lower.endsWith(".ogg") || lower.endsWith(".wav")) {
                    if (copyTrack(ctx, u, name)) ok++;
                } else {
                    Log.w(TAG, "skipped (unsupported type): " + name);
                }
            } catch (Exception e) {
                Log.e(TAG, "import failed: " + name, e);
            }
        }
        return ok;
    }

    private static boolean copyTrack(Context ctx, Uri uri, String name) throws IOException {
        File ext = ctx.getExternalFilesDir(null);
        if (ext == null) return false;
        File dir = new File(new File(ext, "mods"), "music");
        if (!dir.isDirectory() && !dir.mkdirs()) return false;
        File dst = new File(dir, safeName(name));
        File tmp = new File(dir, dst.getName() + ".part");
        long total = 0;
        try (InputStream in = ctx.getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) return false;
            byte[] buf = new byte[128 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                total += n;
                if (total > MAX_BYTES) { throw new IOException("file too large"); }
                out.write(buf, 0, n);
            }
        } catch (IOException e) {
            tmp.delete();
            throw e;
        }
        if (dst.exists()) dst.delete();
        if (!tmp.renameTo(dst)) { tmp.delete(); return false; }
        return true;
    }

    private static String displayName(Context ctx, Uri uri) {
        try (Cursor c = ctx.getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) return c.getString(idx);
            }
        } catch (Exception e) {
            Log.w(TAG, "display-name query failed", e);
        }
        String last = uri.getLastPathSegment();
        return last == null ? "track.mp3" : last;
    }

    private static String safeName(String displayName) {
        String base = displayName == null ? "track.mp3" : displayName;
        int cut = Math.max(base.lastIndexOf('/'), base.lastIndexOf('\\'));
        if (cut >= 0) base = base.substring(cut + 1);
        base = base.replaceAll("[^A-Za-z0-9 ._-]", "_").trim();
        if (base.isEmpty() || base.startsWith(".")) base = "track" + base;
        if (base.length() > 100) {
            String ext = base.substring(base.lastIndexOf('.'));
            base = base.substring(0, 96 - ext.length()) + ext;
        }
        return base;
    }
}
