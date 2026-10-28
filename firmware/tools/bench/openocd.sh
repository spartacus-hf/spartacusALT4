#!/usr/bin/env bash
# The persistent openocd the bench scripts use: ST-LINK SWD, no reset, Tcl
# port 6666 (memory reads/writes without halting the CPU).
#   openocd.sh start | stop | status
set -uo pipefail
LOG=${BENCH_RESULTS:-$(cd "$(dirname "$0")" && pwd)/results}/openocd.log
case "${1:-status}" in
  start)
    if pgrep -x openocd >/dev/null; then echo "openocd already running"; exit 0; fi
    mkdir -p "$(dirname "$LOG")"
    setsid openocd -f interface/stlink.cfg -f target/stm32g4x.cfg -c "reset_config none" \
      -c "tcl_port 6666" -c init >"$LOG" 2>&1 &
    for _ in $(seq 30); do
      (exec 3<>/dev/tcp/127.0.0.1/6666) 2>/dev/null && { echo "openocd up (log $LOG)"; exit 0; }
      sleep 0.2
    done
    echo "openocd did not come up; see $LOG"; exit 1 ;;
  stop) pkill -x openocd; sleep 1 ;;
  status) pgrep -a openocd || echo "no openocd" ;;
  *) echo "usage: openocd.sh start|stop|status"; exit 2 ;;
esac
