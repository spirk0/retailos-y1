package org.retailos;

import android.content.Context;
import android.os.Environment;
import java.io.File;

/** The places of the app's files */
final class Storage {
    private Storage() { }

    /** The user's folder: the IPSW, the iPod's disk, the manifest */
    static File root() { return new File(Environment.getExternalStorageDirectory(), "retailos"); }

    /** The iPod's own files: the FAT32 volume of the virtual disk */
    static File disk() { return new File(root(), "disk"); }

    /** The music files of the database, mapped into the disk (VDISK_MANIFEST) */
    static File manifest() { return new File(root(), "manifest.txt"); }

    static File database() { return new File(disk(), "iPod_Control/iTunes/iTunesDB"); }

    /** The covers: ArtworkDB and the .ithmb files (CoverArt) */
    static File artwork() { return new File(disk(), "iPod_Control/Artwork"); }

    /** The media folders: songs, podcasts, audiobooks */
    static File music() { return new File(Environment.getExternalStorageDirectory(), "Music"); }
    static File podcasts() { return new File(Environment.getExternalStorageDirectory(), "Podcasts"); }
    static File audiobooks() { return new File(Environment.getExternalStorageDirectory(), "Audiobooks"); }

    /** From the IPSW: the firmware partition and the firmware that the runner loads */
    static File firmware(Context c) { return new File(c.getFilesDir(), "firmware.bin"); }
    static File osos(Context c) { return new File(c.getFilesDir(), "osos.bin"); }
}
