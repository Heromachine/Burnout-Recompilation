#!/bin/sh
# Assemble the Windows release folder:
#
#   <OUT>/Burnout.exe            the launcher (build.sh)
#   <OUT>/README.txt
#   <OUT>/tools/VERSION
#   <OUT>/tools/python/          embeddable Python + capstone, cmake, ninja
#   <OUT>/tools/zig/             zig (the C compiler)
#   <OUT>/tools/shims/           zcc/zar/zranlib/zrc.exe, which CMake runs
#   <OUT>/tools/implib/          libxinput.a, libd3dcompiler.a
#   <OUT>/tools/toolkit/         xboxrecomp, committed HEAD
#   <OUT>/tools/project/         this repository's COMMITTED sources, minus the
#                                developer tools and research the build never
#                                runs
#
# No game data: the player's disc is the only input, on their own machine.
#
# Only committed files ship (git ls-files -c, not -co as Breakdown's script
# does): this working tree carries untracked material that is not ours to
# distribute, and nothing untracked can reach the zip by accident. Commit
# before assembling.
#
#   OUT         default /mnt/dev/Burnout-Windows
#   VERSION     default 0.1.0
#   XBOXRECOMP  default /mnt/dev/xboxrecomp-fork
#   ZIG         zig for building the launcher here (default zig on PATH)
#   CACHE       downloads from fetch_toolchain.sh
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT=$(dirname "$HERE")
OUT=${OUT:-/mnt/dev/Burnout-Windows}
VERSION=${VERSION:-0.1.0}
XBOXRECOMP=${XBOXRECOMP:-/mnt/dev/xboxrecomp-fork}
CACHE=${CACHE:-$HOME/.cache/breakdown-win-toolchain}

# The release ships the toolkit's HEAD (git archive below). Warn, loudly but
# without stopping, if that is not the revision Burnout is pinned to
# (config/source-manifest.json), or if the project has uncommitted changes
# that would be left out.
PIN=$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['toolchain']['xboxrecomp']['commit'])" \
      "$PROJECT/config/source-manifest.json")
GOT=$(git -c safe.directory='*' -C "$XBOXRECOMP" rev-parse HEAD)
if [ "$GOT" != "$PIN" ]; then
    echo "WARNING: this release ships xboxrecomp $GOT, but Burnout is pinned" >&2
    echo "         to $PIN (config/source-manifest.json)." >&2
fi
if [ -n "$(git -c safe.directory='*' -C "$PROJECT" status --porcelain --untracked-files=no)" ]; then
    echo "WARNING: $PROJECT has uncommitted changes; only committed files ship." >&2
fi

"$HERE/fetch_toolchain.sh"
OUT="$HERE/out" "$HERE/build.sh"

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/burnout-release.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
T=$STAGE/tools
mkdir -p "$T/python/Lib/site-packages" "$T/toolkit" "$T/project"

cp "$HERE/out/Burnout.exe" "$STAGE/"
cp "$HERE/README.txt" "$STAGE/README.txt"
printf '%s\n' "$VERSION" > "$T/VERSION"

# Python. Its ._pth file fixes sys.path (the embeddable distribution ignores
# PYTHONPATH and the working directory), so site-packages is added there.
unzip -q "$CACHE/python-3.14.7-embed-amd64.zip" -d "$T/python"
printf 'python314.zip\n.\nLib\\site-packages\n' > "$T/python/python314._pth"
for w in "$CACHE"/wheels/*.whl; do unzip -q -o "$w" -d "$T/python/Lib/site-packages"; done
# Unzipping is not installing: a wheel's <name>.data/scripts (ninja.exe) is
# moved into place by pip. Put them beside python.exe, where the ninja
# package looks when there is no Scripts folder.
for d in "$T"/python/Lib/site-packages/*.data; do
    [ -d "$d/scripts" ] && mv "$d"/scripts/* "$T/python/"
    rm -rf "$d"
done

# zig, the shims CMake runs it through, and the two import libraries the
# game links that zig's MinGW lacks (made from zig's own .def files).
unzip -q "$CACHE/zig-x86_64-windows-0.14.1.zip" -d "$STAGE"
mv "$STAGE/zig-x86_64-windows-0.14.1" "$T/zig"
cp -r "$HERE/out/shims" "$T/shims"
mkdir -p "$T/implib"
DEFS=$T/zig/lib/libc/mingw/lib-common
"${ZIG:-zig}" dlltool -d "$DEFS/xinput1_4.def" -l "$T/implib/libxinput.a" -m i386:x86-64
"${ZIG:-zig}" dlltool -d "$DEFS/d3dcompiler_47.def" -l "$T/implib/libd3dcompiler.a" -m i386:x86-64

# Sources. The project keeps what the build runs: CMakeLists.txt, src/,
# config/ (seeds and the XBE hash), tools/{build_from_iso,xiso_extract,
# regenerate,apply_spin_fixes}.py, README.md.
git -c safe.directory='*' -C "$XBOXRECOMP" archive HEAD | tar -x -C "$T/toolkit"
(cd "$PROJECT" && git -c safe.directory='*' ls-files -c -z \
    | grep -zv -e '^launcher-win/' -e '^research/' -e '^debug-instruments/' \
               -e '^docs/' -e '^tests/' -e '^patches/' -e '__pycache__' \
               -e '^tools/section-inventory\.py$' -e '\.sh$' -e '\.gdb\.py$' \
    | xargs -0 tar -c) | tar -x -C "$T/project"

# Line endings the Windows tools and Notepad both read.
sed -i 's/$/\r/' "$STAGE/README.txt"

mkdir -p "$OUT"
rm -rf "$OUT/tools" "$OUT/Burnout.exe" "$OUT/README.txt"
cp -r "$STAGE"/. "$OUT"/
echo "release $VERSION assembled in $OUT ($(du -sh "$OUT" | cut -f1))"
