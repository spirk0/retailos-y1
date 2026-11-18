package org.retailos;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;

/**
 * The start screen: "Build database" and "Start retailOS". At the first start
 * it takes the firmware from the IPSW in /sdcard/retailos and makes sure
 * that root works (the runner needs it). The wheel moves the selection,
 * the center button (or a touch) selects (WheelMenu).
 *
 * It is also a home screen (AndroidManifest.xml). When the user selects it
 * as the default home screen, the Y1 starts into the iPod: the first
 * HOME start of the app process starts the iPod when the firmware, root
 * and the database are ready. HOME later comes back to this screen.
 */
public class StartActivity extends Activity {
    private static final int TEXT = WheelMenu.TEXT;
    private static final int BUILD = 0, START = 1;
    /* once per app process: the start of the Y1, not each HOME */
    private static boolean homeStartDone;
    private volatile boolean startWhenReady;
    private WheelMenu menu;
    private TextView status;
    private volatile boolean busy;
    private volatile boolean ipswMissing;
    private volatile String ipswState = "", rootState = "", dbState = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);
        TextView title = new TextView(this);
        title.setText("retailOS");
        title.setTextColor(TEXT);
        title.setTextSize(20);
        title.setGravity(Gravity.CENTER);
        title.setBackgroundColor(0xffd8dde3);
        title.setPadding(0, 6, 0, 6);
        root.addView(title);
        menu = new WheelMenu(this, new WheelMenu.Listener() {
            public void activate(int index) { StartActivity.this.activate(index); }
        });
        menu.add("Build database");
        menu.add("Start retailOS");
        menu.select(START);
        root.addView(menu.view);
        status = new TextView(this);
        status.setTextColor(TEXT);
        status.setTextSize(14);
        status.setPadding(16, 16, 16, 8);
        root.addView(status);
        setContentView(root);
        String db = MediaLibrary.lastSummary(summaryFile());
        dbState = db == null ? "Database: not built yet" : "Database: " + db;
        Intent i = getIntent();
        if (!homeStartDone && i != null && i.hasCategory(Intent.CATEGORY_HOME)) {
            homeStartDone = true;
            startWhenReady = true;                          /* setup() starts the iPod */
        }
        setup();
    }

    private File summaryFile() { return new File(getFilesDir(), "library.txt"); }

    private void show() {
        runOnUiThread(new Runnable() { public void run() {
            status.setText(ipswState + "\n" + rootState + "\n" + dbState);
        } });
    }

    /** the firmware from the IPSW, and the root check: on a worker thread */
    private void setup() {
        busy = true;
        ipswState = "Firmware: looking for an IPSW...";
        show();
        new Thread(new Runnable() { public void run() {
            /* the sdcard can take a few seconds to mount after boot: retry */
            File ipsw = null;
            for (int i = 0; i < 20 && ipsw == null; i++) {
                ipsw = IpswSetup.find(Storage.root());
                if (ipsw == null) {
                    try { Thread.sleep(500); } catch (InterruptedException e) { break; }
                }
            }
            File stamp = new File(getFilesDir(), "ipsw.stamp");
            ipswMissing = ipsw == null && !firmwareReady();
            if (ipsw == null && firmwareReady()) {
                /* the firmware is already extracted: the IPSW is not needed anymore */
                ipswState = "Firmware: ready";
            } else if (ipswMissing) {
                ipswState = "Firmware: no IPSW. Copy iPod_25.1.3.ipsw to " + Storage.root();
            } else if (IpswSetup.ready(ipsw, Storage.firmware(StartActivity.this), Storage.osos(StartActivity.this), stamp)) {
                ipswState = "Firmware: " + ipsw.getName();
            } else {
                ipswState = "Firmware: reading " + ipsw.getName() + "...";
                show();
                try {
                    IpswSetup.extract(ipsw, Storage.firmware(StartActivity.this), Storage.osos(StartActivity.this), stamp);
                    ipswState = "Firmware: " + ipsw.getName();
                } catch (Exception e) {
                    ipswState = "Firmware: " + e.getMessage();
                }
            }
            show();
            boolean root = hasRoot();
            rootState = root ? "Root: available" : "Root: not available. This app needs a rooted Y1 (su).";
            busy = false;
            show();
            if (startWhenReady && root && firmwareReady() && Storage.database().isFile()) {
                runOnUiThread(new Runnable() { public void run() {
                    startActivity(new Intent(StartActivity.this, NativeActivity.class));
                } });
            }
            startWhenReady = false;
        } }).start();
    }

    private static boolean hasRoot() {
        try {
            Process p = Runtime.getRuntime().exec(new String[] { "su", "-c", "id" });
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line = r.readLine();
            p.waitFor();
            return line != null && line.contains("uid=0");
        } catch (Exception e) {
            return false;
        }
    }

    private boolean firmwareReady() {
        return Storage.firmware(this).isFile() && Storage.osos(this).isFile();
    }

    private void activate(int item) {
        if (busy) return;
        if (item == BUILD) buildDatabase();
        else if (!firmwareReady()) show();                 /* the firmware line says what is missing */
        else startActivity(new Intent(this, NativeActivity.class));
    }

    /* HOME while the start screen shows (this app is the home screen) */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        menu.select(START);
    }

    /* the sdcard can mount long after the app started: while the IPSW is
       missing, check again every 2 s, so a late mount clears the message */
    private final Handler handler = new Handler();
    private final Runnable recheck = new Runnable() {
        public void run() {
            Log.d("retailos", "recheck: ipswMissing=" + ipswMissing + " busy=" + busy
                    + " root=" + Storage.root());
            if (ipswMissing && !busy) setup();
            handler.postDelayed(this, 2000);
        }
    };

    @Override
    protected void onResume() {
        super.onResume();
        handler.removeCallbacks(recheck);
        handler.postDelayed(recheck, 2000);
    }

    @Override
    protected void onPause() {
        super.onPause();
        handler.removeCallbacks(recheck);
    }

    /** true if this app is the default home screen */
    private boolean isHome() {
        Intent home = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME);
        android.content.pm.ResolveInfo r = getPackageManager().resolveActivity(home, 0);
        return r != null && r.activityInfo != null && getPackageName().equals(r.activityInfo.packageName);
    }

    /* BACK (the iPod's MENU) does not close the home screen: there is nothing behind it */
    @Override
    public void onBackPressed() {
        if (!isHome()) super.onBackPressed();
    }

    private void buildDatabase() {
        busy = true;
        dbState = "Database: scanning " + Storage.music() + ", " + Storage.podcasts() + ", " + Storage.audiobooks();
        show();
        new Thread(new Runnable() { public void run() {
            try {
                MediaLibrary.Result r = MediaLibrary.build(new MediaLibrary.Progress() {
                    public void step(String text) { dbState = "Database: " + text; show(); }
                });
                String summary = r.summary();
                MediaLibrary.saveSummary(summaryFile(), summary);
                dbState = "Database: " + summary;
            } catch (Throwable e) {                         /* also an Error: the screen says it */
                dbState = "Database: failed: " + e.getMessage();
            }
            busy = false;
            show();
        } }).start();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        return menu.keyDown(keyCode, event) || super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return WheelMenu.keyUp(keyCode) || super.onKeyUp(keyCode, event);
    }
}
