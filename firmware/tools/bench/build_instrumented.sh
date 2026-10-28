#!/usr/bin/env bash
# Build a cycle-instrumented Release-Speed image (instrument.py) in a scratch
# copy, from a commit or from the working tree; the checkout is not touched.
#
#   build_instrumented.sh NAME [COMMIT] [-- instrument.py options]
#     COMMIT omitted or "worktree": the working tree's tracked files as they are
#   -> $BENCH_BUILD_DIR/NAME/TREE (default ${TMPDIR:-/tmp}/alt2-bench/NAME/TREE),
#      TREE being the firmware directory's name in the repository (alt2)
#
# Flash it with flash.sh and point the scripts at it:
#   tools/bench/flash.sh $BENCH_BUILD_DIR/NAME/alt2
#   CHIREIX_ALT2=$BENCH_BUILD_DIR/NAME/alt2 python3 tools/bench/rx_cpu.py NAME
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$TREE/.." && pwd)
SUB=${TREE##*/}
NAME=${1:?usage: build_instrumented.sh NAME [COMMIT] [-- instrument options]}
shift
REF=worktree
if [ $# -gt 0 ] && [ "$1" != "--" ]; then REF=$1; shift; fi
[ "${1:-}" = "--" ] && shift
OUT=${BENCH_BUILD_DIR:-${TMPDIR:-/tmp}/alt2-bench}/$NAME
rm -rf "$OUT"; mkdir -p "$OUT"
if [ "$REF" = worktree ]; then
  git -C "$REPO" ls-files -z "$SUB" | (cd "$REPO" && tar --null -T - -cf -) | tar -x -C "$OUT"
else
  git -C "$REPO" archive "$REF" "$SUB" | tar -x -C "$OUT"
fi
python3 "$HERE/instrument.py" "$OUT/$SUB" "$@"
if ! command -v arm-none-eabi-gcc >/dev/null; then
  PATH=$(ls -d /opt/stm32cubeide/plugins/*gnu-tools-for-stm32*/tools/bin | tail -1):$PATH
fi
(cd "$OUT/$SUB" && JOBS=${JOBS:-8} bash tools/build_release_speed.sh) > "$OUT/build.log" 2>&1 \
  || { tail -20 "$OUT/build.log"; exit 1; }
# the objects are 150 MB; the ELF, map and dist/ images are what the bench uses
find "$OUT/$SUB" \( -name '*.o' -o -name '*.su' -o -name '*.d' -o -name '*.cyclo' \) -delete
grep -m1 'app .* bytes' "$OUT/build.log"
echo "$OUT/$SUB"
