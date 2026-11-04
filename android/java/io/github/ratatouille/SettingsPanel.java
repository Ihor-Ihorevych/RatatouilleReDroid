package io.github.ratatouille;

import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

/**
 * The settings: a dark card over the dimmed game, two columns so that it fits a phone held sideways. On the left the
 * emblem and the links, on the right the settings in sections: graphics (resolution), screen (fill or fit) and the
 * on-screen controls (opacity, shown). Everything is saved as it is changed.
 */
final class SettingsPanel {
    static final String TELEGRAM = "https://t.me/flipendodroid";
    static final String SOURCE = "https://github.com/Ihor-Ihorevych/RatatouilleReDroid";

    private static final int ACCENT = 0xFFE58A2E, CARD = 0xFF1C1814, SECTION = 0xFF262019, FIELD = 0xFF2E2822,
        TEXT = 0xFFF2EDE6, DIM = 0xFF9A8F84;
    private static final int MIN_PERCENT = 25, STEP = 5, MAX_PERCENT = 100;

    private final Context ctx;
    private final TouchControls owner;
    private final SharedPreferences prefs;
    private final float density;
    private final List<TextView> chips = new ArrayList<>();
    private TextView resolutionLabel;
    private FancySlider resolution;
    private TextView restartNote;
    private Dialog dialog;

    SettingsPanel(Context ctx, TouchControls owner) {
        this.ctx = ctx;
        this.owner = owner;
        this.prefs = ctx.getSharedPreferences(GameActivity.PREFS, Context.MODE_PRIVATE);
        this.density = ctx.getResources().getDisplayMetrics().density;
    }

    private int dp(float v) {
        return Math.round(v * density);
    }

    // ---- building -----------------------------------------------------------------------------------------------

