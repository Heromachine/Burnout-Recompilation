#!/usr/bin/env python3
"""Build a playable Burnout from the player's own disc image.

The launcher runs this once, on first launch. Every step runs on the user's
machine against the user's image; nothing game-derived ships with the tool.
It runs on Windows (the Windows launcher, with the Python and zig shipped
beside it) and on Linux (building the standalone exe for Wine, for testing).

  1. extract    XISO image -> <work>/game_files            (xiso_extract.py)
  2. stage      the toolkit and project sources -> <work>   (writable copies:
                the lift writes intermediates beside its own tools)
  3. lift       tools/regenerate.py: checks the XBE is the supported one, then
                xbe_parser, disasm with the seeds, func_id, abi_analysis,
                recomp -> <work>/project/src/recomp/gen
  4. fix        tools/apply_spin_fixes.py
  5. compile    CMake + Ninja + zig cc (-target x86_64-windows-gnu)
                -> <work>/build/burnout_game.dll   (--target dll, the
                   default on Windows: the Windows launcher hosts it)
                -> <work>/build/burnout_recomp.exe (--target exe, the
                   default elsewhere: run it in Wine)

On success <work>/ready.txt records the image path and bundle version; the
Windows launcher reads it to decide whether the game is built.

Stamps in <work>/stamps/ make a later run skip what is already done: the
extract stamp covers the image (path, size, mtime); the build stamp covers the
image stamp plus the bundle version. A different dump or a new bundle rebuilds.

Output for the launcher's build screen, one line each:
  STAGE <name> <description>
  PROGRESS <percent> <detail>
  DONE <path to the dll or exe>
  ERROR <message>

Usage:
  build_from_iso.py --iso IMAGE --work DIR --bundle DIR [--force] [--target dll|exe]
  build_from_iso.py --game-dir DIR ...   (already-extracted files instead of an image)
The bundle directory holds: toolkit/ (xboxrecomp tools, src, include,
CMakeLists.txt), project/ (this repository's sources), zig/ (zig binary + lib),
and VERSION. Python, with capstone, cmake and ninja installed, is the
interpreter running this script. (The Windows bundle keeps Python in python/
beside those; this script does not care where its interpreter lives.)
"""
import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xiso_extract  # noqa: E402

WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""

# The launcher reads this output through a pipe. On Windows that would be the
# ANSI code page, which cannot print every path a player might have.
for stream in (sys.stdout, sys.stderr):
    stream.reconfigure(encoding="utf-8", errors="replace")

# Unversioned import libraries the project links but zig's mingw lacks; made
# from zig's own .def files at build time (see stage_zig).
IMPLIBS = {"libxinput.a": "xinput1_4.def", "libd3dcompiler.a": "d3dcompiler_47.def"}


def say(kind, *parts):
    print(kind, *parts, flush=True)


def image_stamp(iso):
    st = os.stat(iso)
    return hashlib.sha256(f"{os.path.abspath(iso)}|{st.st_size}|{int(st.st_mtime)}"
                          .encode()).hexdigest()[:16]


