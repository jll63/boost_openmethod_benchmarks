#!/bin/sh
# Drive the benchmark on an isolated core.
#
# rdtsc counts *reference* cycles at the nominal TSC frequency, so results are
# comparable across runs even though the core clock is not visible under a
# hypervisor. Pinning matters: migrating between cores mid-measurement would
# read two different TSCs.
set -e

CPU=${CPU:-2}
REPS=${REPS:-2000}
BIN=${BIN:-bin/benchmark-g++-64}

[ -x "$BIN" ] || { echo "$BIN not built; run ./build.sh" >&2; exit 1; }

"$BIN" --verify

# chrt needs privileges; fall back to plain taskset when unavailable.
if chrt -f 50 true 2>/dev/null; then
    exec taskset -c "$CPU" chrt -f 50 "$BIN" --cpu "$CPU" --reps "$REPS" "$@"
else
    echo "note: no SCHED_FIFO (try sudo); running at normal priority" >&2
    exec taskset -c "$CPU" "$BIN" --cpu "$CPU" --reps "$REPS" "$@"
fi
