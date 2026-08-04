#!/bin/sh
# Build without cmake. OpenMethod comes from ./include (a symlink into the
# working tree); everything else from the system-wide Boost in /usr/local.
#
# The library is header-only and nothing is linked, so the 64-bit Boost
# libraries in /usr/local/lib are irrelevant to a 32-bit build.
#
#   CXX=clang++ BITS=32 ./build.sh
#   DEBUG=1 CLASSES=4 ./build.sh      -> bin/benchmark-g++-64-g, for tracing
#
# DEBUG=1 builds -O0 -g into a `-g`-suffixed binary. Its timings are
# meaningless -- the point is that the dispatch is no longer inlined into
# timed_call and the locals still exist, so it can be stepped through. Pair it
# with a small CLASSES so the hierarchy is comprehensible.
set -e
CXX=${CXX:-g++}
BITS=${BITS:-64}
CLASSES=${CLASSES:-100}
DEBUG=${DEBUG:-0}

if [ "$DEBUG" = "1" ]; then
    OPT="-O0 -g"
    SUFFIX="-g"
else
    OPT="-O2"
    SUFFIX=""
fi

OUT=${OUT:-bin/benchmark-$(basename "$CXX")-$BITS$SUFFIX}

mkdir -p "$(dirname "$OUT")"

set -x
# -fcf-protection=none for BOTH compilers. Ubuntu's gcc defaults to =full and
# clang to none, which put an endbr64 on every indirect-call target in the gcc
# build and none in clang's -- so the compiler axis was comparing a distro
# policy, not code generation. Equalised here; see README.
$CXX -std=c++17 $OPT -march=native -m"$BITS" -fno-stack-protector \
     -fcf-protection=none -Wall -Wextra \
     -DOMB_CLASSES="$CLASSES" \
     -Iinclude -Isrc \
     src/main.cpp -o "$OUT"
