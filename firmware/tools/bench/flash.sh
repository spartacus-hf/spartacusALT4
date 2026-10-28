#!/usr/bin/env bash
# Program the application (dist/release-speed/rev1_recovery.elf: application,
# footer and update journal; the loader and the settings/calibration pages are
# not touched), verify, reset, then restart the persistent openocd.
#   flash.sh [ALT2_TREE]      (default: this checkout's alt2, built Release-Speed)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
A=${1:-$(cd "$HERE/../.." && pwd)}
ELF=$A/dist/release-speed/rev1_recovery.elf
[ -f "$ELF" ] || { echo "$ELF missing: run tools/build_release_speed.sh in $A"; exit 1; }
"$HERE/openocd.sh" stop
openocd -f interface/stlink.cfg -f target/stm32g4x.cfg \
  -c "program $ELF verify reset exit" 2>&1 | grep -E 'Verified OK|rror' || exit 1
sleep 5
"$HERE/openocd.sh" start
