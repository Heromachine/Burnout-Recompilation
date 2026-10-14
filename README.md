# Burnout Recompilation

**Version 0.1.0**: an early, incomplete release, for Windows only.

A launcher that turns **your own copy** of the original Xbox game *Burnout* (Criterion Games /
Acclaim, 2002) into a native Windows build. It uses a fork of
[xboxrecomp](https://github.com/Heromachine/xboxrecomp) to recompile the game's code into C ahead
of time, instead of emulating it one instruction at a time.

**No game data is included.** The first time you start it, it asks for your Burnout (USA) disc
image and builds the game from that image on your own computer.

> ### ⚠️ This is early and incomplete
>
> You can play races, but the game isn't finished. Known issues in 0.1.0:
>
> - After a computer-controlled racer crashes, a crash "scrape" sound sometimes keeps playing on
>   a loop. It stops at the next crash, and at the end of the race.
> - Lighting on some scenery and car parts may not match the original exactly.
>
> It has been tested on a small number of PCs. If something goes wrong, the logs (see below) are
> the most useful thing to include in a report.

## What you need

- Windows 10 or 11, 64-bit
- A graphics card with Direct3D 11
- Your own Burnout (USA) Xbox disc image (`.iso`)
- About 6 GB of free disk space for the one-time build
- An Xbox-style controller (recommended)

## Running it

1. Download `Burnout-Recompilation-v0.1.0-windows-x64.zip` from [Releases](../../releases) and
   extract it. Keep the whole folder together, because `Burnout.exe` needs the `tools` folder
   beside it. The one-time build runs faster from a local disk than from a network share.
2. Run `Burnout.exe`. If Windows warns that it is from an unknown publisher, choose
   **More info**, then **Run anyway**.
3. The first time only, choose your disc image and press **Continue**. The launcher builds the
   game from it. This takes a few minutes, and it happens again once after you update to a new
   release.
4. Press **Play**.

### Options

- **Brightness**: *Original*, in the middle, is the game's own picture. Move left for darker,
  right to lighten the dark parts.
- **Resolution**: 1x is the Xbox's own 640 × 480. 2x (the default) is 1280 × 960, and 3x is
  1920 × 1440. Higher is sharper but needs a faster graphics card.
- **Run the graphics chip on its own thread**: on by default, and much faster. Turn it off only
  if the game hangs or draws wrongly.
- **Full screen**: fills the screen, with black bars on a widescreen monitor so the 4:3 picture
  keeps its shape. F11 or Alt+Enter switches between full screen and a window at any time,
  including in game.
- **Saved games folder**: where your saves are kept. Choosing a new folder doesn't move saves
  already made.

The menus work with mouse, keyboard or controller (D-pad or left stick to move, A to choose,
B to go back). On a laptop with two graphics chips, the launcher asks Windows for the faster one.

### Where things are kept

```
%LOCALAPPDATA%\BurnoutRecomp\
    settings.ini   the disc image you chose, and your Options
    work\          the extracted game and the build (delete it to rebuild)
    logs\          launcher, build and game logs; include the newest with any bug report
    saves\         saved games, unless you choose another folder
```

## Building from source

The release is assembled on Linux and cross-compiled for Windows with Zig.

- `launcher-win/` holds the Windows launcher (Dear ImGui on Direct3D 11).
  - `fetch_toolchain.sh` downloads the embeddable Python, Zig, CMake/Ninja and Capstone.
  - `build.sh` builds `Burnout.exe`.
  - `make_release.sh` assembles the release folder:
    `OUT=... XBOXRECOMP=/path/to/xboxrecomp ./launcher-win/make_release.sh`.
- `tools/build_from_iso.py` is the pipeline the launcher runs on the player's machine. It
  extracts the ISO, recompiles the XBE with xboxrecomp (`tools/regenerate.py`, which uses the
  seed lists in `config/`), applies `tools/apply_spin_fixes.py`, and compiles
  `burnout_game.dll`. On Linux, `regenerate.sh` is the wrapper.
- `src/` holds the host entry point, plus manual overrides that replace the generated code for
  specific functions.
- `config/source-manifest.json` records the xboxrecomp commit this release is pinned to.
  The release zip also includes that toolkit's source, under `tools/toolkit`.

Nothing game-derived belongs in this repository (see `.gitignore`). The paths in the manifest
and the build scripts are the author's machine; set your own.

## Credits and licences

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet, MIT
- [xemu](https://github.com/xemu-project/xemu), GPL-2.0-or-later. xboxrecomp's APU and NV2A
  libraries are derived from it, and it was the reference for how the hardware behaves.
- [Dear ImGui](https://github.com/ocornut/imgui), MIT
- [Capstone](https://www.capstone-engine.org/), [Python](https://www.python.org/),
  [Zig](https://ziglang.org/), [CMake](https://cmake.org/) and [Ninja](https://ninja-build.org/),
  bundled for the one-time build under their own licences

*Burnout* is © Criterion Games / Electronic Arts. This project is not affiliated with or endorsed
by Criterion Games, Electronic Arts, Acclaim or Microsoft.
