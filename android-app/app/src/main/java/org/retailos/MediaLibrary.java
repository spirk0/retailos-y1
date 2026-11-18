package org.retailos;

import android.media.MediaMetadataRetriever;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * The music database: it scans the media folders, reads the tags, and
 * writes the iTunesDB into the disk folder and the manifest that maps the
 * files into the virtual disk (native-runner/src/vdisk.c). The files stay
 * where they are: the iPod sees them under iPod_Control/Music/Fnn/ with
 * 8.3 names, as iTunes names them.
 */
final class MediaLibrary {
    /** Progress for the screen: called from the worker thread */
    interface Progress { void step(String text); }

    /** The result for the screen */
    static final class Result {
        int songs, podcasts, audiobooks, covers;
        final Map<String, Integer> skipped = new TreeMap<String, Integer>();   /* reason: count */

        String summary() {
            StringBuilder s = new StringBuilder();
            s.append(songs).append(" songs, ").append(podcasts).append(" podcasts, ").append(audiobooks).append(" audiobooks, ")
                    .append(covers).append(" album covers");
            for (Map.Entry<String, Integer> e : skipped.entrySet())
                s.append("\nskipped: ").append(e.getValue()).append(" ").append(e.getKey());
            return s.toString();
        }
    }

    /** The formats of the iPod 5G (no video) */
    private static final List<String> TYPES = Arrays.asList("mp3", "m4a", "m4b", "wav", "aif", "aiff");
    /** Audio that the iPod cannot play: the result lists it. Other files (cover images, lyrics) are left out silently. */
    private static final List<String> OTHER_AUDIO = Arrays.asList("flac", "ogg", "oga", "opus", "wma", "ape", "mka", "aac", "wv", "dsf", "mpc");
    private static final int FOLDERS = 50;                  /* F00 ... F49, as iTunes */

    private MediaLibrary() { }

    private static String type(File f) {
        String n = f.getName();
        int dot = n.lastIndexOf('.');
        return dot < 0 ? "" : n.substring(dot + 1).toLowerCase(Locale.US);
    }

    private static void collect(File dir, int media, List<File> files, List<Integer> kinds, Result r) {
        File[] list = dir.listFiles();
        if (list == null) return;
        Arrays.sort(list);
        for (File f : list) {
            if (f.isDirectory()) { collect(f, media, files, kinds, r); continue; }
            String t = type(f);
            if (f.getName().startsWith(".")) continue;
            if (!TYPES.contains(t)) {
                if (OTHER_AUDIO.contains(t)) skip(r, "files of type ." + t + " (not an iPod format)");
                continue;
            }
            if (f.getPath().indexOf('\t') >= 0 || f.getPath().indexOf('\n') >= 0) { skip(r, "files with a tab or a line break in the name"); continue; }
            files.add(f);
            kinds.add(t.equals("m4b") && media == ITunesDb.MEDIA_AUDIO ? ITunesDb.MEDIA_AUDIOBOOK : media);
        }
    }

    private static void skip(Result r, String reason) {
        Integer n = r.skipped.get(reason);
        r.skipped.put(reason, n == null ? 1 : n + 1);
    }

    private static String meta(MediaMetadataRetriever m, int key) {
        String s = m.extractMetadata(key);
        return s == null ? "" : s.trim();
    }

    private static int number(String s) {
        int slash = s.indexOf('/');
        String n = (slash < 0 ? s : s.substring(0, slash)).trim();
        try { return n.isEmpty() ? 0 : Integer.parseInt(n); } catch (NumberFormatException e) { return 0; }
    }

