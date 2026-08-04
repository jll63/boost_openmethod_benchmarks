#!/bin/sh
# Build and run every (compiler, bitness) combination, RUNS times over.
#
# Output: results/run<k>/<compiler>-<bits>.csv
#
# Why repeated passes: a single cold measurement of one cell moves by a median
# of 11% (p90 40%) between passes on this machine, which is more than the
# differences between the columns. Looping the whole matrix rather than
# repeating each build in place also interleaves the four builds in time, so
# thermal and background-load drift lands on all of them alike instead of
# favouring whichever ran first. report.py takes the median across passes.
#
# `warm` and `clflush` are run separately and concatenated, rather than
# `--mode all`: the `sweep` block costs ~110 s per build and the matrix tables
# do not use it (under a full sweep `disp` for the fast variants is a small
# residue of two large DRAM-bound numbers -- see README).
set -e

CPU=${CPU:-2}
REPS=${REPS:-6000}
RUNS=${RUNS:-5}
OUTDIR=${OUTDIR:-results}

# Old pass directories would silently blend into report.py's medians -- and
# because results/ is committed, stale runs are the norm here, not an
# accident: RUNS=5 over the committed run1..run7 used to leave run6 and run7
# mixing a previous code version into every median (review finding).
mkdir -p "$OUTDIR"
rm -rf "$OUTDIR"/run*

# Build once; the passes only re-measure.
for cc in g++ clang++; do
    for bits in 64 32; do
        echo "building $cc / $bits-bit"
        CXX="$cc" BITS="$bits" ./build.sh > /dev/null

        # The gate: 32-bit OpenMethod is not covered by upstream CI, so a
        # silently wrong column is a real possibility. Fail loudly instead.
        "bin/benchmark-$cc-$bits" --verify
    done
done

tmp_warm=$(mktemp)
tmp_cold=$(mktemp)
trap 'rm -f "$tmp_warm" "$tmp_cold"' EXIT

k=1
while [ "$k" -le "$RUNS" ]; do
    dir="$OUTDIR/run$k"
    mkdir -p "$dir"
    echo "=== pass $k of $RUNS ==="

    for cc in g++ clang++; do
        for bits in 64 32; do
            bin="bin/benchmark-$cc-$bits"

            # Two plain redirections, then concatenate: the old
            # `... | tail -n +2` pipeline reported only tail's exit status,
            # so a benchmark killed mid-leg left a warm-only CSV and the
            # script still exited 0 (review finding).
            taskset -c "$CPU" "$bin" --cpu "$CPU" --reps "$REPS" \
                --mode warm --csv > "$tmp_warm"
            taskset -c "$CPU" "$bin" --cpu "$CPU" --reps "$REPS" \
                --mode clflush --csv > "$tmp_cold"

            {
                cat "$tmp_warm"
                tail -n +2 "$tmp_cold"
            } > "$dir/$cc-$bits.csv"
        done
    done

    k=$((k + 1))
done

echo
echo "render the README tables with:  python3 report.py"
