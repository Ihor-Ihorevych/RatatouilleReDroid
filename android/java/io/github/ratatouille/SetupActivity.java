package io.github.ratatouille;

import android.app.Activity;
import android.content.Intent;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;

/**
 * The launcher. A selfpack APK carries the game in assets/game (listed in assets/game.list, "size TAB path" per line);
 * the first start unpacks it into the app's files/game, where the native side looks, with a progress bar, then starts
 * the game. A plain APK has no list and goes straight to the game.
 */
public class SetupActivity extends Activity {
    private TextView status;
    private ProgressBar bar;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final AssetManager assets = getAssets();
        final List<String> names = new ArrayList<>();
        final List<Long> sizes = new ArrayList<>();
        long total = 0;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(assets.open("game.list"), "UTF-8"))) {
            for (String line; (line = r.readLine()) != null; ) {
                int tab = line.indexOf('\t');
                if (tab <= 0) continue;
                long size = Long.parseLong(line.substring(0, tab));
                sizes.add(size);
                names.add(line.substring(tab + 1));
                total += size;
            }
        } catch (IOException | NumberFormatException e) {
            names.clear();  // no list: a plain APK
        }
        final File game = new File(getExternalFilesDir(null), "game");
        final String stamp = names.size() + ":" + total;
        final File marker = new File(game, ".unpacked");
        if (names.isEmpty() || stamp.equals(read(marker))) {
            launch();
            return;
        }

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(0xFF15110F);
        int pad = Math.round(32 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        status = new TextView(this);
        status.setTextColor(0xFFEEEEEE);
        status.setTextSize(18);
        status.setGravity(Gravity.CENTER);
        status.setText("Unpacking the game (first start only)");
        box.addView(status, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                          ViewGroup.LayoutParams.WRAP_CONTENT));
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        box.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                       ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(box);

        final long all = total;
        new Thread(() -> {
            try {
                game.mkdirs();  // the free space of a folder that is not there yet reads as zero
                if (game.getUsableSpace() < all + (64L << 20)) throw new IOException("Not enough free space: the game needs "
                    + (all >> 20) + " MB");
                marker.delete();
                byte[] buf = new byte[1 << 20];
                long done = 0;
                for (int i = 0; i < names.size(); i++) {
                    File out = new File(game, names.get(i));
                    out.getParentFile().mkdirs();
                    try (InputStream in = assets.open("game/" + names.get(i), AssetManager.ACCESS_STREAMING);
                         FileOutputStream os = new FileOutputStream(out)) {
                        for (int n; (n = in.read(buf)) > 0; ) {
                            os.write(buf, 0, n);
                            done += n;
                            progress(done, all, i + 1, names.size());
                        }
                    }
                }
                try (FileOutputStream m = new FileOutputStream(marker)) {
                    m.write(stamp.getBytes("UTF-8"));
                }
                runOnUiThread(this::launch);
            } catch (IOException e) {
                runOnUiThread(() -> status.setText("Could not unpack the game: " + e.getMessage()));
            }
        }, "unpack").start();
    }

    private long lastUi;

    private void progress(final long done, final long total, final int file, final int files) {
        long now = System.nanoTime();
        if (now - lastUi < 100_000_000L && done < total) return;
        lastUi = now;
        runOnUiThread(() -> {
            bar.setProgress((int) (done * 1000 / Math.max(total, 1)));
            status.setText("Unpacking the game (first start only)\n" + (done >> 20) + " / " + (total >> 20) + " MB");
        });
    }

    private static String read(File f) {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new java.io.FileInputStream(f), "UTF-8"))) {
            return r.readLine();
        } catch (IOException e) {
            return null;
        }
    }

    private void launch() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }
}
