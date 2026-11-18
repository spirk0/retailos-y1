package org.retailos;

import java.io.ByteArrayOutputStream;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Writes an iTunesDB that the iPod 5G firmware reads. Plain Java.
 *
 * The layout is iTunesDB version 0x0d, with a track record (mhit) of 0xf4
 * bytes: it has the media type at
 * 0xd0, so the iPod lists audiobooks and podcasts in their menus and does
 * not count songs as videos. Records:
 * <pre>
 * mhbd
 *   mhsd 1: mhlt with one mhit per track (string mhods: title, location,
 *           album, artist, genre, file type)
 *   mhsd 3: (only with podcasts) mhlp with the master playlist and the
 *           podcast playlist: one group mhip per show (the album), then
 *           its episodes, as libgpod writes it
 *   mhsd 2: mhlp with the master playlist (mhyp, mhip per track), and the
 *           podcast playlist without groups
 * The order 1, 3, 2 is that of iTunes 4.9 and later.
 * </pre>
 */
final class ITunesDb {
    /* Charset.forName: java.nio.charset.StandardCharsets is not on Android 4.2 (API 19) */
    private static final Charset UTF16 = Charset.forName("UTF-16LE"), ASCII = Charset.forName("US-ASCII");
    static final int MEDIA_AUDIO = 1, MEDIA_PODCAST = 4, MEDIA_AUDIOBOOK = 8;
    static final long MAC_EPOCH = 2082844800L;              /* seconds from 1904-01-01 to 1970-01-01 */

    private static final int MHIT_SIZE = 0xf4;
    private static final int FIRST_TRACK_ID = 100;

    /** One track. location: ":iPod_Control:Music:F00:NAME.mp3" */
    static final class Track {
        String title = "", artist = "", album = "", genre = "", location = "", fileType = "mp3";
        long size;
        int lengthMs, trackNumber, discNumber, year, bitrateKbps, sampleRate = 44100, mediaType = MEDIA_AUDIO;
        /** the size of the cover's source image, 0 = no cover (ArtworkDb) */
        int artworkSize;
    }

    /** The dbid of the track at this index of the list: the ArtworkDB links a cover to it */
    static long dbid(int index) { return 0x1000000000000000L + FIRST_TRACK_ID + index; }

    private ITunesDb() { }

    /** A little-endian byte buffer with in-place writes */
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

    private static void put32(byte[] a, int o, long v) {
        for (int i = 0; i < 4; i++) a[o + i] = (byte) (v >> (8 * i));
    }

    /** A record: tag, header size, total size (set later), then the header fields */
    private static byte[] record(String tag, int header, byte[] fields, byte[] body) {
        Out o = new Out().tag(tag).u32(header).u32(header + body.length).bytes(fields);
        o.zero(header - o.pos());
        return new Out().bytes(o.array()).bytes(body).array();
    }

    private static byte[] mhodString(int kind, String text) {
        byte[] s = text.getBytes(UTF16);
        byte[] body = new Out().u32(1).u32(s.length).u32(1).u32(0).bytes(s).array();
        return record("mhod", 0x18, new Out().u32(kind).u32(0).u32(0).array(), body);
    }

    private static byte[] mhodPosition(int pos) {
        return record("mhod", 0x18, new Out().u32(100).u32(0).u32(0).array(), new Out().u32(pos).zero(16).array());
    }

    private static String typeName(String t) {
        switch (t) {
            case "mp3": return "MPEG audio file";
            case "wav": return "WAV audio file";
            case "aif": case "aiff": return "AIFF audio file";
            default: return "AAC audio file";
        }
    }

    private static long typeMarker(String t) {
        switch (t) {
            case "mp3": return 0x4d503320L;                 /* "MP3 " */
            case "m4b": return 0x4d344220L;                 /* "M4B " */
            case "wav": return 0x57415620L;                 /* "WAV " */
            case "aif": case "aiff": return 0x41494646L;    /* "AIFF" */
            default: return 0x4d344120L;                    /* "M4A " */
        }
    }

