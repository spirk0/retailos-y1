package org.retailos;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.util.Log;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Locale;

/**
 * Shows the native runner on the screen.
 *
 * The activity starts the bundled runner (assets/runner-native) through su:
 * the runner maps guest memory below mmap_min_addr, so it must run as root.
 * The runner resumes from its snapshot, or boots osos.bin cold, and sends
 * each display present through a named pipe. Until the first frame, the
 * activity shows the boot progress from the runner's status lines. Keys go
 * to the runner's stdin (see poll_input() in native-runner/src/machine.c).
 * The runner writes the audio to its stdout (AUDIO_OUT=-, see
 * native-runner/src/dev_i2s.c): raw PCM, 16 bit, stereo, 44100 Hz. An
 * AudioTrack plays it.
 *
 * Files: osos.bin and firmware.bin from the IPSW (StartActivity), the
 * iPod's disk as the folder /sdcard/retailos/disk with the music through
 * /sdcard/retailos/manifest.txt (MediaLibrary, native-runner/src/vdisk.c).
 */
public class NativeActivity extends Activity implements SurfaceHolder.Callback {

    /* The runner's working folder (RUNNER_TMP): the log, the frame pipe, the
     * snapshot, the written sectors. The app makes it, so it can read the log. */
    private String TMP, LOG, FRAME;
    /* each present of the guest at once (native-runner/src/frameout.c);
     * frame.ppm only shows until the first frame comes through it. Each
     * runner gets a new pipe name: a reader of the pipe of an earlier
     * runner would wait forever. */
    private volatile String fifo;
    private static final int FRAME_MAGIC = 0x31465049;      /* "IPF1" */
    private static final int EVENT_MAGIC = 0x31455049;      /* "IPE1": an event, header only */
    private static final int EVENT_CLICK = 1;               /* the clicker ticked: value = its length in us */
    private static final int EVENT_VOLUME = 2;              /* the codec's volume: left | right << 16, cB down */
    private static final int EVENT_BACKLIGHT = 3;           /* the backlight: 0 = dark, 1-32 = the level */
    private static final int BACKLIGHT_LEVELS = 32;
    /* the frame while the backlight is dark: an LCD without light, nearly black */
    private static final int DARK_LCD = 0xe0000000;
    private static final int SAMPLE_RATE = 44100;
    /* Audio latency. A stream AudioTrack of this Android version starts to
     * play only when its buffer is full, so the buffer size is the latency
     * of the sound. When the buffer is full, write() blocks, the pipe fills,
     * and the runner drops samples: the queue cannot grow past the buffer.
     * The runner sends the samples in bursts (a screen update stops the
     * audio for about 0.1 s). If the buffer runs empty during playback, the
     * app makes a new track with LATENCY_STEP_MS more, up to LATENCY_MAX_MS.
     * A pause of the data longer than PAUSE_MS (a song change, a stop) is not
     * an underrun, and neither is an underrun in the first BOOT_MS after the
     * first data: the boot sends the samples in larger bursts. */
    private static final int LATENCY_MIN_MS = 500, LATENCY_STEP_MS = 250, LATENCY_MAX_MS = 1000, PAUSE_MS = 1500;
    private static final int BOOT_MS = 30000;
    private static final long POLL_MS = 100;

    private SurfaceView surface;
    private Process runner;
    private OutputStream runnerIn;
    private Thread pump, audio;
    private AudioTrack track;
    private volatile boolean running, destroyed, fifoFrames;
    private volatile long keyAt;                            /* uptime of the last key that no frame answered yet */
    private long startedAt;

