package org.retailos;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The iPod firmware from an IPSW (a ZIP file from Apple: iPod_25.1.3.ipsw
 * for the iPod 5.5G, iPod_20.1.3.ipsw for the 5G). Its Firmware-* file is
 * the firmware partition of the disk; the retailOS image (osos) is in it,
 * at the "!ATA" entry of the image directory. No Apple file ships in the APK.
 *
 * The osos and rsrc images of 25.1.3 and 20.1.3 are the same bytes. Only
 * the layout of the file is different (see layout()).
 */
final class IpswSetup {
    private IpswSetup() { }

    /** The IPSWs that the app prefers, in this order */
    private static final String[] PREFERRED = { "iPod_25.1.3.ipsw", "iPod_20.1.3.ipsw" };

    /** The IPSW in dir: the first of PREFERRED that is there, else any .ipsw, or null */
    static File find(File dir) {
        for (String name : PREFERRED) {
            File f = new File(dir, name);
            if (f.isFile()) return f;
        }
        File[] files = dir.listFiles();
        if (files == null) return null;
        for (File f : files)
            if (f.isFile() && f.getName().toLowerCase().endsWith(".ipsw")) return f;
        return null;
    }

    /** "name size date": an extraction is done again when the IPSW changes */
    static String stamp(File ipsw) {
        return ipsw.getName() + " " + ipsw.length() + " " + ipsw.lastModified();
    }

    /** 1 if firmware and osos are there and come from this IPSW */
    static boolean ready(File ipsw, File firmware, File osos, File stampFile) {
        return firmware.isFile() && osos.isFile() && stamp(ipsw).equals(read(stampFile));
    }

    /** Extract the firmware partition and osos. Throws with a message for the user. */
    static void extract(File ipsw, File firmware, File osos, File stampFile) throws IOException {
        byte[] fw = null;
        ZipInputStream zin = new ZipInputStream(new FileInputStream(ipsw));   /* no try-with-resources: API 19 */
        try {
            ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                if (!e.getName().startsWith("Firmware-")) continue;
                fw = readAll(zin);
                break;
            }
        } finally {
            AudioInfo.close(zin);
        }
        if (fw == null) throw new IOException(ipsw.getName() + " has no Firmware-* file: is it an iPod IPSW?");
        Image image;
        try {
            image = layout(fw);
        } catch (IOException e) {
            throw new IOException(ipsw.getName() + ": " + e.getMessage());
        }
        write(firmware, fw, 0, fw.length);
        write(osos, fw, image.start, image.length);
        byte[] s = stamp(ipsw).getBytes("UTF-8");
        write(stampFile, s, 0, s.length);
    }

    /** The place of the osos image in a Firmware-* file */
    static final class Image {
        final int start, length;
        Image(int start, int length) { this.start = start; this.length = length; }
    }

    /** The image directory: 40-byte entries, from the first "!ATA" after this offset */
    private static final int DIRECTORY_FROM = 0x1000, ENTRY = 40;
    /** The header before the content of each image: one disk sector. 512 bytes
     * on the 5G, 2048 bytes on the 5.5G (its disk has 2048-byte sectors). */
    private static final int HEADER_5G = 0x200, HEADER_55G = 0x800;

    /**
     * Find osos in fw, and change the directory of a 5.5G file to the 5G
     * layout, in place. retailOS reads an image at its offset + 0x200 + its
     * entry field, the layout of a disk with 512-byte sectors, which the
     * runner gives. In the 5.5G file the content is at offset + 0x800 +
     * entry (rsrc: entry 0x600, its FAT volume at + 0xe00). Without the
     * change, retailOS did not find its fonts in rsrc and restarted for ever.
     * The change adds 0x600 to the offset of each image and removes 0x600
     * from its length: the content stays where it is.
     */
    static Image layout(byte[] fw) throws IOException {
        int dir = find(fw, "!ATA".getBytes("US-ASCII"), DIRECTORY_FROM);
        if (dir < 0) throw new IOException("no image directory in the firmware");
        int osos = -1;
        for (int e = dir; e + ENTRY <= fw.length && fw[e] == '!' && fw[e + 1] == 'A' && fw[e + 2] == 'T' && fw[e + 3] == 'A'; e += ENTRY)
            if (fw[e + 4] == 's' && fw[e + 5] == 'o' && fw[e + 6] == 's' && fw[e + 7] == 'o') osos = e;   /* "osos", reversed */
        if (osos < 0) throw new IOException("no retailOS image (osos) in the firmware");
        int off = le32(fw, osos + 12), len = le32(fw, osos + 16);
        if (vectorsAt(fw, off + HEADER_55G) && !vectorsAt(fw, off + HEADER_5G)) {
            int shift = HEADER_55G - HEADER_5G;
            for (int e = dir; e + ENTRY <= fw.length && fw[e] == '!' && fw[e + 1] == 'A' && fw[e + 2] == 'T' && fw[e + 3] == 'A'; e += ENTRY) {
                put32(fw, e + 12, le32(fw, e + 12) + shift);
                put32(fw, e + 16, le32(fw, e + 16) - shift);
            }
            off += shift;
            len -= shift;
        }
        int start = off + HEADER_5G;
        if (!vectorsAt(fw, start)) throw new IOException("the retailOS image does not start with ARM vectors");
        if (len <= 0 || start + len > fw.length) throw new IOException("a bad image directory");
        return new Image(start, len);
    }

    /** The first two words at o are ARM branches: the exception vectors of an image */
    private static boolean vectorsAt(byte[] fw, int o) {
        return o >= 0 && o + 8 <= fw.length && (le32(fw, o) & 0xff000000) == 0xea000000
                && (le32(fw, o + 4) & 0xff000000) == 0xea000000;
    }

    private static void put32(byte[] b, int o, int v) {
        for (int i = 0; i < 4; i++) b[o + i] = (byte) (v >> (8 * i));
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] b = new byte[1 << 16];
        int n;
        while ((n = in.read(b)) > 0) out.write(b, 0, n);
        return out.toByteArray();
    }

    private static void write(File f, byte[] b, int off, int len) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        FileOutputStream out = new FileOutputStream(tmp);
        try { out.write(b, off, len); } finally { out.close(); }
        tmp.setReadable(true, false);                       /* the runner reads it as root, but also for adb */
        if (!tmp.renameTo(f)) throw new IOException("cannot write " + f);
    }

    private static String read(File f) {
        FileInputStream in = null;
        try {
            in = new FileInputStream(f);
            return new String(readAll(in), "UTF-8");
        } catch (IOException e) {
            return "";
        } finally {
            AudioInfo.close(in);
        }
    }

    private static int le32(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24;
    }

    private static int find(byte[] hay, byte[] needle, int from) {
        outer:
        for (int i = from; i <= hay.length - needle.length; i++) {
            for (int j = 0; j < needle.length; j++) if (hay[i + j] != needle[j]) continue outer;
            return i;
        }
        return -1;
    }
}
