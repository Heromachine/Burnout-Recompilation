#!/bin/sh
#
# Verify every pinned input against config/source-manifest.json.
#
# Nothing in the pipeline may run against unverified inputs: a silently
# different ISO or XBE would produce a lift that looks fine and matches no
# evidence in this project. This script is the gate.
#
# Checks, in order:
#   1. the read-only source ISO's size and SHA-256;
#   2. the extracted game_files/default.xbe size and SHA-256 (skipped with a
#      notice if it has not been extracted yet);
#   3. the xboxrecomp / xemu / Breakdown revisions the project is pinned to
#      (a mismatch is a WARNING, not a failure -- the toolchain is expected to
#      move; the point is that it is never allowed to move unnoticed).
#
# Usage:  tools/verify-inputs.sh
# Exit:   0 all pinned hashes match, non-zero otherwise.
set -e

PROJECT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST="$PROJECT/config/source-manifest.json"
[ -f "$MANIFEST" ] || { echo "missing $MANIFEST" >&2; exit 1; }

field() { python3 -c "import json,sys;d=json.load(open(sys.argv[1]));
for k in sys.argv[2].split('.'): d=d[k]
print(d)" "$MANIFEST" "$1"; }

fail=0

check() { # label path expected_sha expected_size
    label=$1; path=$2; want_sha=$3; want_size=$4
    if [ ! -f "$path" ]; then
        echo "  $label: MISSING at $path"
        return 1
    fi
    got_size=$(stat -c '%s' "$path")
    got_sha=$(sha256sum "$path" | cut -d' ' -f1)
    if [ "$got_size" != "$want_size" ]; then
        echo "  $label: SIZE MISMATCH got $got_size want $want_size"
        return 1
    fi
    if [ "$got_sha" != "$want_sha" ]; then
        echo "  $label: SHA-256 MISMATCH"
        echo "      got  $got_sha"
        echo "      want $want_sha"
        return 1
    fi
    echo "  $label: OK ($got_size bytes)"
    return 0
}

echo "==> source media"
check "ISO" "$(field source_media.path)" \
      "$(field source_media.sha256)" "$(field source_media.size_bytes)" || fail=1

echo "==> extracted executable"
XBE="$PROJECT/$(field executable.extracted_path)"
if [ -f "$XBE" ]; then
    check "default.xbe" "$XBE" \
          "$(field executable.sha256)" "$(field executable.size_bytes)" || fail=1
else
    echo "  default.xbe: not extracted yet -- run tools/extract-game-files.sh"
fi

echo "==> toolchain revisions (warn-only)"
for key in toolchain.xboxrecomp toolchain.xemu toolchain.regression_control; do
    p=$(field "$key.path")
    want=$(field "$key.commit")
    if [ -d "$p/.git" ]; then
        got=$(git -C "$p" rev-parse HEAD)
        if [ "$got" = "$want" ]; then
            echo "  $p: OK ($got)"
        else
            echo "  $p: WARNING moved"
            echo "      pinned  $want"
            echo "      current $got"
        fi
    else
        echo "  $p: WARNING not a git worktree"
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "FAILED: a pinned input does not match the manifest." >&2
    exit 1
fi
echo "OK: all pinned hashes match."
