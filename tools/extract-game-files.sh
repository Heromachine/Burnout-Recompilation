#!/bin/sh
#
# Extract the analysis inputs from the user's own Burnout disc image.
#
# This is the single documented command that recreates the pipeline's inputs
# from the read-only source media. It:
#
#   1. refuses to run unless the ISO matches the pinned hash in
#      config/source-manifest.json;
#   2. extracts default.xbe (and, with --all, the full disc) into game_files/,
#      which is gitignored -- nothing game-derived enters version control;
#   3. re-verifies what it extracted against the manifest before exiting.
#
# The ISO is opened read-only and is never written, moved or copied wholesale.
# game_files/ is disposable: delete it and re-run this script to rebuild it.
#
# Usage:
#   tools/extract-game-files.sh          extract default.xbe only
#   tools/extract-game-files.sh --all    also unpack every disc file
set -e

PROJECT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST="$PROJECT/config/source-manifest.json"
XBOXRECOMP=${XBOXRECOMP:-$(python3 -c "import json;print(json.load(open('$MANIFEST'))['toolchain']['xboxrecomp']['path'])")}
PY="$XBOXRECOMP/.venv/bin/python"
ISO=$(python3 -c "import json;print(json.load(open('$MANIFEST'))['source_media']['path'])")
OUT="$PROJECT/game_files"

[ -f "$ISO" ] || { echo "missing source media $ISO" >&2; exit 1; }
[ -x "$PY" ]  || { echo "missing $PY -- create the xboxrecomp venv first" >&2; exit 1; }

echo "==> verifying source media before extraction"
want_sha=$(python3 -c "import json;print(json.load(open('$MANIFEST'))['source_media']['sha256'])")
got_sha=$(sha256sum "$ISO" | cut -d' ' -f1)
if [ "$got_sha" != "$want_sha" ]; then
    echo "REFUSING: $ISO does not match the pinned hash." >&2
    echo "  got  $got_sha" >&2
    echo "  want $want_sha" >&2
    echo "Every piece of evidence in this project is tied to that image." >&2
    exit 1
fi
echo "  ISO OK"

mkdir -p "$OUT"

# tools.* resolve as modules only with the toolkit as the working directory,
# so run from there and keep every path absolute. Nothing is written inside
# $XBOXRECOMP: -o points back into this project.
if [ "$1" = "--all" ]; then
    echo "==> unpacking the whole disc into $OUT (large)"
    (cd "$XBOXRECOMP" && "$PY" -m tools.xiso unpack "$ISO" -o "$OUT")
else
    echo "==> extracting default.xbe into $OUT"
    (cd "$XBOXRECOMP" && "$PY" -m tools.xiso get "$ISO" default.xbe -o "$OUT")
fi

echo "==> verifying extracted inputs"
exec "$PROJECT/tools/verify-inputs.sh"
