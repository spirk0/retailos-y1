package org.retailos;

import java.io.ByteArrayOutputStream;
import java.nio.charset.Charset;
import java.util.List;

/**
 * Writes the ArtworkDB of the iPod 5G and 5.5G: the album covers. Plain
 * Java.
 *
 * The pixels are in two files next to the ArtworkDB (CoverArt writes
 * them): F1028_1.ithmb with 100x100 images, F1029_1.ithmb with 200x200
 * images, RGB565 little endian, one slot after the other. The layout is
 * that of libgpod (db-artwork-writer.c, the iPod video formats):
 * <pre>
 * mhfd
 *   mhsd 1: mhli with one mhii per track with a cover. mhii.song_id is the
 *           dbid of the track's mhit. Its children: one mhod type 2 per
 *           format, with an mhni (format, offset in the file, size) and
 *           in it an mhod type 3 with the file name (":F1028_1.ithmb").
 *   mhsd 2: mhla, empty (no photo albums)
 *   mhsd 3: mhlf with one mhif per format
 * </pre>
 * The track's mhit also says that it has a cover (ITunesDb.Track.artworkSize).
 */
final class ArtworkDb {
    private static final Charset UTF16 = Charset.forName("UTF-16LE"), ASCII = Charset.forName("US-ASCII");
    private static final int FIRST_IMAGE_ID = 100;

    /** A cover format of the iPod 5G/5.5G */
    static final class Format {
        final int id, width, height;
        Format(int id, int width, int height) { this.id = id; this.width = width; this.height = height; }
        int imageSize() { return width * height * 2; }
        String fileName() { return "F" + id + "_1.ithmb"; }
    }

    /** The small cover (lists, Now Playing) and the large one */
    static final Format[] FORMATS = { new Format(1028, 100, 100), new Format(1029, 200, 200) };

    /** The cover of one track: the track's dbid, its slot in the .ithmb files, the size of the source image */
    static final class Item {
        final long dbid;
        final int slot, sourceSize;
        Item(long dbid, int slot, int sourceSize) { this.dbid = dbid; this.slot = slot; this.sourceSize = sourceSize; }
    }

    private ArtworkDb() { }

    /** A little-endian byte buffer */
    private static final class Out {
        private final ByteArrayOutputStream b = new ByteArrayOutputStream();
        int pos() { return b.size(); }
        Out u8(int v) { b.write(v); return this; }
        Out u16(int v) { u8(v); return u8(v >> 8); }
        Out u32(long v) { u16((int) v); return u16((int) (v >> 16)); }
        Out u64(long v) { u32(v); return u32(v >> 32); }
        Out tag(String t) { byte[] s = t.getBytes(ASCII); b.write(s, 0, 4); return this; }
        Out bytes(byte[] s) { b.write(s, 0, s.length); return this; }
        Out zero(int n) { for (int i = 0; i < n; i++) b.write(0); return this; }
        byte[] array() { return b.toByteArray(); }
    }

    /** A record: tag, header size, total size, the fields from offset 12, zeros to the header size, the body */
    private static byte[] record(String tag, int header, byte[] fields, byte[] body) {
        Out o = new Out().tag(tag).u32(header).u32(header + body.length).bytes(fields);
        o.zero(header - o.pos());
        return o.bytes(body).array();
    }

    /** A list record (mhli, mhla, mhlf): tag, header size, count; no total size */
    private static byte[] list(String tag, int count, byte[] body) {
        Out o = new Out().tag(tag).u32(0x5c).u32(count);
        o.zero(0x5c - o.pos());
        return o.bytes(body).array();
    }

    /** The file name mhod (type 3): UTF-16, padded to 4 bytes. Its header
     * size says 0x18, as iTunes writes it, but 12 more header bytes follow. */
    private static byte[] fileName(Format f) {
        byte[] s = (":" + f.fileName()).getBytes(UTF16);
        int total = 36 + s.length, padding = (4 - total % 4) % 4;
        Out o = new Out().tag("mhod").u32(0x18).u32(total + padding);
        o.u16(3).u8(0).u8(padding).u32(0).u32(0);           /* type, unknown, padding length, 2 unknown */
        o.u32(s.length).u8(2).u8(0).u16(0).u32(0);          /* string length, encoding 2 = UTF-16 */
        return o.bytes(s).zero(padding).array();
    }

    private static byte[] mhni(Format f, int slot) {
        Out fields = new Out().u32(1).u32(f.id).u32((long) slot * f.imageSize()).u32(f.imageSize());
        fields.u16(0).u16(0).u16(f.height).u16(f.width);    /* vertical and horizontal padding, height, width */
        return record("mhni", 0x4c, fields.array(), fileName(f));
    }

    private static byte[] mhii(int imageId, Item item) {
        Out body = new Out();
        for (Format f : FORMATS)                            /* a thumbnail container (type 2) per format */
            body.bytes(record("mhod", 0x18, new Out().u16(2).u16(0).array(), mhni(f, item.slot)));
        Out fields = new Out().u32(FORMATS.length).u32(imageId).u64(item.dbid);
        fields.u32(0).u32(0).u32(0).u32(0).u32(0).u32(item.sourceSize);   /* unknown, rating, unknown, dates, source size */
        return record("mhii", 0x98, fields.array(), body.array());
    }

    private static byte[] mhsd(int index, byte[] child) {
        return record("mhsd", 0x60, new Out().u16(index).u16(0).array(), child);
    }

    /** The ArtworkDB for these covers */
    static byte[] build(List<Item> items) {
        Out images = new Out();
        for (int i = 0; i < items.size(); i++) images.bytes(mhii(FIRST_IMAGE_ID + i, items.get(i)));
        Out files = new Out();
        for (Format f : FORMATS) files.bytes(record("mhif", 0x7c, new Out().u32(0).u32(f.id).u32(f.imageSize()).array(), new byte[0]));
        Out body = new Out()
                .bytes(mhsd(1, list("mhli", items.size(), images.array())))
                .bytes(mhsd(2, list("mhla", 0, new byte[0])))
                .bytes(mhsd(3, list("mhlf", FORMATS.length, files.array())));
        int lastId = items.isEmpty() ? 0 : FIRST_IMAGE_ID + items.size() - 1;   /* libgpod writes the last id */
        Out fields = new Out().u32(0).u32(2).u32(3).u32(0).u32(lastId).zero(16).u8(2);   /* unknown2 = 2, 3 sections */
        return record("mhfd", 0x84, fields.array(), body.array());
    }
}
