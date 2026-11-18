package org.retailos;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.media.MediaMetadataRetriever;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The album covers for the ArtworkDB (ArtworkDb.java): one image per
 * album, from the first file of the album with a picture. The picture is
 * the one in the file's tags (ID3 APIC, MP4 covr), else an image next to
 * the file (cover.jpg, folder.jpg, front.jpg, album.jpg, also .png). Each
 * cover goes to the .ithmb files once, as RGB565 at 100x100 and 200x200
 * (the middle square of the picture); the tracks of an album share it.
 */
final class CoverArt {
    private static final List<String> FOLDER_IMAGES = Arrays.asList(
            "cover.jpg", "folder.jpg", "front.jpg", "album.jpg", "cover.png", "folder.png", "front.png", "album.png");

    private final File dir;
    private final OutputStream[] out = new OutputStream[ArtworkDb.FORMATS.length];
    private final Bitmap[] scaled = new Bitmap[ArtworkDb.FORMATS.length];
    private final ByteBuffer[] pixels = new ByteBuffer[ArtworkDb.FORMATS.length];
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.DITHER_FLAG);
    private final Map<String, int[]> albums = new HashMap<String, int[]>();   /* key: { slot, source size }, slot -1 = none */
    private final List<ArtworkDb.Item> items = new ArrayList<ArtworkDb.Item>();
    private int slots;

    /** dir: iPod_Control/Artwork of the disk. The .ithmb files are written as temporary files until finish(). */
    CoverArt(File dir) throws IOException {
        this.dir = dir;
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot make " + dir);
        for (int k = 0; k < out.length; k++) {
            ArtworkDb.Format f = ArtworkDb.FORMATS[k];
            out[k] = new BufferedOutputStream(new FileOutputStream(tmp(f.fileName())), 1 << 16);
            scaled[k] = Bitmap.createBitmap(f.width, f.height, Bitmap.Config.RGB_565);
            pixels[k] = ByteBuffer.allocate(f.imageSize());
        }
    }

    private File tmp(String name) { return new File(dir, name + ".tmp"); }

    /** The number of covers (albums with a picture) */
    int count() { return slots; }

    /** The album of a track: album artist (or artist) and album; without an album tag, the folder */
    static String albumKey(String albumArtist, String artist, String album, File f) {
        if (album == null || album.isEmpty()) return "folder:" + f.getParent();
        String who = albumArtist != null && !albumArtist.isEmpty() ? albumArtist : artist == null ? "" : artist;
        return (who + "\u0000" + album).toLowerCase(Locale.US);
    }

    /**
     * The cover of the track with this dbid. m: the open retriever of its
     * file. Returns the size of the source picture for the track's record,
     * 0 if the album has no picture.
     */
    int cover(String key, MediaMetadataRetriever m, File f, long dbid) throws IOException {
        int[] album = albums.get(key);
        if (album == null) {
            album = add(m, f);
            albums.put(key, album);
        }
        if (album[0] < 0) return 0;
        items.add(new ArtworkDb.Item(dbid, album[0], album[1]));
        return album[1];
    }

    /** A new album: find its picture, write it to the files. { slot, size } or { -1, 0 } */
    private int[] add(MediaMetadataRetriever m, File f) throws IOException {
        byte[] picture = null;
        try {
            picture = m.getEmbeddedPicture();
        } catch (RuntimeException e) {                      /* a broken tag: no picture */
        }
        if (picture == null) picture = folderImage(f.getParentFile());
        if (picture == null) return new int[] { -1, 0 };
        Bitmap b = decode(picture);
        if (b == null) return new int[] { -1, 0 };
        int side = Math.min(b.getWidth(), b.getHeight());
        Rect src = new Rect((b.getWidth() - side) / 2, (b.getHeight() - side) / 2, 0, 0);
        src.right = src.left + side;
        src.bottom = src.top + side;
        for (int k = 0; k < out.length; k++) {
            ArtworkDb.Format fmt = ArtworkDb.FORMATS[k];
            new Canvas(scaled[k]).drawBitmap(b, src, new Rect(0, 0, fmt.width, fmt.height), paint);
            pixels[k].rewind();
            scaled[k].copyPixelsToBuffer(pixels[k]);        /* RGB565 in the CPU's order: little endian */
            out[k].write(pixels[k].array(), 0, fmt.imageSize());
        }
        b.recycle();
        return new int[] { slots++, picture.length };
    }

    /** A picture at about the size of the large cover: a large JPEG must not fill the memory */
    private static Bitmap decode(byte[] data) {
        try {
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(data, 0, data.length, o);
            if (o.outWidth <= 0 || o.outHeight <= 0) return null;
            int target = ArtworkDb.FORMATS[ArtworkDb.FORMATS.length - 1].width, sample = 1;
            while (Math.min(o.outWidth, o.outHeight) / (sample * 2) >= target) sample *= 2;
            BitmapFactory.Options d = new BitmapFactory.Options();
            d.inSampleSize = sample;
            d.inPreferredConfig = Bitmap.Config.RGB_565;
            return BitmapFactory.decodeByteArray(data, 0, data.length, d);
        } catch (OutOfMemoryError e) {
            return null;
        }
    }

    private static byte[] folderImage(File dir) {
        File[] files = dir == null ? null : dir.listFiles();
        if (files == null) return null;
        for (String name : FOLDER_IMAGES)
            for (File f : files)
                if (f.isFile() && f.getName().equalsIgnoreCase(name) && f.length() < (16 << 20)) return read(f);
        return null;
    }

    private static byte[] read(File f) {
        FileInputStream in = null;
        try {
            in = new FileInputStream(f);
            byte[] b = new byte[(int) f.length()];
            int got = 0, n;
            while (got < b.length && (n = in.read(b, got, b.length - got)) > 0) got += n;
            return got == b.length ? b : null;
        } catch (IOException e) {
            return null;
        } finally {
            AudioInfo.close(in);
        }
    }

    /** Close the files and put the ArtworkDB and the .ithmb files in place. Without covers: no artwork files. */
    void finish() throws IOException {
        for (OutputStream o : out) o.close();
        for (Bitmap b : scaled) b.recycle();
        File db = new File(dir, "ArtworkDB");
        if (items.isEmpty()) {
            for (ArtworkDb.Format f : ArtworkDb.FORMATS) { tmp(f.fileName()).delete(); new File(dir, f.fileName()).delete(); }
            db.delete();
            return;
        }
        for (ArtworkDb.Format f : ArtworkDb.FORMATS) {
            File done = new File(dir, f.fileName());
            if (done.exists() && !done.delete()) throw new IOException("cannot replace " + done);
            if (!tmp(f.fileName()).renameTo(done)) throw new IOException("cannot write " + done);
        }
        MediaLibrary.writeAtomic(db, ArtworkDb.build(items));
    }

    /** After an error: close and remove the temporary files */
    void abort() {
        for (int k = 0; k < out.length; k++) {
            AudioInfo.close(out[k]);
            tmp(ArtworkDb.FORMATS[k].fileName()).delete();
        }
    }
}