    private static byte[] mhit(int id, Track t, long mac) {
        Out body = new Out().bytes(mhodString(1, t.title)).bytes(mhodString(2, t.location));
        int mhods = 2;
        String[] values = { t.album, t.artist, t.genre };
        for (int k = 0; k < values.length; k++)
            if (values[k] != null && !values[k].isEmpty()) { body.bytes(mhodString(3 + k, values[k])); mhods++; }
        body.bytes(mhodString(6, typeName(t.fileType)));
        mhods++;
        boolean mp3 = t.fileType.equals("mp3");
        boolean spoken = t.mediaType == MEDIA_PODCAST || t.mediaType == MEDIA_AUDIOBOOK;
        Out f = new Out();                                  /* the fields from offset 12 */
        f.u32(mhods).u32(id).u32(1).u32(typeMarker(t.fileType));
        f.u8(mp3 ? 1 : 0).u8(mp3 ? 1 : 0).u8(0).u8(0);     /* 28: type1, type2, compilation, rating */
        f.u32(mac).u32(t.size).u32(t.lengthMs).u32(t.trackNumber).u32(0).u32(t.year).u32(t.bitrateKbps);
        f.u32((long) t.sampleRate << 16);                   /* 60 */
        f.zero(4 * 7);                                      /* 64: volume ... last played */
        f.u32(t.discNumber).u32(0).u32(0).u32(mac).u32(0); /* 92: disc, discs, user id, date added, bookmark */
        f.u64(dbid(id - FIRST_TRACK_ID));                   /* 0x70: dbid */
        boolean art = t.artworkSize > 0;
        f.zero(4);                                          /* 0x78: checked, rating, BPM */
        f.u16(art ? 1 : 0).u16(0).u32(art ? t.artworkSize : 0);   /* 0x7c: artwork count, 0x80: artwork size */
        f.zero(0xa4 - 0x84);
        f.u8(art ? 1 : 2);                                  /* 0xa4: has artwork (1), no artwork (2), as libgpod */
        boolean podcast = t.mediaType == MEDIA_PODCAST;
        f.u8(spoken ? 1 : 0).u8(spoken ? 1 : 0);            /* 0xa5: skip when shuffling, remember the position */
        f.u8(podcast ? 1 : 0);                              /* 0xa7: the podcast flag */
        f.zero(0xb2 - 0xa8);
        f.u8(podcast ? 2 : 0);                              /* 0xb2: unplayed (the blue dot) */
        f.zero(0xd0 - 0xb3);
        f.u32(t.mediaType);                                 /* 0xd0 */
        return record("mhit", MHIT_SIZE, f.array(), body.array());
    }

    private static byte[] mhip(int flag, int groupId, int trackId, int groupRef, long mac, byte[] mhod) {
        Out f = new Out().u32(1).u16(flag).u16(0).u32(groupId).u32(trackId).u32(mac).u32(groupRef);
        return record("mhip", 0x4c, f.array(), mhod);
    }

    private static byte[] mhyp(String name, boolean master, boolean podcast, long id, int items, byte[] itemBytes, long mac) {
        byte[] title = mhodString(1, name);
        Out f = new Out().u32(1).u32(items).u32(master ? 1 : 0).u32(mac).u64(id).u32(0);
        f.u16(1).u16(podcast ? 1 : 0);                      /* 40: string mhods, podcast flag */
        return record("mhyp", 0x6c, f.array(), new Out().bytes(title).bytes(itemBytes).array());
    }

    private static byte[] master(List<Integer> ids, String name, long mac) {
        Out items = new Out();
        for (int i = 0; i < ids.size(); i++) items.bytes(mhip(0, 0x1000 + i + 1, ids.get(i), 0, mac, mhodPosition(i + 1)));
        return mhyp(name, true, false, 0x2000000000000001L, ids.size(), items.array(), mac);
    }

