# Ratatouille Recomp

An unofficial native PC port of the original Xbox version of Ratatouille (2007), made by static recompilation with [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp). The game's x86 code is translated to C++ ahead of time and built into a 64 bit Windows program that draws with Direct3D 12.

> [!IMPORTANT]
> This repository has no game files and no game code. To build it you need your own legally acquired copy of the Xbox game. Please don't post links to game files here.

> [!NOTE]
> A free fan project, not affiliated with or endorsed by Disney, Pixar, THQ or Asobo Studio. Ratatouille is a trademark of Disney/Pixar.

## Status

Fully playable. The game has been finished at 100% completion on PC, with everything working normally. There are no release builds yet.

## Building

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
| `src/` | what the runtime asks of the game, developer input and hooks |
| `sdk/` | a copy of XboxXBERecomp at `0fd0044` with the runtime fixes below |

## Runtime fixes made for this game

Sent upstream as [TGP482/XboxXBERecomp#25](https://github.com/TGP482/XboxXBERecomp/pull/25):

- **Loading screen hang.** The game polls a visibility test with no present in between, and the result stayed pending. The runtime now completes it when it is polled.
- **Saves refused for lack of space.** The game reads the free space's low 32 bits alone, so the 8 GiB the runtime reported read as none. The runtime now reports just under 2 GiB.
- **Remy and pickups in one colour.** Their texture coordinates go through a texture matrix that only wrapping undoes, and the runtime clamped them. It no longer does where the vertex stage changes the coordinates.
- Two debugging aids: `XBE_VERBOSE` writes info and warning lines in release builds, and `XBE_CAPTURE_VS=<handle>` adds one vertex shader's vertices and constants to a frame capture.

## Debugging

- `XBE_VERBOSE=1`: a detailed log in the `logs` folder beside the executable.
- `XBE_CAPTURE_DIR=<folder>`: F3 saves the current frame's draws, textures and shaders there.
- `RAT_DEV=1`: the first controller port stays connected, and a file named `pad_cmd` beside the executable presses buttons on it (see `src/common/dev_input.cpp`).

## Credits

- [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.

## License

This repository's own files are under the MIT license (`LICENSE`). `sdk/` keeps its own MIT license (`sdk/LICENSE`) and third party notices (`sdk/THIRD_PARTY.txt`).
