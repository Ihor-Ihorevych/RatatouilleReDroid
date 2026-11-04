# Ratatouille ReDroid

An Android port of [paulohenriquesn/RatatouilleRecomp](https://github.com/paulohenriquesn/RatatouilleRecomp), the unofficial PC port of the original Xbox version of Ratatouille (2007). The PC build and everything below it still work as before; this fork adds an arm64 Android build.

## Ratatouille Recomp

An unofficial native PC port of the original Xbox version of Ratatouille (2007), made by static recompilation with [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp). The game's x86 code is translated to C++ ahead of time and built into a 64 bit Windows program that draws with Direct3D 12.

> [!IMPORTANT]
> This repository has no game files and no game code. To build it you need your own legally acquired copy of the Xbox game. Please don't post links to game files here.

> [!NOTE]
> A free fan project, not affiliated with or endorsed by Disney, Pixar, THQ or Asobo Studio. Ratatouille is a trademark of Disney/Pixar.

## Status

- **PC:** fully playable, finished at 100% completion, as upstream reports. No release builds yet.
- **Android:** boots and plays on a Mali-G615 phone (Android 16), about 100 fps in menus and about 40 to 100 fps in levels at native resolution. Rendering has known gaps (point sprites, CPU-written render targets, occlusion tests); the list is in [`docs/android-vulkan.md`](docs/android-vulkan.md). Tested on one phone only.

There are no release builds: an APK contains code translated from the game, so everyone builds their own from their own copy.

## Android

- **Renderer:** Vulkan 1.3 (`sdk/runtime/gpu/vulkan.cpp`). The game's shaders are the same HLSL the Direct3D 12 renderer uses, compiled to SPIR-V at run time with glslang. A Vulkan 1.3 GPU is required.
- **Resolution and screen shape:** drawn at the screen's own pixels, with a slider (25% to 100%) behind the top-left Settings button. The picture is real widescreen: the game is told the screen's aspect ratio (16:9 up to about 21:9), so the 3D view is wider rather than stretched. Menus and 2D art still stretch sideways. A `display.txt` in the app's files folder containing `fit` keeps 16:9 with side bars.
- **Controls:** on-screen buttons, one per action the game binds (Jump, Action for attack and use, HUD, Sprint, Sniff, Select, Start, camera reset), with icons drawn from SVG paths. There are no sticks until you touch: the left half of the screen moves, the right half turns the camera. A physical gamepad hides the overlay.
- **Sound:** AAudio.

### Build (Windows, Git Bash)

You need your own copy of the game, the Android SDK (platform 35, NDK 28.2.13676358, CMake 4.1.2) and a JDK. More detail: [`android/README.md`](android/README.md).

1. `cmake -S tools-build -B tools-build/build -G Ninja && cmake --build tools-build/build` builds the recompiler (MinGW is enough).
2. Extract your disc image to `game_files/` and copy `default.xbe` into `assets/`.
3. `bash android/build-apk.sh --recomp` writes `build/android/ratatouille.apk` (about 10 MB, no game data).
4. `bash android/build-apk.sh --selfpack` also writes `ratatouille-selfpack.apk` (about 1.1 GB), which carries the game and unpacks it on the first start. Add `--install` to install over adb.

### The light APK

The light APK has no game data. On the first start it asks you to select the folder with the game's files (the one holding `default.xbe`, extracted from your disc image) and copies them into the app once. Pick a subfolder of Downloads or Documents: Android refuses the root of storage or Downloads itself.

CI (`.github/workflows/ci.yml`) cannot build an APK without the game. It builds the host tools, checks that every generated shader compiles to SPIR-V, compile-checks the arm64 runtime sources and builds the Java and resources.

## Building for PC

1. Install Visual Studio 2022 with **Desktop development with C++**, **C++ Clang tools for Windows** and **C++ CMake tools for Windows** (`build.bat` offers to add the last two).
2. Clone this repository.
3. Put the game's files in the `assets` folder: `default.xbe`, with the game's folders beside it, from your own disc.
4. Run `build.bat`. The first build recompiles the game's code and takes a while. The executable is written to `out\build\release\Ratatouille.exe`.

What the build makes from your copy of the game (`generated` and `out`) is the game's code: keep it to yourself, out of any repository.

## What is here

| Path | What it is |
|---|---|
| `config/game.toml` | the recompiler's input for `default.xbe`, written by `xbe_symbols`: code sections, entry point, and the XDK functions and data the runtime replaces |
| `config/manual.toml` | entries found by hand: two functions the recompiler missed, and `D3DDevice_GetVisibilityTestResult`, which the XDK 5849 signatures don't find |
| `src/` | what the runtime asks of the game, developer input and hooks (including the Android widescreen hook) |
| `sdk/` | a copy of XboxXBERecomp at `0fd0044` with the runtime fixes below, plus the Android additions (Vulkan renderer, arm64 and logcat fixes) |
| `android/` | the Android app: SDL activity, launcher, touch controls, AAudio, build script |
| `tools-build/` | builds the recompiler and a shader check with MinGW or Linux, no Visual Studio needed |
| `docs/` | notes on the Vulkan backend |

## Runtime fixes made for this game

Sent upstream as [TGP482/XboxXBERecomp#25](https://github.com/TGP482/XboxXBERecomp/pull/25):

- **Loading screen hang.** The game polls a visibility test with no present in between, and the result stayed pending. The runtime now completes it when it is polled.
- **Saves refused for lack of space.** The game reads the free space's low 32 bits alone, so the 8 GiB the runtime reported read as none. The runtime now reports just under 2 GiB.
- **Remy and pickups in one colour.** Their texture coordinates go through a texture matrix that only wrapping undoes, and the runtime clamped them. It no longer does where the vertex stage changes the coordinates.
- Two debugging aids: `XBE_VERBOSE` writes info and warning lines in release builds, and `XBE_CAPTURE_VS=<handle>` adds one vertex shader's vertices and constants to a frame capture.

## Debugging

- `XBE_VERBOSE=1`: a detailed log in the `logs` folder beside the executable. On Android the log goes to logcat (tag `ratatouille`) and to `logs` in the app's files folder.
- `XBE_CAPTURE_DIR=<folder>`: F3 saves the current frame's draws, textures and shaders there (Direct3D 12 renderer).
- `RAT_DEV=1`: the first controller port stays connected, and a file named `pad_cmd` beside the executable presses buttons on it (see `src/common/dev_input.cpp`).

## Credits

- [paulohenriquesn/RatatouilleRecomp](https://github.com/paulohenriquesn/RatatouilleRecomp), the PC port this fork starts from.
- [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [glslang](https://github.com/KhronosGroup/glslang) for compiling the shaders to SPIR-V on Android.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.

## License

This repository's own files are under the MIT license (`LICENSE`). `sdk/` keeps its own MIT license (`sdk/LICENSE`) and third party notices (`sdk/THIRD_PARTY.txt`).
