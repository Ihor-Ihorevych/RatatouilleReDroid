package io.github.ratatouille;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Bundle;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;

/** SDL3's activity: it loads libSDL3.so and libmain.so (the game) and runs SDL_main on its own thread. */
public class GameActivity extends SDLActivity {
    /** The resolution the picture is rendered at, as a percent of the screen's own pixels (100 is native). */
    static native void nativeSetResolution(int percent);

    /** The picture fills a wide screen (true) or stays 16:9 with bars (false); the game reads it once, when it starts. */
    static native void nativeSetFill(boolean fill);

    static final String PREFS = "ratatouille";
    static final String RESOLUTION = "resolution";          // percent of the screen's pixels
    static final String FILL = "fill";                      // boolean, default true
    static final String CONTROLS_ALPHA = "controls_alpha";  // percent, default 100
    static final String CONTROLS_LAYOUT = "controls_layout"; // int: 0 game icons, 1 Xbox letters
    static final String CONTROLS_SHOWN = "controls_shown";  // boolean, default true

    /** The fill setting this run of the game started with: what the panel compares against to ask for a restart. */
    static boolean startedFill = true;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // the picture reaches every edge, under a camera cutout too: the controls keep clear of it (TouchControls)
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= 30
                ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        super.onCreate(savedInstanceState);  // loads libmain.so
        // the saved resolution is in force before the game's first frame
        SharedPreferences prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        nativeSetResolution(prefs.getInt(RESOLUTION, 100));
        startedFill = prefs.getBoolean(FILL, true);
        nativeSetFill(startedFill);
        // the on-screen controller goes on top of the game's surface
        mLayout.addView(new TouchControls(this),
                new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** The app is in the background (true) or back in front: the sound stops and starts with it. */
    static native void nativeSetBackground(boolean background);

    @Override
    protected void onPause() {
        try { nativeSetBackground(true); } catch (UnsatisfiedLinkError e) { /* the game's library is not loaded yet */ }
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        try { nativeSetBackground(false); } catch (UnsatisfiedLinkError e) { /* the game's library is not loaded yet */ }
    }
}
