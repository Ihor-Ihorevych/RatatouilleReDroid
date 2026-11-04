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

    static final String PREFS = "ratatouille";
    static final String RESOLUTION = "resolution";

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
        // the on-screen controller goes on top of the game's surface
        mLayout.addView(new TouchControls(this),
                new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }
}