def read_stamp(work, name):
    try:
        with open(os.path.join(work, "stamps", name)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_stamp(work, name, data):
    os.makedirs(os.path.join(work, "stamps"), exist_ok=True)
    with open(os.path.join(work, "stamps", name), "w") as f:
        json.dump(data, f, indent=1)


def run(cmd, cwd=None, env=None, stage=None, expect_lines=None):
    """Run a step, relaying its output as PROGRESS lines."""
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1,
                            encoding="utf-8", errors="replace",
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    lines, tail, first_error = 0, [], None
    for line in proc.stdout:
        line = line.rstrip()
        lines += 1
        tail = (tail + [line])[-40:]
        # The first compiler-style error names the cause; the tail of CMake's
        # output usually does not, and the build screen shows only a few
        # lines of it.
        if first_error is None and ("error:" in line or "error :" in line):
            first_error = line.strip()
        pct = min(99, lines * 100 // expect_lines) if expect_lines else 0
        say("PROGRESS", pct, line[:160])
    if proc.wait() != 0:
        say("ERROR", f"{stage or cmd[0]} failed (exit {proc.returncode})"
                     + (f": {first_error[:300]}" if first_error else "") + "; last output:")
        for line in tail:
            print("  " + line, flush=True)
        raise SystemExit(1)


def stage_game_dir(args, work):
    """Use an already-extracted copy (default.xbe + data) instead of an image."""
    xbe = os.path.join(args.game_dir, "default.xbe")
    if not os.path.exists(xbe):
        say("ERROR", f"{args.game_dir} has no default.xbe")
        raise SystemExit(1)
    say("STAGE", "extract", "Using the extracted game folder")
    game = os.path.join(work, "game_files")
    if os.path.islink(game):
        os.remove(game)
    elif os.path.exists(game):
        shutil.rmtree(game)
    os.symlink(os.path.abspath(args.game_dir), game)
    with open(xbe, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()[:16]


def stage_extract(args, work):
    stamp = image_stamp(args.iso)
    game = os.path.join(work, "game_files")
    if not args.force and (read_stamp(work, "extract.json") or {}).get("image") == stamp \
            and os.path.exists(os.path.join(game, "default.xbe")):
        say("STAGE", "extract", "Game files already extracted")
        return stamp
    say("STAGE", "extract", "Extracting the game from your disc image")
    remove_tree(game)
    last = [-1]

    def progress(done, total, path):
        pct = done * 100 // total
        if pct != last[0]:
            last[0] = pct
            say("PROGRESS", pct, path)

    xiso_extract.extract(args.iso, game, progress)
    if not os.path.exists(os.path.join(game, "default.xbe")):
        say("ERROR", "the image has no default.xbe; is it an Xbox disc image?")
        raise SystemExit(1)
    write_stamp(work, "extract.json", {"image": stamp, "iso": os.path.abspath(args.iso),
                                       "time": time.time()})
    return stamp


def remove_tree(path):
    """Delete a folder, read-only files included, and fail loudly if it stays.

    Files copied from a network share often arrive read-only on Windows, and
    Windows will not delete a read-only file. rmtree(ignore_errors=True) then
    left the old folder in place without a word, and the copy over it failed
    with WinError 183 on the second build of a release (HeroGame, rc2)."""
    def clear_and_retry(func, target, exc):
        os.chmod(target, stat.S_IWRITE)
        func(target)
    if os.path.lexists(path):
        shutil.rmtree(path, onexc=clear_and_retry)


def copytree(src, dst):
    remove_tree(dst)
    # copyfile, not copy2: the copies must not inherit a read-only flag.
    shutil.copytree(src, dst, symlinks=True, copy_function=shutil.copyfile,
                    ignore=shutil.ignore_patterns("__pycache__", "output", "*.pyc"))


def stage_sources(args, work):
    say("STAGE", "stage", "Preparing the recompiler")
    toolkit = os.path.join(work, "toolkit")
    project = os.path.join(work, "project")
    copytree(os.path.join(args.bundle, "toolkit"), toolkit)
    copytree(os.path.join(args.bundle, "project"), project)
    if not WINDOWS:
        # The standalone exe runs from <work>/project, where it looks for
        # game_files\. The Windows launcher points the DLL at <work> itself,
        # so it needs no link (which Windows reserves for admins).
        link = os.path.join(project, "game_files")
        if os.path.lexists(link):
            os.remove(link)
        os.symlink(os.path.join(work, "game_files"), link)
    return toolkit, project


def stage_zig(args, work):
    """Toolchain file so CMake drives zig as a Windows C compiler.

    On Windows the bundle carries tools\\shims\\z*.exe (see
    launcher-win/src/zig_shim.c) and tools\\implib\\; elsewhere small shell
    wrappers and the import libraries are made here."""
    zig = os.path.join(args.bundle, "zig", "zig" + EXE)
    # zig's caches inside the work folder rather than in the user's profile.
    os.environ["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(work, "zig-cache")
    os.environ["ZIG_LOCAL_CACHE_DIR"] = os.path.join(work, "zig-cache")
    tc = os.path.join(work, "zig-toolchain")
    os.makedirs(tc, exist_ok=True)
    names = {}
    if WINDOWS:
        for name in ("zcc", "zar", "zranlib", "zrc"):
            names[name] = os.path.join(args.bundle, "shims", name + ".exe").replace("\\", "/")
    else:
        os.makedirs(os.path.join(tc, "bin"), exist_ok=True)
        implib = os.path.join(tc, "implib")
        os.makedirs(implib, exist_ok=True)
        defs = os.path.join(args.bundle, "zig", "lib", "libc", "mingw", "lib-common")
        for lib, d in IMPLIBS.items():
            subprocess.run([zig, "dlltool", "-d", os.path.join(defs, d), "-l",
                            os.path.join(implib, lib), "-m", "i386:x86-64"], check=True)
        tools = {
            "zcc": f'cc -target x86_64-windows-gnu -L"{implib}"',
            "zar": "ar",
            "zranlib": "ranlib",
            "zrc": "rc",
        }
        for name, sub in tools.items():
            p = os.path.join(tc, "bin", name)
            with open(p, "w") as f:
                f.write(f'#!/bin/sh\nexec "{zig}" {sub} "$@"\n')
            os.chmod(p, 0o755)
            names[name] = p
    toolchain = os.path.join(tc, "zig-windows.cmake")
    with open(toolchain, "w") as f:
        f.write(f"""set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "{names['zcc']}")
set(CMAKE_AR "{names['zar']}")
set(CMAKE_RANLIB "{names['zranlib']}")
set(CMAKE_RC_COMPILER "{names['zrc']}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
""")
    return toolchain


def stage_lift(args, work, toolkit, project):
    xbe = os.path.join(work, "game_files", "default.xbe")
    say("STAGE", "lift", "Recompiling the game code (this takes a few minutes)")
    run([sys.executable, os.path.join(project, "tools", "regenerate.py"),
         "--xbe", xbe, "--toolkit", toolkit, "--no-fixes"], cwd=project, stage="lift",
        expect_lines=1100)   # a full lift prints ~1,080 lines (HeroGame, 2026-10-13)
    say("STAGE", "fix", "Applying fixes")
    run([sys.executable, os.path.join(project, "tools", "apply_spin_fixes.py")], cwd=project,
        stage="fix")


def stage_compile(args, work, project, toolchain, output):
    say("STAGE", "compile", "Compiling (this takes a few minutes)")
    build = os.path.join(work, "build")
    remove_tree(build)
    # The bundle's Python carries the cmake and ninja wheels; a development
    # checkout may use the system ones instead.
    try:
        import cmake
        cmake_cmd = [os.path.join(cmake.CMAKE_BIN_DIR, "cmake" + EXE)]
    except ImportError:
        cmake_cmd = [shutil.which("cmake") or "cmake"]
    try:
        import ninja
        ninja_bin = os.path.join(ninja.BIN_DIR, "ninja" + EXE) if ninja.BIN_DIR else None
    except ImportError:
        ninja_bin = None
    ninja_bin = ninja_bin or shutil.which("ninja")
    gen = ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja_bin}"] if ninja_bin \
        else ["-G", "Unix Makefiles"]
    # The first configure prints nothing for several minutes while zig builds
    # its Windows C runtime into the work folder's cache; say so, or the
    # build screen looks stuck.
    say("PROGRESS", 0, "Setting up the C compiler (the first build takes several minutes)")
    run(cmake_cmd + ["-S", project, "-B", build] + gen +
        [f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         f"-DXBOXRECOMP_DIR={os.path.join(work, 'toolkit')}",
         f"-DBURNOUT_BUILD_DLL={'ON' if args.target == 'dll' else 'OFF'}"],
        stage="configure")
    jobs = str(max(1, (os.cpu_count() or 2)))
    run(cmake_cmd + ["--build", build, "-j", jobs], stage="compile", expect_lines=120)
    if not os.path.exists(output):
        say("ERROR", f"the build finished without producing {os.path.basename(output)}")
        raise SystemExit(1)
    return output


def write_ready(args, work, version, output):
    """What the Windows launcher checks before it loads the game: this exact
    image path (as it passed it) and this bundle version."""
    if not args.iso:
        return
    with open(os.path.join(work, "ready.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"iso={args.iso}\nversion={version}\noutput={output}\n")


def main():
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--iso")
    src.add_argument("--game-dir")
    ap.add_argument("--work", required=True)
    ap.add_argument("--bundle", required=True)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--target", choices=("dll", "exe"), default="dll" if WINDOWS else "exe",
                    help="burnout_game.dll for the Windows launcher, or the standalone exe")
    args = ap.parse_args()
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(args.bundle, "VERSION")) as f:
        version = f.read().strip()

    image = stage_game_dir(args, work) if args.game_dir else stage_extract(args, work)
    want = {"image": image, "bundle": version, "target": args.target}
    built = read_stamp(work, "build.json")
    output = os.path.join(work, "build", "burnout_game.dll" if args.target == "dll"
                          else "burnout_recomp.exe")
    if not args.force and built and {k: built.get(k) for k in want} == want \
            and os.path.exists(output):
        write_ready(args, work, version, output)
        say("STAGE", "ready", "Already built")
        say("DONE", output)
        return 0

    toolkit, project = stage_sources(args, work)
    toolchain = stage_zig(args, work)
    stage_lift(args, work, toolkit, project)
    output = stage_compile(args, work, project, toolchain, output)
    write_stamp(work, "build.json", dict(want, output=output, time=time.time()))
    write_ready(args, work, version, output)
    say("STAGE", "ready", "Ready to play")
    say("DONE", output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
