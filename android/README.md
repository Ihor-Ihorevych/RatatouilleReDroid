# Android port

An arm64 Android build of the recompiled game: the SDK's runtime with a Vulkan 1.3 renderer (`sdk/runtime/gpu/vulkan.cpp`,
HLSL to SPIR-V through glslang), AAudio sound, SDL3 for the window and pads, and on-screen touch controls.
Status and known gaps: [`docs/android-vulkan.md`](../docs/android-vulkan.md).

The repository holds no game data. You need your own copy of the game.

## Build (Windows, Git Bash)

1. Build the host tools: `cmake -S tools-build -B tools-build/build -G Ninja && cmake --build tools-build/build`
   (MinGW works; no Visual Studio needed).
2. Extract your disc image so `game_files/default.xbe` exists, and copy `default.xbe` to `assets/`.
3. `bash android/build-apk.sh --recomp` builds `build/android/ratatouille.apk`; the game goes on the phone by hand
   (`adb push game_files /sdcard/Android/data/io.github.ratatouille/files/game`).
4. `bash android/build-apk.sh --selfpack` also builds `ratatouille-selfpack.apk`, which carries the game and unpacks it on the
   first start. It contains data and code derived from the game: keep it to your own phone.

Needs the Android SDK (platform 35, NDK 28.2.13676358, CMake 4.1.2) and a JDK; `--install` installs over adb.

## Settings

The top-left button opens the settings: resolution (25% to 100% of the screen's pixels), picture shape (fill the screen, or 16:9 with side bars; applies on the next start), button opacity, visibility and style (game icons or Xbox A/B/X/Y), and links to the Telegram group and the source. Choices are saved in the app's preferences.
