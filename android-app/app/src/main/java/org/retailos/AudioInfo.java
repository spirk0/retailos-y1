package org.retailos;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.Charset;

/**
 * The sample rate of an audio file, from its header: Android 4.2's
 * MediaMetadataRetriever does not give it. Plain Java.
 */
final class AudioInfo {
    /* Charset.forName: java.nio.charset.StandardCharsets is not on Android 4.2 (API 19) */
    private static final Charset ASCII = Charset.forName("US-ASCII");
    static final int DEFAULT_RATE = 44100;
    private static final int[][] MP3_RATES = {
        { 11025, 12000, 8000 },                             /* MPEG 2.5 */
        { 0, 0, 0 },
        { 22050, 24000, 16000 },                            /* MPEG 2 */
        { 44100, 48000, 32000 },                            /* MPEG 1 */
    };

    private AudioInfo() { }

    /* Only the parts that hold the rate: the scan of thousands of files read
     * 1 MB of each before. */
    private static final int MP3_READ = 16 << 10;           /* after the ID3 tag: the first frames */
    private static final int MOOV_MAX = 8 << 20;            /* the moov box: chapters make it larger */
    private static final int WAV_READ = 64;

    static int sampleRate(String path, String type) {
        RandomAccessFile f = null;                          /* no try-with-resources: API 19 */
        try {
            f = new RandomAccessFile(path, "r");
            switch (type) {
                case "mp3": return mp3(read(f, id3Size(f), MP3_READ));
                case "m4a": case "m4b": return mp4File(f);
                case "wav": return wav(read(f, 0, WAV_READ));
                default: return DEFAULT_RATE;
            }
        } catch (IOException e) {
            return DEFAULT_RATE;
        } finally {
            close(f);
        }
    }

    private static byte[] read(RandomAccessFile f, long at, int max) throws IOException {
        long len = f.length();
        if (at >= len) return new byte[0];
        byte[] b = new byte[(int) Math.min(max, len - at)];
        f.seek(at);
        f.readFully(b);
        return b;
    }

    /** the size of the ID3v2 tag at the start, or 0 */
    private static long id3Size(RandomAccessFile f) throws IOException {
        byte[] h = read(f, 0, 10);
        if (h.length < 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3') return 0;
        return 10 + ((h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f));
    }

    /** the top-level boxes by their sizes: only the moov box is read */
    private static int mp4File(RandomAccessFile f) throws IOException {
        long len = f.length(), at = 0;
        for (int n = 0; n < 64 && at + 8 <= len; n++) {
            byte[] h = read(f, at, 16);
            long size = (h[0] & 0xffL) << 24 | (h[1] & 0xffL) << 16 | (h[2] & 0xffL) << 8 | (h[3] & 0xffL);
            if (size == 1 && h.length >= 16) {
                size = 0;
                for (int i = 8; i < 16; i++) size = size << 8 | (h[i] & 0xffL);
            } else if (size == 0) {
                size = len - at;                            /* to the end of the file */
            }
            if (size < 8) break;
            if (h[4] == 'm' && h[5] == 'o' && h[6] == 'o' && h[7] == 'v')
                return mp4(read(f, at, (int) Math.min(size, MOOV_MAX)));
            at += size;
        }
        return DEFAULT_RATE;
    }

    static void close(java.io.Closeable c) {
        try { if (c != null) c.close(); } catch (IOException ignore) { }
    }

    /** The sample rate in b (the start of the file, or for MP4 any part), or DEFAULT_RATE */
    static int sampleRate(byte[] b, String type) {
        switch (type) {
            case "mp3": return mp3(b);
            case "m4a": case "m4b": return mp4(b);
            case "wav": return wav(b);
            default: return DEFAULT_RATE;
        }
    }

    private static int mp3(byte[] b) {
        int o = 0;
        if (b.length >= 10 && b[0] == 'I' && b[1] == 'D' && b[2] == '3')
            o = 10 + ((b[6] & 0x7f) << 21 | (b[7] & 0x7f) << 14 | (b[8] & 0x7f) << 7 | (b[9] & 0x7f));
        for (; o + 3 < b.length; o++) {
            if ((b[o] & 0xff) != 0xff || (b[o + 1] & 0xe0) != 0xe0) continue;
            int version = (b[o + 1] >> 3) & 3, layer = (b[o + 1] >> 1) & 3, rate = (b[o + 2] >> 2) & 3;
            if (version == 1 || layer == 0 || rate == 3) continue;
            return MP3_RATES[version][rate];
        }
        return DEFAULT_RATE;
    }

    private static int mp4(byte[] b) {
        byte[][] entries = { "mp4a".getBytes(ASCII), "alac".getBytes(ASCII) };
        for (int o = 0; o + 32 <= b.length; o++)
            for (byte[] e : entries)
                if (b[o] == e[0] && b[o + 1] == e[1] && b[o + 2] == e[2] && b[o + 3] == e[3]) {
                    int rate = (b[o + 28] & 0xff) << 8 | (b[o + 29] & 0xff);   /* 16.16 at +28, big endian */
                    if (rate >= 8000 && rate <= 192000) return rate;
                }
        return DEFAULT_RATE;
    }

    private static int wav(byte[] b) {
        if (b.length < 28 || b[0] != 'R' || b[8] != 'W' || b[12] != 'f') return DEFAULT_RATE;
        int rate = (b[24] & 0xff) | (b[25] & 0xff) << 8 | (b[26] & 0xff) << 16 | (b[27] & 0xff) << 24;
        return rate > 0 ? rate : DEFAULT_RATE;
    }
}