    /** The podcast playlist without groups: in the playlists (mhsd 2), as libgpod */
    private static byte[] podcastsFlat(List<Track> tracks, List<Integer> ids, long mac) {
        Out items = new Out();
        int n = 0;
        for (int i = 0; i < tracks.size(); i++) {
            if (tracks.get(i).mediaType != MEDIA_PODCAST) continue;
            n++;
            items.bytes(mhip(0, 0x4000 + n, ids.get(i), 0, mac, mhodPosition(n)));
        }
        return mhyp("Podcasts", false, true, 0x2000000000000002L, n, items.array(), mac);
    }

    /** The podcast playlist with one group per show: in mhsd 3, for the Podcasts menu */
    private static byte[] podcasts(List<Track> tracks, List<Integer> ids, long mac) {
        Map<String, List<Integer>> shows = new LinkedHashMap<String, List<Integer>>();
        for (int i = 0; i < tracks.size(); i++) {
            if (tracks.get(i).mediaType != MEDIA_PODCAST) continue;
            String show = tracks.get(i).album == null || tracks.get(i).album.isEmpty() ? "Podcast" : tracks.get(i).album;
            if (!shows.containsKey(show)) shows.put(show, new ArrayList<Integer>());
            shows.get(show).add(ids.get(i));
        }
        Out items = new Out();
        int n = 0, next = 0x3000;
        for (Map.Entry<String, List<Integer>> e : shows.entrySet()) {
            int group = next++;
            items.bytes(mhip(0x100, group, 0, 0, mac, mhodString(1, e.getKey())));
            n++;
            int pos = 1;
            for (int id : e.getValue()) {
                items.bytes(mhip(0, next++, id, group, mac, mhodPosition(pos++)));
                n++;
            }
        }
        return mhyp("Podcasts", false, true, 0x2000000000000002L, n, items.array(), mac);
    }

    private static byte[] mhsd(int kind, byte[] child) {
        return record("mhsd", 0x60, new Out().u32(kind).array(), child);
    }

    private static byte[] list(String tag, int count, byte[] body) {
        Out o = new Out().tag(tag).u32(0x5c).u32(count);
        o.zero(0x5c - o.pos());
        return new Out().bytes(o.array()).bytes(body).array();
    }

    /** The database. unixNow: the time for the dates, in seconds since 1970. */
    static byte[] build(List<Track> tracks, String name, long unixNow) {
        long mac = unixNow + MAC_EPOCH;
        Out mhits = new Out();
        List<Integer> ids = new ArrayList<Integer>();
        boolean anyPodcast = false;
        for (int i = 0; i < tracks.size(); i++) {
            int id = FIRST_TRACK_ID + i;
            ids.add(id);
            mhits.bytes(mhit(id, tracks.get(i), mac));
            anyPodcast |= tracks.get(i).mediaType == MEDIA_PODCAST;
        }
        byte[] masterList = master(ids, name, mac);
        /* the order of iTunes 4.9 and later: tracks, podcasts, playlists */
        Out body = new Out().bytes(mhsd(1, list("mhlt", tracks.size(), mhits.array())));
        if (anyPodcast) {
            body.bytes(mhsd(3, list("mhlp", 2, new Out().bytes(masterList).bytes(podcasts(tracks, ids, mac)).array())));
            body.bytes(mhsd(2, list("mhlp", 2, new Out().bytes(masterList).bytes(podcastsFlat(tracks, ids, mac)).array())));
        } else {
            body.bytes(mhsd(2, list("mhlp", 1, masterList)));
        }
        Out f = new Out().u32(1).u32(0x0d).u32(anyPodcast ? 3 : 2).u64(0x5eed5eed5eed5eedL).u16(2);
        byte[] db = record("mhbd", 0x68, f.array(), body.array());
        put32(db, 8, db.length);
        return db;
    }
}
