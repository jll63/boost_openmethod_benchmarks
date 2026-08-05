#!/bin/sh
# Build the disassembly oracle for asmtab.py: src/dispatch_probe.cpp, which
# contains every dispatch and nothing else, compiled with the same flags as the
# benchmark for all four compiler x bitness combinations.
#
# asmtab.py uses these to check that its backward slice of the *timed* window
# has not lost a dispatch instruction. They are never measured and never
# published. See the comment at the top of src/dispatch_probe.cpp.
set -e

CLANG=${CLANG:-$(ls /usr/bin/clang++-[0-9]* 2>/dev/null | sort -V | tail -1)}
CLANG=${CLANG:-clang++}
GXX=${GXX:-$(ls /usr/bin/g++-[0-9]* 2>/dev/null | sort -V | tail -1)}
GXX=${GXX:-g++}

mkdir -p bin

for cc in clang++ g++; do
    case $cc in
        clang++) real=$CLANG ;;
        g++) real=$GXX ;;
    esac

    for bits in 64 32; do
        echo "probe $cc / $bits-bit"
        "$real" -std=c++17 -O2 -march=native -m"$bits" -fno-stack-protector \
            -fcf-protection=none -Wall -Wextra -DOMB_CLASSES="${CLASSES:-100}" \
            -Iinclude -Isrc src/dispatch_probe.cpp -o "bin/probe-$cc-$bits"
    done
done
