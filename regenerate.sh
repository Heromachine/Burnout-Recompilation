#!/bin/sh
#
# Regenerate the recompiled C for Burnout from the user's own disc image.
#
# The pipeline itself is tools/regenerate.py (Python, so the Windows launcher
# runs the same code); its comments carry the seed history and the reasons
# behind each stage. This wrapper adds what only development needs: the pinned
# inputs are verified first, and the toolkit's venv supplies capstone.
#
# Usage:  ./regenerate.sh        then rebuild in build-win/
#
# Prerequisite: tools/extract-game-files.sh has produced game_files/default.xbe.
set -e

PROJECT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MANIFEST="$PROJECT/config/source-manifest.json"
XBOXRECOMP=${XBOXRECOMP:-$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['toolchain']['xboxrecomp']['path'])" "$MANIFEST")}
PY="$XBOXRECOMP/.venv/bin/python"

[ -x "$PY" ] || { echo "missing $PY -- create the xboxrecomp venv first" >&2; exit 1; }

# Inputs are verified before anything is lifted. A lift built from an
# unverified image matches no evidence in this project.
"$PROJECT/tools/verify-inputs.sh"

exec "$PY" "$PROJECT/tools/regenerate.py" --toolkit "$XBOXRECOMP" "$@"
