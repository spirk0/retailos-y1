package org.retailos;

import android.content.Context;
import android.graphics.Color;
import android.view.KeyEvent;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

/**
 * A list for the click wheel of the Y1: the wheel (media play = up,
 * media pause = down) moves the selection, the center button or a touch
 * selects.
 */
final class WheelMenu {
    interface Listener { void activate(int index); }

    static final int BLUE = 0xff3d7fd9, TEXT = 0xff202020;

    final LinearLayout view;
    private final Context context;
    private final Listener listener;
    private final List<TextView> items = new ArrayList<TextView>();
    private int selected;

    WheelMenu(Context context, Listener listener) {
        this.context = context;
        this.listener = listener;
        view = new LinearLayout(context);
        view.setOrientation(LinearLayout.VERTICAL);
    }

    void add(String label) {
        final int index = items.size();
        TextView t = new TextView(context);
        t.setText(label);
        t.setTextSize(22);
        t.setPadding(16, 10, 16, 10);
        t.setSingleLine(true);
        t.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { select(index); listener.activate(index); }
        });
        items.add(t);
        view.addView(t);
        select(selected);
    }

    int size() { return items.size(); }

    int selected() { return selected; }

    void select(int i) {
        if (items.isEmpty()) return;
        selected = Math.max(0, Math.min(items.size() - 1, i));
        for (int k = 0; k < items.size(); k++) {
            items.get(k).setBackgroundColor(k == selected ? BLUE : Color.WHITE);
            items.get(k).setTextColor(k == selected ? Color.WHITE : TEXT);
        }
    }

    /** The down event of a key: true if the list used it */
    boolean keyDown(int keyCode, KeyEvent event) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_MEDIA_PLAY: case KeyEvent.KEYCODE_DPAD_UP:
                select(selected - 1);
                return true;
            case KeyEvent.KEYCODE_MEDIA_PAUSE: case KeyEvent.KEYCODE_DPAD_DOWN:
                select(selected + 1);
                return true;
            case KeyEvent.KEYCODE_DPAD_CENTER: case KeyEvent.KEYCODE_ENTER:
                if (event.getRepeatCount() == 0 && !items.isEmpty()) listener.activate(selected);
                return true;
            default:
                return false;
        }
    }

    /** The up event of a key: true if it is a key of the list. The up
     * events of the wheel must not go on (see NativeActivity.onKeyUp). */
    static boolean keyUp(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_MEDIA_PLAY: case KeyEvent.KEYCODE_MEDIA_PAUSE:
            case KeyEvent.KEYCODE_DPAD_UP: case KeyEvent.KEYCODE_DPAD_DOWN:
            case KeyEvent.KEYCODE_DPAD_CENTER: case KeyEvent.KEYCODE_ENTER:
                return true;
            default:
                return false;
        }
    }
}