    /* frame buffers, kept: a new 230 KB array per poll made the garbage
     * collector run all the time, on the CPU core that the runner needs */
    private byte[] lastFrame, readBuf;
    private int[] pixels;
    private Bitmap bitmap;
    private final Paint textPaint = new Paint();
    private final Paint imagePaint = new Paint();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        File work = new File(getFilesDir(), "runner");
        work.mkdirs();
        work.setExecutable(true, false);
        work.setReadable(true, false);
        TMP = work.getPath() + "/";
        LOG = TMP + "runner.log";
        FRAME = TMP + "frame.ppm";
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        setContentView(surface);
        textPaint.setColor(Color.WHITE);
        textPaint.setTextSize(16);
        textPaint.setAntiAlias(true);
        imagePaint.setFilterBitmap(true);
    }

    /* One runner for the life of the activity. Two runners must never run at
     * the same time: they save the same snapshot. The activity ends in
     * onStop (the runner hibernates), so onStart starts a new one. */
    @Override
    protected void onStart() {
        super.onStart();
        if (runner != null) su("pkill -CONT runner-native");
        synchronized (this) { if (track != null) track.play(); }
        running = true;
        registerReceiver(battery, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        pump = new Thread(new Runnable() { public void run() { pumpLoop(); } });
        pump.start();
    }

    /* In the background the iPod sleeps: the runner hibernates at once, and
     * the activity ends. (Android runs the onDestroy() of a finished activity
     * in the background only later: the runner played on for 30 s and more.)
     * The next start of the app shows the start screen, and "Start retailOS"
     * resumes. */
    @Override
    protected void onStop() {
        running = false;
        unregisterReceiver(battery);
        stopRunner();
        super.onStop();
        finish();
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        stopRunner();
        super.onDestroy();
    }

    private static void su(String cmd) {
        try {
            Runtime.getRuntime().exec(new String[] { "su", "-c", cmd }).waitFor();
        } catch (Exception ignore) { }
    }

    /* ---------- the runner process ---------- */

    private static void copyAsset(InputStream in, File dst) throws Exception {
        OutputStream out = new FileOutputStream(dst);
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        out.close();
        in.close();
    }

    private void startRunner() throws Exception {
        /* always the copy in this APK; rename, because the old file can still run */
        File bin = new File(getFilesDir(), "runner-native"), tmp = new File(getFilesDir(), "runner-native.tmp");
        copyAsset(getAssets().open("runner-native"), tmp);
        tmp.setExecutable(true, false);
        if (!tmp.renameTo(bin)) throw new Exception("cannot replace " + bin);
        String osos = Storage.osos(this).getPath();      /* from the IPSW (StartActivity) */
        /* An older runner can still be there, stopped (onStop) after its app
         * was gone. It continues, hibernates on SIGTERM (saves the snapshot)
         * and exits; after 15 s it is killed. The new runner then resumes
         * from the snapshot, or boots cold without one. */
        /* the log belongs to the app, so that it can read it; the runner (root) writes it */
        File log = new File(LOG);
        if (!log.exists()) log.createNewFile();
        log.setReadable(true, false);
        log.setWritable(true, false);
        /* the disk from the folder, the music through the manifest (vdisk.c) */
        String disk = "VDISK=" + Storage.disk() + " VDISK_FIRMWARE=" + Storage.firmware(this)
                + " VDISK_MANIFEST=" + Storage.manifest() + " VDISK_OVERLAY=" + TMP + "disk.overlay";
        String fifoName = TMP + "frames-" + System.currentTimeMillis() + ".fifo";
        fifo = fifoName;
        String cmd = "pkill -CONT runner-native; pkill -TERM runner-native; pkill runner-v2;"
                + " n=0; while pidof runner-native > /dev/null && [ $n -lt 15 ]; do sleep 1; n=$((n+1)); done;"
                + " pkill -9 runner-native;"
                + " while pidof runner-native > /dev/null || pidof runner-v2 > /dev/null; do sleep 1; done;"
                + " cd " + TMP + " && rm -f frame.ppm frame.ppm.tmp frames*.fifo"
                + " && RUNNER_TMP=" + TMP + " FRAMES_FIFO=" + fifoName + " AUDIO_OUT=- HIBERNATE_ON_EOF=1 " + disk + " exec " + bin.getPath()
                + " " + osos + " 2> " + LOG;
        runner = new ProcessBuilder("su", "-c", cmd).start();
        runnerIn = runner.getOutputStream();
        startedAt = System.currentTimeMillis();
        runOnUiThread(new Runnable() { public void run() { powerSent = null; sendPower(); } });
        startAudio(runner.getInputStream());
        Thread frames = new Thread(new Runnable() { public void run() { frameLoop(); } });
        frames.setDaemon(true);
        frames.start();
    }

    /* ---------- the frame stream ---------- */

    /* The iPod's headphone volume (the WM8758 codec of the runner). The
     * firmware does not scale the samples: it sets the volume in the codec.
     * The track's gain applies it at once, also to the samples that wait in
     * the track's buffer. */
    private float gainLeft = 1f, gainRight = 1f;

    private static float gain(int cB) {
        return cB == 0xffff ? 0f : (float) Math.pow(10, -cB / 200.0);
    }

    private synchronized void volume(int value) {
        gainLeft = gain(value & 0xffff);
        gainRight = gain((value >>> 16) & 0xffff);
        if (track != null) track.setStereoVolume(gainLeft, gainRight);
    }

    /* The iPod's backlight (native-runner/src/dev_backlight.c): its level
     * sets the brightness of this window, 1-32 of the full brightness. When
     * the iPod's backlight is off (the backlight timer, sleep), the window
     * gets the lowest brightness and the frame is drawn nearly black, as
     * an LCD without light. The fade of the firmware is 31 steps: the UI
     * thread applies only the newest level. */
    private volatile int backlightLevel = -1;               /* -1: no event yet, the Y1's brightness */
    private volatile boolean backlightPosted;
    /* when the runner (re)starts, the guest time jumps to host time; the
       firmware then expires its backlight timer at once. Ignore "off" for
       a few seconds after a (re)start, as a waking iPod turns the light on. */
    private volatile long fifoOpenedAt;

    private void backlight(int level) {
        if (level == 0 && SystemClock.uptimeMillis() - fifoOpenedAt < 3000) return;
        boolean dark = level == 0, wasDark = backlightLevel == 0;
        backlightLevel = level;
        if (dark != wasDark) {
            synchronized (surface) { if (shown != null && running) drawBitmap(shown); }
        }
        if (backlightPosted) return;
        backlightPosted = true;
        runOnUiThread(new Runnable() { public void run() {
            backlightPosted = false;
            int l = backlightLevel;
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.screenBrightness = l < 0 ? WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
                    : Math.max(0f, Math.min(1f, l / (float) BACKLIGHT_LEVELS));
            getWindow().setAttributes(lp);
        } });
    }

    private static int le16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    private static int le32(byte[] b, int o) { return le16(b, o) | le16(b, o + 2) << 16; }

    /* Read frames.fifo: a 16-byte header (magic, width, height, sequence)
     * and RGB565 pixels per frame. Each frame is drawn when it comes. The
     * log (tag retailos) gives the time from a key to the next frame, and
     * the frames per second. */
    private void frameLoop() {
        byte[] hdr = new byte[16], pix = null;
        Bitmap frame = null;
        long countFrom = SystemClock.uptimeMillis();
        int count = 0;
        while (!destroyed) {
            String path = fifo;
            if (path == null || !new File(path).exists()) {
                try { Thread.sleep(100); } catch (InterruptedException e) { return; }
                continue;
            }
            DataInputStream in = null;
            try {
                in = new DataInputStream(new BufferedInputStream(new FileInputStream(path), 1 << 16));
                fifoOpenedAt = SystemClock.uptimeMillis();
                for (;;) {
                    in.readFully(hdr);
                    if (le32(hdr, 0) == EVENT_MAGIC) {
                        /* the clicker's events (EVENT_CLICK) are not used: the click sounds in the audio */
                        if (le16(hdr, 4) == EVENT_VOLUME) volume(le32(hdr, 8));
                        else if (le16(hdr, 4) == EVENT_BACKLIGHT) backlight(le32(hdr, 8));
                        continue;
                    }
                    if (le32(hdr, 0) != FRAME_MAGIC) throw new IOException("frames.fifo: bad header");
                    int w = le16(hdr, 4), h = le16(hdr, 6);
                    if (pix == null || pix.length != w * h * 2) {
                        pix = new byte[w * h * 2];
                        frame = Bitmap.createBitmap(w, h, Bitmap.Config.RGB_565);
                    }
                    in.readFully(pix);
                    synchronized (surface) {
                        frame.copyPixelsFromBuffer(ByteBuffer.wrap(pix));
                        shown = frame;
                    }
                    fifoFrames = true;
                    if (!bufferSized) sizeBuffer(w, h);
                    if (running) drawBitmap(frame);
                    long now = SystemClock.uptimeMillis(), k = keyAt;
                    if (k != 0) {
                        keyAt = 0;
                        Log.w("retailos", "key to frame " + (now - k) + " ms");
                    }
                    count++;
                    if (now - countFrom >= 5000) {
                        Log.w("retailos", "frames " + count * 1000 / (now - countFrom) + " per s");
                        count = 0;
                        countFrom = now;
                    }
                }
            } catch (IOException e) {                       /* the runner ended, or a new one starts */
                try { if (in != null) in.close(); } catch (IOException ignore) { }
                try { Thread.sleep(100); } catch (InterruptedException ie) { return; }
            }
        }
    }

    /* ---------- audio ---------- */

    /* A new track with a buffer of ms milliseconds, in place of the old one */
    private synchronized AudioTrack newTrack(int ms) {
        if (track != null) track.release();
        int min = AudioTrack.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT);
        track = new AudioTrack(AudioManager.STREAM_MUSIC, SAMPLE_RATE, AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT, Math.max(min, SAMPLE_RATE * 4 / 1000 * ms), AudioTrack.MODE_STREAM);
        track.setStereoVolume(gainLeft, gainRight);
        if (running) track.play();
        return track;
    }

    /* Copy the runner's stdout to an AudioTrack. The runner does not wait for
     * this reader: if it is slow, the runner drops samples. */
    private void startAudio(final InputStream pcm) {
        audio = new Thread(new Runnable() {
            public void run() {
                byte[] buf = new byte[4608];            /* one DMA buffer of the firmware */
                int n, odd = 0, latency = LATENCY_MIN_MS;
                AudioTrack t = newTrack(latency);
                long written = 0, lastData = SystemClock.uptimeMillis(), firstData = 0;
                try {
                    while ((n = pcm.read(buf, odd, buf.length - odd)) > 0) {
                        long now = SystemClock.uptimeMillis();
                        if (firstData == 0) firstData = now;
                        n += odd;
                        odd = n & 3;                    /* write whole frames (4 bytes) only */
                        long queued = written - (t.getPlaybackHeadPosition() & 0xffffffffL);
                        if (t.getPlayState() == AudioTrack.PLAYSTATE_PLAYING && written > 0 && queued <= 0
                                && now - lastData < PAUSE_MS && now - firstData > BOOT_MS && latency < LATENCY_MAX_MS) {
                            latency = Math.min(latency + LATENCY_STEP_MS, LATENCY_MAX_MS);
                            Log.w("retailos", "audio underrun after " + written / SAMPLE_RATE + " s: latency now "
                                    + latency + " ms");
                            t = newTrack(latency);
                            written = 0;
                        }
                        lastData = now;
                        t.write(buf, 0, n - odd);
                        written += (n - odd) / 4;
                        if (odd > 0) System.arraycopy(buf, n - odd, buf, 0, odd);
                    }
                } catch (Exception ignore) { }
            }
        });
        audio.start();
    }

    /* The runner hibernates: it continues (it can be stopped), saves the
     * snapshot on SIGTERM and exits. The next start resumes from it, as a
     * sleeping iPod wakes up. startRunner() waits for the exit. */
    private void stopRunner() {
        su("pkill -CONT runner-native; pkill -TERM runner-native");
        try { if (runnerIn != null) runnerIn.close(); } catch (IOException ignore) { }
        synchronized (this) {
            if (track != null) track.release();
            track = null;
        }
        runner = null;
        runnerIn = null;
    }

    /* ---------- the screen ---------- */

    private String startError;

    private void pumpLoop() {
        synchronized (this) {                               /* an earlier pump thread can still be here */
            if (runner == null && startError == null) {
                try {
                    startRunner();
                } catch (Exception e) {
                    startError = "cannot start the runner: " + e.getMessage();
                }
            }
        }
        String error = startError;
        lastFrame = null;
        while (running) {
            try {
                if (error == null && runnerExited()) error = "The runner stopped.\n" + lastLogProblem();
                if (error != null) drawText(error, null);
                else if (!fifoFrames && !showFrame()) drawText("Booting the iPod firmware...", bootStatus());
                Thread.sleep(POLL_MS);
            } catch (Exception ignore) { }
        }
    }

    /** Read frame.ppm. Returns false if there is no frame yet. */
    private boolean showFrame() throws Exception {
        File f = new File(FRAME);
        if (!f.exists() || f.lastModified() < startedAt - 2000) return lastFrame != null;
        int len = (int) f.length();
        if (readBuf == null || readBuf.length != len) readBuf = new byte[len];
        byte[] data = readBuf;
        FileInputStream in = new FileInputStream(f);
        int got = 0, n;
        while (got < data.length && (n = in.read(data, got, data.length - got)) > 0) got += n;
        in.close();
        if (got != data.length) return lastFrame != null;
        if (lastFrame != null && Arrays.equals(data, lastFrame)) return true;
        /* "P6\n<w> <h>\n255\n" then RGB */
        int pos = 0, nl = 0, dataOff = -1;
        while (pos < data.length && nl < 3) if (data[pos++] == '\n') nl++;
        if (nl < 3) return lastFrame != null;
        dataOff = pos;
        String[] hdr = new String(data, 0, dataOff, "US-ASCII").trim().split("\\s+");
        int w = Integer.parseInt(hdr[1]), h = Integer.parseInt(hdr[2]);
        if (dataOff + 3 * w * h > data.length) return lastFrame != null;
        if (pixels == null || pixels.length != w * h) pixels = new int[w * h];
        int[] px = pixels;
        for (int i = 0, j = dataOff; i < w * h; i++, j += 3)
            px[i] = 0xff000000 | (data[j] & 0xff) << 16 | (data[j + 1] & 0xff) << 8 | (data[j + 2] & 0xff);
        if (bitmap == null || bitmap.getWidth() != w || bitmap.getHeight() != h)
            bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        bitmap.setPixels(px, 0, w, 0, 0, w, h);
        readBuf = lastFrame;                                /* swap: the next read goes to the old one */
        lastFrame = data;
        drawBitmap(bitmap);
        return true;
    }

    /* The surface buffer gets the frame's height (or width) and the view's
     * aspect ratio: the compositor scales it to the screen, not the CPU.
     * drawBitmap() then copies the frame 1:1. Before, the software scaling
     * to 480x360 took CPU time from the runner on the same core. */
    private volatile boolean bufferSized;

    private void sizeBuffer(final int w, final int h) {
        bufferSized = true;
        runOnUiThread(new Runnable() { public void run() {
            int vw = surface.getWidth(), vh = surface.getHeight();
            if (vw <= 0 || vh <= 0) { bufferSized = false; return; }
            if ((long)vw * h > (long)vh * w) surface.getHolder().setFixedSize((h * vw + vh / 2) / vh, h);
            else surface.getHolder().setFixedSize(w, (w * vh + vw / 2) / vw);
        } });
    }

    /* The last frame of the stream. The firmware sends a frame only when the
     * screen changes. After a new buffer size, a canvas can still have the
     * old size for one frame: the frame then showed 1.5 times too large
     * until the next wheel input. surfaceChanged() draws it again. */
    private Bitmap shown;

    @Override public void surfaceCreated(SurfaceHolder holder) { }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        synchronized (surface) {
            if (shown != null && running) drawBitmap(shown);
        }
    }

    /* two threads draw: the frame stream, and the boot text */
    private void drawBitmap(Bitmap bitmap) {
        synchronized (surface) {
            SurfaceHolder sh = surface.getHolder();
            Canvas c = sh.lockCanvas();
            if (c == null) return;
            c.drawColor(Color.BLACK);
            /* the largest rectangle with the frame's aspect ratio */
            int cw = c.getWidth(), ch = c.getHeight(), w = bitmap.getWidth(), h = bitmap.getHeight();
            int dw = cw, dh = cw * h / w;
            if (dh > ch) { dh = ch; dw = ch * w / h; }
            int x = (cw - dw) / 2, y = (ch - dh) / 2;
            c.drawBitmap(bitmap, null, new Rect(x, y, x + dw, y + dh), imagePaint);
            if (backlightLevel == 0) c.drawColor(DARK_LCD);
            sh.unlockCanvasAndPost(c);
        }
    }

    private void drawText(String line1, String line2) {
        synchronized (surface) { drawTextLocked(line1, line2); }
    }

    private void drawTextLocked(String line1, String line2) {
        SurfaceHolder sh = surface.getHolder();
        Canvas c = sh.lockCanvas();
        if (c == null) return;
        c.drawColor(Color.BLACK);
        int y = c.getHeight() / 3;
        String all = line2 == null ? line1 : line1 + "\n" + line2;
        for (String line : all.split("\n")) {
            while (line.length() > 0) {                     /* wrap at the screen edge */
                int n = textPaint.measureText(line) <= c.getWidth() - 20 ? line.length()
                        : Math.max(1, textPaint.breakText(line, true, c.getWidth() - 20, null));
                c.drawText(line, 0, n, 10, y, textPaint);
                line = line.substring(n);
                y += 22;
            }
        }
        sh.unlockCanvasAndPost(c);
    }

    private boolean runnerExited() {
        if (runner == null) return false;
        try {
            runner.exitValue();
            return true;
        } catch (IllegalThreadStateException stillRunning) {
            return false;
        }
    }

    /** The FATAL line of the log, or its last line. */
    private String lastLogProblem() {
        String last = "";
        try {
            RandomAccessFile f = new RandomAccessFile(LOG, "r");
            long len = f.length();
            byte[] tail = new byte[(int) Math.min(len, 16384)];
            f.seek(len - tail.length);
            f.readFully(tail);
            f.close();
            for (String line : new String(tail, "US-ASCII").split("\n")) {
                if (line.startsWith("FATAL")) return line + "\n(log: " + LOG + ")";
                if (line.length() > 0) last = line;
            }
        } catch (Exception ignore) { }
        return last + "\n(log: " + LOG + ")";
    }

    /** "guest 1.2 s, wall 9 s" from the last status line of the log, or null. */
    private String bootStatus() {
        long wall = (System.currentTimeMillis() - startedAt) / 1000;
        String last = null;
        try {
            RandomAccessFile f = new RandomAccessFile(LOG, "r");
            long len = f.length();
            byte[] tail = new byte[(int) Math.min(len, 4096)];
            f.seek(len - tail.length);
            f.readFully(tail);
            f.close();
            for (String line : new String(tail, "US-ASCII").split("\n")) {
                if (line.startsWith("FATAL")) return line;
                if (line.startsWith("st ")) last = line;
            }
        } catch (Exception ignore) { }
        if (last == null) return "wall " + wall + " s";
        int i = last.indexOf("gt=");
        int j = i < 0 ? -1 : last.indexOf("ms", i);
        if (j < 0) return "wall " + wall + " s";
        long gt = Long.parseLong(last.substring(i + 3, j));
        return "guest " + gt / 1000 + "." + gt % 1000 / 100 + " s, wall " + wall + " s";
    }

    /* ---------- keys ---------- */

    /* The wheel of the Y1: media play = wheel up, media pause = wheel
     * down. One byte per detent (wheel_key() characters). Returns the byte,
     * or -1. */
    private static int wheelKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_MEDIA_PLAY: return 'q';           /* anticlockwise: up the list */
            case KeyEvent.KEYCODE_MEDIA_PAUSE: return 'e';          /* clockwise: down the list */
            default: return -1;
        }
    }

    /* The buttons: media previous/next = back/forward, center = select,
     * play/pause = play, back = menu. Returns the press byte of
     * wheel_hold_key() (M/B/P/F/C), or -1. The release byte is the same
     * letter in lower case. */
    private static int buttonKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_MEDIA_PREVIOUS: return 'B';
            case KeyEvent.KEYCODE_MEDIA_NEXT: return 'F';
            case KeyEvent.KEYCODE_DPAD_CENTER: case KeyEvent.KEYCODE_ENTER: return 'C';
            case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE: return 'P';
            case KeyEvent.KEYCODE_BACK: return 'M';
            default: return -1;
        }
    }

    private void send(int b) {
        try {
            if (runnerIn != null) { runnerIn.write(b); runnerIn.flush(); }
        } catch (Exception ignore) { }
    }

    /* The up event of a key of the iPod must not go on: Android sends a
     * media key on as a MEDIA_BUTTON broadcast, and the launcher of the Y1
     * (ButtonRemapper) then injects a DPAD key. That work, for each
     * detent of the wheel, took CPU time from the runner on the same core:
     * a game slowed down while the wheel turned. */
    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (wheelKey(keyCode) >= 0) return true;
        int b = buttonKey(keyCode);
        if (b < 0) return super.onKeyUp(keyCode, event);
        int i = chordIndex(b);
        if (i >= 0) chordUp(i);
        else send(Character.toLowerCase(b));
        return true;
    }

    /* ---------- the battery ----------
     * The Y1's battery goes to the iPod as the message "%NNNs": the level
     * in percent and the state u (no external power), c (charging) or f
     * (power, not charging). The runner sets the PMU's battery channel and
     * the power and charge pins from it (power_set() in dev_i2c.c). */
    private String powerMsg, powerSent;                     /* on the main thread only */

    private final BroadcastReceiver battery = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            if (level < 0 || scale <= 0) return;
            int percent = Math.max(0, Math.min(100, level * 100 / scale));
            boolean plugged = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
            int status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, 0);
            char state = !plugged ? 'u' : status == BatteryManager.BATTERY_STATUS_CHARGING ? 'c' : 'f';
            powerMsg = String.format(Locale.US, "%%%03d%c", percent, state);
            sendPower();
        }
    };

    private void sendPower() {
        String m = powerMsg;
        if (m == null || runnerIn == null || m.equals(powerSent)) return;
        for (int i = 0; i < m.length(); i++) send(m.charAt(i));
        powerSent = m;
    }

    /* ---------- the hold switch: BACK + PLAY/PAUSE ----------
     * The two keys of the chord (MENU 'M' and PLAY/PAUSE 'P') wait up to
     * CHORD_MS for the other key. Both down: the app sends 'H' (toggle the
     * hold switch) and no button. One key alone: its press goes out after
     * CHORD_MS, or at its release. Only these two keys have the delay. */
    private static final int CHORD_MS = 100;
    private static final char[] CHORD = { 'M', 'P' };
    private final Handler handler = new Handler();
    private final boolean[] chordDown = new boolean[2], pressSent = new boolean[2];
    private final Runnable[] pressLater = new Runnable[2];
    private boolean chordUsed;                              /* until both keys are up: their releases send nothing */

    private static int chordIndex(int b) {
        return b == CHORD[0] ? 0 : b == CHORD[1] ? 1 : -1;
    }

    private void chordDown(final int i) {
        chordDown[i] = true;
        int o = 1 - i;
        if (chordDown[o] && !chordUsed) {
            if (pressLater[o] != null) { handler.removeCallbacks(pressLater[o]); pressLater[o] = null; }
            if (pressSent[o]) { send(Character.toLowerCase(CHORD[o])); pressSent[o] = false; }
            chordUsed = true;
            send('H');
            return;
        }
        if (chordUsed) return;
        pressLater[i] = new Runnable() { public void run() {
            pressLater[i] = null;
            pressSent[i] = true;
            send(CHORD[i]);
        } };
        handler.postDelayed(pressLater[i], CHORD_MS);
    }

    private void chordUp(int i) {
        chordDown[i] = false;
        if (pressLater[i] != null) {                        /* a short press: it goes out now */
            handler.removeCallbacks(pressLater[i]);
            pressLater[i] = null;
            send(CHORD[i]);
            send(Character.toLowerCase(CHORD[i]));
        } else if (pressSent[i]) {
            send(Character.toLowerCase(CHORD[i]));
        }
        pressSent[i] = false;
        if (!chordDown[1 - i]) chordUsed = false;
    }

    /* A held button sends its press once: Android repeats the down event
     * while the key is held, and each repeat was a new press for the iPod. */
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        int k = wheelKey(keyCode), b = buttonKey(keyCode);
        if (k < 0 && b < 0) return super.onKeyDown(keyCode, event);
        if (b >= 0 && event.getRepeatCount() > 0) return true;
        if (keyAt == 0) keyAt = SystemClock.uptimeMillis();
        int i = b >= 0 ? chordIndex(b) : -1;
        if (i >= 0) chordDown(i);
        else send(k >= 0 ? k : b);
        return true;
    }
}
