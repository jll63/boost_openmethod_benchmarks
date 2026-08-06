#!/bin/sh
# Build the disassembly oracle for asmtab.py: src/dispatch_probe.cpp, which
# contains every dispatch and nothing else, compiled with the same flags as the
# benchmark for all four compiler x bitness combinations.
#
# asmtab.py uses these to check that its backward slice of the *timed* window
# has not lost a dispatch instruction. They are never measured and never
# published. See the comment at the top of src/dispatch_probe.cpp.
set -e

mkdir -p bin

for cc in $(grep -v '^[[:space:]]*#' compilers.conf | grep -v '^[[:space:]]*$')
do
    echo "probe $cc"
    "$cc" -std=c++17 -O2 -march=native -m64 -fno-stack-protector \
        -fcf-protection=none -Wall -Wextra -DOMB_CLASSES="${CLASSES:-25}" \
        -Iinclude -Isrc src/dispatch_probe.cpp -o "bin/probe-$cc"
done