    void show() {
        dialog = new Dialog(ctx, android.R.style.Theme_DeviceDefault_Dialog_NoActionBar);
        Window window = dialog.getWindow();
        window.setBackgroundDrawable(new ColorDrawable(0xB0000000));
        window.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = window.getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            window.setAttributes(lp);
        }
        window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);

        FrameLayout root = new FrameLayout(ctx);
        root.setOnClickListener(v -> dialog.dismiss());  // a tap outside the card closes it
        final int cardWidth = Math.min(Math.round(owner.getWidth() * 0.9f), dp(900));
        final int cardHeight = Math.min(dp(430), Math.round(owner.getHeight() * 0.94f));
        LinearLayout card = new LinearLayout(ctx);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setBackground(round(CARD, 22));
        card.setClickable(true);
        card.addView(leftColumn(), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 3f));
        card.addView(rightColumn(), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 7f));
        FrameLayout.LayoutParams cp = new FrameLayout.LayoutParams(cardWidth, cardHeight, Gravity.CENTER);
        root.addView(card, cp);
        dialog.setContentView(root);
        dialog.show();
    }

    private View leftColumn() {
        LinearLayout col = new LinearLayout(ctx);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        col.setPadding(dp(18), dp(18), dp(12), dp(18));
        col.addView(new IconView(ctx), new LinearLayout.LayoutParams(dp(84), dp(84)));
        TextView title = text("Settings", 22, TEXT, true);
        title.setGravity(Gravity.CENTER);
        col.addView(title, margin(wrap(), 0, 12, 0, 0));
        TextView subtitle = text("Ratatouille for Android", 13, DIM, false);
        subtitle.setGravity(Gravity.CENTER);
        col.addView(subtitle, margin(wrap(), 0, 2, 0, 0));
        View spacer = new View(ctx);
        col.addView(spacer, new LinearLayout.LayoutParams(0, 0, 1f));
        col.addView(link(Icons.TELEGRAM, "Telegram group", TELEGRAM), margin(match(), 0, 0, 0, 8));
        col.addView(link(Icons.CODE, "Source code", SOURCE), match());
        return col;
    }

    private View rightColumn() {
        ScrollView scroll = new ScrollView(ctx);
        scroll.setVerticalScrollBarEnabled(false);
        LinearLayout col = new LinearLayout(ctx);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(6), dp(14), dp(18), dp(14));

        // the close button, top right
        LinearLayout head = new LinearLayout(ctx);
        head.setGravity(Gravity.END);
        FrameLayout close = new FrameLayout(ctx);
        close.setBackground(round(FIELD, 18));
        IconView x = new IconView(ctx, Icons.CLOSE, TEXT);
        close.addView(x, new FrameLayout.LayoutParams(dp(18), dp(18), Gravity.CENTER));
        close.setOnClickListener(v -> dialog.dismiss());
        head.addView(close, new LinearLayout.LayoutParams(dp(36), dp(36)));
        col.addView(head, margin(match(), 0, 0, 0, 6));

        col.addView(graphicsSection(), margin(match(), 0, 0, 0, 10));
        col.addView(screenSection(), margin(match(), 0, 0, 0, 10));
        col.addView(controlsSection(), match());
        scroll.addView(col, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return scroll;
    }

    private LinearLayout section(Glyph icon, String title) {
        LinearLayout box = new LinearLayout(ctx);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(round(SECTION, 16));
        box.setPadding(dp(16), dp(12), dp(16), dp(14));
        LinearLayout head = new LinearLayout(ctx);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.addView(new IconView(ctx, icon, ACCENT), new LinearLayout.LayoutParams(dp(20), dp(20)));
        head.addView(text(title, 15, ACCENT, true), margin(wrap(), 8, 0, 0, 0));
        box.addView(head, margin(match(), 0, 0, 0, 6));
        return box;
    }

    // graphics: the resolution
    private View graphicsSection() {
        LinearLayout box = section(Icons.GRAPHICS, "Graphics");
        int current = prefs.getInt(GameActivity.RESOLUTION, 100);
        resolutionLabel = text("", 14, TEXT, false);
        box.addView(resolutionLabel);
        resolution = new FancySlider(ctx, MIN_PERCENT, MAX_PERCENT, STEP);
        resolution.setValue(current);
        resolution.setListener(new FancySlider.Listener() {
            @Override
            public void onChange(int value) {
                updateResolution(value);
            }

            @Override
            public void onStop(int value) {
                commitResolution(value);  // the targets are made again once, when the finger lifts
            }
        });
        box.addView(resolution, match());

        LinearLayout row = new LinearLayout(ctx);
        for (final int percent : new int[] {50, 75, 100}) {
            final TextView chip = text(percent + "%", 14, TEXT, true);
            chip.setGravity(Gravity.CENTER);
            chip.setPadding(dp(18), dp(8), dp(18), dp(8));
            chip.setOnClickListener(v -> {
                resolution.setValue(percent);
                commitResolution(percent);
            });
            chip.setTag(percent);
            chips.add(chip);
            row.addView(chip, margin(wrap(), 0, 0, 8, 0));
        }
        box.addView(row);
        box.addView(text("A lower resolution is faster and a little softer.", 12, DIM, false), margin(wrap(), 0, 8, 0, 0));
        updateResolution(current);
        return box;
    }

    private void updateResolution(int percent) {
        resolutionLabel.setText("Resolution   " + percent + "%   "
            + Math.round(owner.getWidth() * percent / 100f) + " x " + Math.round(owner.getHeight() * percent / 100f));
        for (TextView chip : chips) {
            boolean on = (Integer) chip.getTag() == percent;
            chip.setBackground(round(on ? ACCENT : FIELD, 18));
            chip.setTextColor(on ? 0xFF1C1814 : 0xFFD8D0C6);
        }
    }

    private void commitResolution(int percent) {
        updateResolution(percent);
        prefs.edit().putInt(GameActivity.RESOLUTION, percent).apply();
        GameActivity.nativeSetResolution(percent);
    }

    // screen: fill or fit
    private View screenSection() {
        LinearLayout box = section(Icons.SCREEN, "Screen");
        box.addView(text("Picture shape", 14, TEXT, false));
        final Segmented mode = new Segmented(ctx, "Fill the screen", "16:9 with bars");
        mode.setSelected(prefs.getBoolean(GameActivity.FILL, true) ? 0 : 1);
        restartNote = text("Restart the game to apply this.", 12, ACCENT, true);
        restartNote.setVisibility(View.GONE);
        mode.setListener(index -> {
            boolean fill = index == 0;
            prefs.edit().putBoolean(GameActivity.FILL, fill).apply();
            GameActivity.nativeSetFill(fill);
            // the game's own aspect was set when it started, so only the next start shows the change
            restartNote.setVisibility(fill == GameActivity.startedFill ? View.GONE : View.VISIBLE);
        });
        box.addView(mode, margin(wrap(), 0, 6, 0, 0));
        box.addView(text("Fill widens the picture to your screen, up to 21:9. Menus stretch with it.", 12, DIM, false),
                    margin(wrap(), 0, 8, 0, 0));
        box.addView(restartNote, margin(wrap(), 0, 4, 0, 0));
        return box;
    }

    // controls: opacity and shown
    private View controlsSection() {
        LinearLayout box = section(Icons.CONTROLS, "On-screen controls");
        final TextView label = text("", 14, TEXT, false);
        final int alpha = prefs.getInt(GameActivity.CONTROLS_ALPHA, 100);
        label.setText("Button opacity   " + alpha + "%");
        box.addView(label);
        FancySlider opacity = new FancySlider(ctx, 25, 100, 5);
        opacity.setValue(alpha);
        opacity.setListener(new FancySlider.Listener() {
            @Override
            public void onChange(int value) {
                label.setText("Button opacity   " + value + "%");
                prefs.edit().putInt(GameActivity.CONTROLS_ALPHA, value).apply();
                owner.applyPrefs();
            }

            @Override
            public void onStop(int value) {}
        });
        box.addView(opacity, match());
        box.addView(text("Button style", 14, TEXT, false), margin(wrap(), 0, 4, 0, 0));
        Segmented style = new Segmented(ctx, "Game icons", "Xbox buttons");
        style.setSelected(prefs.getInt(GameActivity.CONTROLS_LAYOUT, 0) == 1 ? 1 : 0);
        style.setListener(index -> {
            prefs.edit().putInt(GameActivity.CONTROLS_LAYOUT, index).apply();
            owner.applyPrefs();
        });
        box.addView(style, margin(wrap(), 0, 6, 0, 8));
        box.addView(text("Show the buttons", 14, TEXT, false), margin(wrap(), 0, 4, 0, 0));
        Segmented shown = new Segmented(ctx, "Shown", "Hidden");
        shown.setSelected(prefs.getBoolean(GameActivity.CONTROLS_SHOWN, true) ? 0 : 1);
        shown.setListener(index -> {
            prefs.edit().putBoolean(GameActivity.CONTROLS_SHOWN, index == 0).apply();
            owner.applyPrefs();
        });
        box.addView(shown, margin(wrap(), 0, 6, 0, 0));
        box.addView(text("Hidden is for a gamepad the phone does not report. The settings button stays.", 12, DIM, false),
                    margin(wrap(), 0, 8, 0, 0));
        return box;
    }

    // ---- helpers ------------------------------------------------------------------------------------------------

    private View link(Glyph icon, String label, final String url) {
        LinearLayout row = new LinearLayout(ctx);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setBackground(round(FIELD, 14));
        row.setPadding(dp(12), dp(10), dp(12), dp(10));
        row.addView(new IconView(ctx, icon, ACCENT), new LinearLayout.LayoutParams(dp(22), dp(22)));
        row.addView(text(label, 14, TEXT, true), margin(wrap(), 10, 0, 0, 0));
        row.setOnClickListener(v -> open(url));
        return row;
    }

    private void open(String url) {
        try {
            ctx.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } catch (RuntimeException e) {
            Toast.makeText(ctx, "No app can open " + url, Toast.LENGTH_LONG).show();
        }
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(ctx);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.getPaint().setFakeBoldText(bold);
        return t;
    }

    private GradientDrawable round(int color, float radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radiusDp));
        return d;
    }

    private static LinearLayout.LayoutParams match() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams margin(LinearLayout.LayoutParams p, float l, float t, float r, float b) {
        p.setMargins(dp(l), dp(t), dp(r), dp(b));
        return p;
    }
}