    /** The tags and the cover of f, or null if it cannot be read or has video. index: the track's place in the database. */
    private static ITunesDb.Track read(File f, int media, Result r, CoverArt covers, int index) throws IOException {
        MediaMetadataRetriever m = new MediaMetadataRetriever();
        try {
            m.setDataSource(f.getPath());
            if ("yes".equals(m.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_VIDEO))) {
                skip(r, "videos (not supported)");
                return null;
            }
            ITunesDb.Track t = new ITunesDb.Track();
            String name = f.getName();
            t.title = meta(m, MediaMetadataRetriever.METADATA_KEY_TITLE);
            if (t.title.isEmpty()) t.title = name.substring(0, Math.max(0, name.lastIndexOf('.')));
            t.artist = meta(m, MediaMetadataRetriever.METADATA_KEY_ARTIST);
            if (t.artist.isEmpty()) t.artist = meta(m, MediaMetadataRetriever.METADATA_KEY_ALBUMARTIST);
            t.album = meta(m, MediaMetadataRetriever.METADATA_KEY_ALBUM);
            t.genre = meta(m, MediaMetadataRetriever.METADATA_KEY_GENRE);
            t.trackNumber = number(meta(m, MediaMetadataRetriever.METADATA_KEY_CD_TRACK_NUMBER));
            t.discNumber = number(meta(m, MediaMetadataRetriever.METADATA_KEY_DISC_NUMBER));
            t.year = number(meta(m, MediaMetadataRetriever.METADATA_KEY_YEAR));
            t.lengthMs = number(meta(m, MediaMetadataRetriever.METADATA_KEY_DURATION));
            t.bitrateKbps = number(meta(m, MediaMetadataRetriever.METADATA_KEY_BITRATE)) / 1000;
            t.fileType = type(f).equals("aiff") ? "aif" : type(f);
            t.sampleRate = AudioInfo.sampleRate(f.getPath(), t.fileType);
            t.size = f.length();
            t.mediaType = media;
            String key = CoverArt.albumKey(meta(m, MediaMetadataRetriever.METADATA_KEY_ALBUMARTIST), t.artist, t.album, f);
            t.artworkSize = covers.cover(key, m, f, ITunesDb.dbid(index));
            return t;
        } catch (RuntimeException e) {
            skip(r, "files that Android cannot read");
            return null;
        } finally {
            m.release();
        }
    }

    /** Scan, write the database and the manifest. Throws with a message for the user. */
    static Result build(Progress progress) throws IOException {
        Result r = new Result();
        List<File> files = new ArrayList<File>();
        List<Integer> kinds = new ArrayList<Integer>();
        collect(Storage.music(), ITunesDb.MEDIA_AUDIO, files, kinds, r);
        collect(Storage.podcasts(), ITunesDb.MEDIA_PODCAST, files, kinds, r);
        collect(Storage.audiobooks(), ITunesDb.MEDIA_AUDIOBOOK, files, kinds, r);
        List<ITunesDb.Track> tracks = new ArrayList<ITunesDb.Track>();
        StringBuilder manifest = new StringBuilder();
        CoverArt covers = new CoverArt(Storage.artwork());
        try {
            readAll(files, kinds, tracks, manifest, covers, r, progress);
            covers.finish();                                /* before the iTunesDB, which points to the covers */
        } catch (IOException e) {
            covers.abort();
            throw e;
        } catch (RuntimeException e) {
            covers.abort();
            throw e;
        }
        r.covers = covers.count();
        progress.step("Writing the database");
        for (String d : new String[] { "iPod_Control/iTunes", "iPod_Control/Device", "Contacts", "Calendars", "Notes" }) {
            File dir = new File(Storage.disk(), d);
            if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot make " + dir);
        }
        File sysInfo = new File(Storage.disk(), "iPod_Control/Device/SysInfo");
        if (!sysInfo.exists() && !sysInfo.createNewFile()) throw new IOException("cannot make " + sysInfo);
        writeAtomic(Storage.database(), ITunesDb.build(tracks, "IPOD", System.currentTimeMillis() / 1000));
        writeAtomic(Storage.manifest(), manifest.toString().getBytes("UTF-8"));
        return r;
    }

    private static void readAll(List<File> files, List<Integer> kinds, List<ITunesDb.Track> tracks, StringBuilder manifest,
                                CoverArt covers, Result r, Progress progress) throws IOException {
        for (int i = 0; i < files.size(); i++) {
            File f = files.get(i);
            progress.step("Reading " + (i + 1) + " of " + files.size() + ": " + f.getName());
            ITunesDb.Track t = read(f, kinds.get(i), r, covers, tracks.size());
            if (t == null) continue;
            int n = tracks.size();
            String folder = String.format(Locale.US, "F%02d", n % FOLDERS);
            String name = String.format(Locale.US, "R%05d.%s", n, t.fileType.toUpperCase(Locale.US));
            t.location = ":iPod_Control:Music:" + folder + ":" + name;
            manifest.append("iPod_Control/Music/").append(folder).append('/').append(name).append('\t').append(f.getPath()).append('\n');
            tracks.add(t);
            if (t.mediaType == ITunesDb.MEDIA_PODCAST) r.podcasts++;
            else if (t.mediaType == ITunesDb.MEDIA_AUDIOBOOK) r.audiobooks++;
            else r.songs++;
        }
    }

    static void writeAtomic(File f, byte[] b) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        FileOutputStream out = new FileOutputStream(tmp);   /* no try-with-resources: API 19 */
        try { out.write(b); } finally { out.close(); }
        if (f.exists() && !f.delete()) throw new IOException("cannot replace " + f);
        if (!tmp.renameTo(f)) throw new IOException("cannot write " + f);
    }

    /** The counts of the last build, for the screen, or null */
    static String lastSummary(File summaryFile) {
        java.io.FileInputStream in = null;
        try {
            in = new java.io.FileInputStream(summaryFile);
            byte[] b = new byte[(int) Math.min(summaryFile.length(), 4096)];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8") : null;
        } catch (IOException e) {
            return null;
        } finally {
            AudioInfo.close(in);
        }
    }

    static void saveSummary(File summaryFile, String text) {
        Writer w = null;
        try {
            w = new OutputStreamWriter(new FileOutputStream(summaryFile), "UTF-8");
            w.write(text);
        } catch (IOException ignore) {
        } finally {
            AudioInfo.close(w);
        }
    }
}
