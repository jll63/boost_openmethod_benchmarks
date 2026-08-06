#!/bin/sh
# Build and run every compiler in compilers.conf, RUNS times over.
#
# Output: results/run<k>/<compiler>.csv
#
# Why repeated passes: a single cold measurement of one cell moves by a median
# of 11% (p90 40%) between passes on this machine, which is more than the
# differences between the columns. Looping the whole matrix rather than
# repeating each build in place also interleaves the four builds in time, so
# thermal and background-load drift lands on all of them alike instead of
# favouring whichever ran first. report.py takes the median across passes.
#
# Only `clflush` is measured. `warm` and `sweep` remain in the binary as
# diagnostics: warm resolves a few cycles but its yardstick is mostly branch
# misprediction, whose cost moves with binary layout, so warm ratios say more
# about where the code landed than about dispatch (see README).
set -e

CPU=${CPU:-2}
REPS=${REPS:-6000}
RUNS=${RUNS:-5}
OUTDIR=${OUTDIR:-results}

# The compilers come from compilers.conf, one per line, in column order; the
# first is the reference build. Adding a compiler is adding a line there.
CC_LIST=$(grep -v '^[[:space:]]*#' compilers.conf | grep -v '^[[:space:]]*$')

for cc in $CC_LIST; do
    command -v "$cc" > /dev/null || { echo "$cc: not installed" >&2; exit 1; }
    echo "$($cc --version | head -1)"
done

# Old pass directories would silently blend into report.py's medians -- and
# because results/ is committed, stale runs are the norm here, not an
# accident: RUNS=5 over the committed run1..run7 used to leave run6 and run7
# mixing a previous code version into every median (review finding).
mkdir -p "$OUTDIR"
rm -rf "$OUTDIR"/run*

# Build once; the passes only re-measure.
for cc in $CC_LIST; do
    echo "building $cc"
    CXX="$cc" OUT="bin/benchmark-$cc" ./build.sh > /dev/null

    # The gate: a registry wired to the wrong method, or an overrider that
    # failed to register, otherwise produces a plausible table.
    "bin/benchmark-$cc" --verify
done

k=1
while [ "$k" -le "$RUNS" ]; do
    dir="$OUTDIR/run$k"
    mkdir -p "$dir"
    echo "=== pass $k of $RUNS ==="

    for cc in $CC_LIST; do
        taskset -c "$CPU" "bin/benchmark-$cc" --cpu "$CPU" \
            --reps "$REPS" --mode clflush --csv > "$dir/$cc.csv"
    done

    k=$((k + 1))
done

echo
echo "render the README tables with:  python3 report.py"
