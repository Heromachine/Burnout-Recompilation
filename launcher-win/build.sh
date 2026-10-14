#!/bin/sh
# Build Burnout.exe, the Windows launcher, from Linux with zig's clang.
#   ZIG   zig 0.14.x binary (default: zig on PATH)
#   OUT   output directory (default: launcher-win/out)
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ZIG=${ZIG:-zig}
OUT=${OUT:-$HERE/out}
IMGUI=$HERE/third_party/imgui
mkdir -p "$OUT"
"$ZIG" rc /fo "$OUT/launcher.res" -- "$HERE/src/launcher.rc"
"$ZIG" c++ -target x86_64-windows-gnu -O2 -std=c++17 \
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -I"$IMGUI" \
    "$HERE/src/main.cpp" \
    "$IMGUI/imgui.cpp" "$IMGUI/imgui_draw.cpp" "$IMGUI/imgui_tables.cpp" "$IMGUI/imgui_widgets.cpp" \
    "$IMGUI/backends/imgui_impl_win32.cpp" "$IMGUI/backends/imgui_impl_dx11.cpp" \
    "$OUT/launcher.res" \
    -ld3d11 -ldxgi -ld3dcompiler_47 -lcomdlg32 -lgdi32 -ldwmapi -limm32 -lshell32 -lole32 \
    -Wl,--subsystem,windows -municode \
    -o "$OUT/Burnout.exe"
# The compiler shims CMake runs on Windows (see src/zig_shim.c).
mkdir -p "$OUT/shims"
for sub in cc ar ranlib rc; do
    case $sub in cc) name=zcc ;; *) name=z$sub ;; esac
    "$ZIG" cc -target x86_64-windows-gnu -O2 -municode -DZIG_SUB="\"$sub\"" \
        "$HERE/src/zig_shim.c" -o "$OUT/shims/$name.exe"
done
rm -f "$OUT"/shims/*.pdb
echo "built $OUT/Burnout.exe and $OUT/shims/"
