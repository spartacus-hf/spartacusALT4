#!/usr/bin/env bash
# Bit-exactness of the TX renderers between a commit and the working tree, on
# the host: both pipelines render the same ~81k frames (both SSB ANALYTIC
# backends; 4 carriers x 2 sidebands x 3 speech-processing setups x every TX
# bandwidth x 7 signals, then 21 feature variations: laws, calibration, AC
# offset, branch routing, math modes, gate, drive, short and empty frames,
# mute toggles) and every compare tick, flag, counter and renderer state
# word must match.
#   run.sh [BASE_REF=HEAD]       e.g. run.sh origin/main
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$H/../../.." && pwd)
REPO=$(cd "$TREE/.." && pwd)
SUB=${TREE##*/}
BASE=${1:-HEAD}
W=$(mktemp -d "${TMPDIR:-/tmp}/ssb_equiv.XXXXXX")
trap 'rm -rf "$W"' EXIT
git -C "$REPO" archive "$BASE" "$SUB/Core" | tar -x -C "$W"
bash "$H/build.sh" "$W/$SUB/Core" "$W/eq_base" 2>"$W/base.log" || { tail "$W/base.log"; exit 1; }
bash "$H/build.sh" "$TREE/Core" "$W/eq_new" 2>"$W/new.log" || { tail "$W/new.log"; exit 1; }
"$W/eq_base" "$W/base.bin"
"$W/eq_new" "$W/new.bin"
echo "base $BASE vs working tree:"
python3 "$H/compare.py" "$W/base.bin" "$W/new.bin"
