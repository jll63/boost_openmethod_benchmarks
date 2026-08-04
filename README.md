# Boost.OpenMethod dispatch benchmark

Times **one** open-method dispatch with `rdtsc`, with the caches scrubbed
beforehand, against an ordinary virtual function call as the yardstick.

Boost.OpenMethod ships no benchmark. `doc/modules/ROOT/pages/performance.adoc`
states that dispatching through a reference is "between 30% and 50% slower than
calling the equivalent virtual function", attributed to "micro- and RDTSC-based
benchmarks", but no code in the repository produces that number. This does.

## What is measured

Five axes, plus yardsticks and baselines:

| axis | values |
|---|---|
| dispatch | how the v-table pointer is found: `vptr_vector` (the default: `std_rtti` + `fast_perfect_hash` + `vptr_vector`), `indirect` (the default plus `indirect_vptr`), `vptr_map` over `std::unordered_map`, `vptr_map` over `boost::unordered_flat_map`, `inplace` (`inplace_vptr`, the pointer stored in the object), `inplace_ind` (`inplace_vptr` plus `indirect_vptr`) |
| call form | `virtual_<const Base&>` (the v-table pointer is looked up at the call site) vs `virtual_ptr<const Base, R>` (already carries it) |
| arity | 1 and 2 virtual arguments |
| compiler | g++ 13.3 vs clang++ 18.1 |
| bitness | 64-bit vs 32-bit (`-m32`) |
| yardstick | `vf` — one virtual call; `vf+vf` — the double dispatch idiom, two chained virtual calls |
| baseline | `ovh` — timed region, no dispatch, object never touched; `nvf` / `nvf+nvf` — a non-virtual member call, which loads the receiver(s) but dispatches on nothing |

In the `dispatch` row, the first four values are registries and the last two are
not — `inplace_vptr` is a CRTP mixin, not a policy — so that axis is named for
what it selects, the way the v-table pointer is reached, rather than for how it
is spelled.

`inplace` and `inplace_ind` are measured **through a reference only**. A
`virtual_ptr` would be meaningless there: its purpose is to carry a v-table
pointer that would otherwise be looked up, and with `inplace_vptr` the object
already holds one, so the reference form does no lookup either.

They also need their own class hierarchy, and hence their own `nvf` baseline and
`vf` yardstick: `inplace_vptr_base` declares
`friend auto boost_openmethod_registry(Class*) -> Registry`, so a class binds to
exactly one registry, and the objects are 24 bytes rather than 16. Every ratio
below is against the yardstick of its own hierarchy; the three hierarchies' `vf`
measurements agree to within a cycle, which is what makes one table legitimate.

That comes to 33 variants, each run in 3 cache states: 21 on the main hierarchy
(4 registries x 2 call forms x 2 arities, plus 2 yardsticks and 3 baselines) and
6 on each inplace hierarchy (1 call form x 2 arities, plus its own 2 yardsticks
and 2 baselines).

Two compensations are applied, giving two columns:

- **`net` = mean − `ovh`** removes the apparatus: the `rdtsc` pair, the fences,
  and reaching the call site with cold code.
- **`disp` = mean − `nvf` of the same arity** additionally removes the cost of
  *reaching* the receiver. In the cold modes that is 40-45% of what a virtual
  call appears to cost, and it is common to every variant, so `net` alone
  compresses all the ratios toward 1. `disp` is dispatch alone.

## Build and run

Boost 1.91 must be installed system-wide, and `./include` must be a symlink into
a Boost checkout — it is machine-specific, so it is not in the repository:

```sh
ln -s /path/to/boost/libs/openmethod/include include
```

OpenMethod is then taken live from that working tree and everything else from
`/usr/local/include`. The library is header-only, so `-Iinclude` is the whole of
it — nothing to link.

```sh
./build.sh                       # -> bin/benchmark-g++-64
bin/benchmark-g++-64 --verify    # correctness gate, see below
./run.sh                         # pins a core, runs the full matrix
```

`CXX=clang++ BITS=32 ./build.sh` selects compiler and bitness; binaries land in
`bin/benchmark-<compiler>-<bits>`. `REPS=8000 CPU=5 ./run.sh` to change the run.
Options: `--reps --objects --sweep-mb --cpu --seed --mode
warm|clflush|sweep|all --csv --verify`.

For the compiler x bitness matrix:

```sh
./matrix.sh          # builds and verifies all four, RUNS=5 passes -> results/
python3 report.py    # renders the two markdown tables below
```

32-bit builds need `sudo apt install g++-multilib` (clang uses gcc's multilib
headers, so that one install covers both compilers).

The class count is compile-time (`Derived<N>` is a template):
`CLASSES=1000 ./build.sh`, or `-DOMB_CLASSES=1000` with CMake.

## Results

AMD Ryzen 9 9955HX (Zen 5), 32 MiB L3, WSL2, g++ 13.3.0 `-O2 -march=native`
64-bit, 100 classes, 4096 objects, 6000 reps, median of 7 passes (the `gcc/64`
column of the matrix below). `net` is reference cycles above `ovh`; `x vf` is
the ratio to the virtual-function yardstick of the same arity.

### Warm caches — the sharpest numbers

Nothing is evicted, so reaching the receiver is almost free: the `nvf` baseline
costs 3.6 cycles more than `ovh`. `net` and `disp` therefore agree, and both are
stable to ~1 cycle run to run. The `inplace` rows are ratioed against the inplace
hierarchy's own `vf`, which measures within a cycle of the main one.

| dispatch | arity | disp | x vf |
|---|---|---|---|
| `vf` | 1 | 13.8 | 1.00x |
| `om vptr / vptr_vector` | 1 | 12.5 | 0.89x |
| `om ref / vptr_vector` | 1 | 17.4 | 1.30x |
| `om vptr / indirect` | 1 | 16.4 | 1.13x |
| `om ref / indirect` | 1 | 24.5 | 1.69x |
| `om vptr / vptr_map` | 1 | 12.1 | 0.90x |
| `om ref / vptr_map` | 1 | 26.8 | 1.95x |
| `om vptr / flat_map` | 1 | 13.1 | 0.91x |
| `om ref / flat_map` | 1 | 27.0 | 1.92x |
| `om ref / inplace` | 1 | 12.1 | 0.89x |
| `om ref / inplace_ind` | 1 | 17.3 | 1.24x |
| `vf+vf (double dispatch)` | 2 | 18.5 | 1.00x |
| `om vptr / vptr_vector` | 2 | 6.8 | 0.39x |
| `om ref / vptr_vector` | 2 | 12.7 | 0.70x |
| `om vptr / indirect` | 2 | 8.2 | 0.46x |
| `om ref / indirect` | 2 | 15.0 | 0.83x |
| `om vptr / vptr_map` | 2 | 6.8 | 0.39x |
| `om ref / vptr_map` | 2 | 27.8 | 1.50x |
| `om vptr / flat_map` | 2 | 6.8 | 0.39x |
| `om ref / flat_map` | 2 | 25.6 | 1.40x |
| `om ref / inplace` | 2 | 6.8 | 0.42x |
| `om ref / inplace_ind` | 2 | 8.8 | 0.56x |

### Caches cold (`clflush`)

Flushed, the first touch of the receiver is a cache miss in its own right: the
`nvf` baseline costs 263 cycles more than `ovh`, against 574 for the whole `vf`
yardstick. So 46% of a virtual call's `net` is reaching the object rather than
dispatching on it — which is exactly what the `disp` column removes.

| dispatch | arity | net | disp | x net | x disp |
|---|---|---|---|---|---|
| `vf` | 1 | 574 | 307 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 1 | 563 | 299 | 0.98x | 0.96x |
| `om ref / vptr_vector` | 1 | 1114 | 858 | 1.92x | 2.73x |
| `om vptr / indirect` | 1 | 540 | 273 | 0.94x | 0.89x |
| `om ref / indirect` | 1 | 1340 | 1064 | 2.33x | 3.56x |
| `om vptr / vptr_map` | 1 | 535 | 279 | 0.93x | 0.88x |
| `om ref / vptr_map` | 1 | 773 | 513 | 1.37x | 1.69x |
| `om vptr / flat_map` | 1 | 540 | 279 | 0.90x | 0.83x |
| `om ref / flat_map` | 1 | 774 | 506 | 1.35x | 1.67x |
| `om ref / inplace` | 1 | 583 | 345 | 1.11x | 1.21x |
| `om ref / inplace_ind` | 1 | 739 | 502 | 1.41x | 1.76x |
| `vf+vf (double dispatch)` | 2 | 682 | 343 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 2 | 818 | 482 | 1.23x | 1.46x |
| `om ref / vptr_vector` | 2 | 1428 | 1105 | 2.11x | 3.21x |
| `om vptr / indirect` | 2 | 860 | 520 | 1.26x | 1.52x |
| `om ref / indirect` | 2 | 1747 | 1405 | 2.53x | 3.90x |
| `om vptr / vptr_map` | 2 | 797 | 474 | 1.18x | 1.35x |
| `om ref / vptr_map` | 2 | 1166 | 838 | 1.73x | 2.44x |
| `om vptr / flat_map` | 2 | 780 | 446 | 1.15x | 1.30x |
| `om ref / flat_map` | 2 | 1157 | 826 | 1.72x | 2.40x |
| `om ref / inplace` | 2 | 888 | 569 | 1.44x | 1.87x |
| `om ref / inplace_ind` | 2 | 1048 | 740 | 1.74x | 2.42x |

`net` and `disp` answer different questions, and the gap between the two columns
is the point: subtract only the apparatus and you learn how much slower a cold
open-method *call* is; subtract the object touch as well and you learn how much
slower open-method *dispatch* is. Neither is wrong. The second is the one that
isolates the library.

### Reading it

- **`virtual_ptr` dispatch costs about what a virtual function call does**, in
  every cache state: 0.89x warm, and cold within the spread of the cold table
  (see Reproducibility). The v-table pointer is
  already in the pointer, so the dispatch is a slot load and an indirect call —
  the three-instruction sequence in `performance.adoc:91-93`, confirmed in the
  disassembly below.
- **`virtual_` reference dispatch costs 1.30x a virtual call warm**, landing in
  the documented "30% to 50% slower" band. Cold and fully compensated it is
  roughly three times a virtual call, because the hash-and-look-up that builds the v-table pointer touches
  three more cache lines (the `type_info`, the vptr vector, the method slot),
  and each is its own miss.
- **At two virtual arguments the open-method beats the double dispatch idiom**
  outright, on `vptr_vector`: 0.70x warm through a reference, 0.39x through
  `virtual_ptr`. Double dispatch pays two *dependent* virtual calls, which
  cannot overlap; the multi-method issues two independent hash chains and
  indexes one two-dimensional table.
- **`vptr_map` costs about 1.45x `vptr_vector`** on the reference path warm
  (1.95x vs 1.30x of the yardstick). `boost::unordered_flat_map` is **not** a
  clear improvement on `std::unordered_map`: 1.92x vs 1.95x warm, and the two
  stay within run-to-run variance of each other cold as well, with the sign of
  the difference flipping between builds. Once a DRAM miss is on the critical path
  the container's probe strategy stops mattering much.
- **`inplace_vptr` makes reference dispatch as cheap as `virtual_ptr`
  dispatch** — which is the whole claim of the feature, and it holds exactly:
  12.1 cycles / 0.89x through a reference against 12.5 / 0.89x for a
  `virtual_ptr` on `vptr_vector`, and at arity 2, 0.42x against 0.39x. Compared
  with a *reference* on `vptr_vector` (17.4 / 1.30x) it saves 5.3 cycles, the
  entire hash-and-look-up. Cold the gap widens further, because
  it removes three of the four lines the lookup touches.
  The cost is 8 bytes per object and a hierarchy committed to one registry.
- **`indirect_vptr` costs about 3 cycles through a `virtual_ptr` and 6 through a
  reference**, warm: 12.5 -> 16.4 and 17.4 -> 24.5 on gcc/64, taking
  `virtual_ptr` dispatch from 0.89x a virtual call to 1.13x. The disassembly
  shows exactly one extra instruction, `mov rax, QWORD PTR [rax]` — the second
  dereference — in both call forms. It costs more on the reference path because
  there it lands at the end of an already-long dependency chain and has nothing
  to overlap with. Cold, the reference path pays a full miss for it: the extra
  indirection is a cache line of its own.
- The dispatch axis is **irrelevant to `virtual_ptr` dispatch** among the three
  *direct* registries, as it must be: 0.89x / 0.90x / 0.91x warm. That is the
  harness's built-in control — the vptr policy is not on that call path, so if
  those three rows diverge, the measurement is wrong, not the library.
  `indirect` is deliberately excluded from the control: it *does* add work to
  that path (a load), which is the whole point of the section below.

## Cost of `indirect_vptr`

`indirect_vptr` (`preamble.hpp:751`) is a marker policy. With it in the registry,
`vptr_vector` stores `const vptr_type*` instead of `vptr_type`, and a
`virtual_ptr` holds a pointer *to* the v-table pointer rather than the v-table
pointer itself (`core.hpp:751-754`). What you buy is the ability to call
`initialize()` again — after loading a shared library, say — and have every
`virtual_ptr` already in flight pick up the new v-tables, instead of dangling.
What you pay is one more load on every dispatch.

The `indirect` rows above are `boost::openmethod::indirect_registry`, i.e. the
default policies plus `indirect_vptr`, so the comparison against `vptr_vector`
isolates that one policy.

### What it costs in instructions: exactly one

Timed region, gcc 13 `-O2`, `virtual_ptr`, arity 1. Direct on the left, indirect
on the right; the only difference is the marked line.

```asm
mov  rcx, QWORD PTR [rsp]            mov  rcx, QWORD PTR [rsp]
mov  rax, QWORD PTR [rsp]            mov  rax, QWORD PTR [rsp]
mov  edx, edi                        mov  edx, edi
                                     mov  rax, QWORD PTR [rax]   ; <-- the load
mov  rdi, rcx                        mov  rdi, rcx
mov  rcx, QWORD PTR [rip+...]        mov  rcx, QWORD PTR [rip+...]
call QWORD PTR [rax+rcx*8]           call QWORD PTR [rax+rcx*8]
```

The `virtual_` reference path gains the same single `mov rax, QWORD PTR [rax]`,
inserted between the v-table lookup and the call.

### What it costs in cycles

Median of 7 passes; `disp` cycles, direct `vptr_vector` → `indirect`.

#### Warm — the extra load, uncontended

| call form | arity | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|---|
| `virtual_ptr` | 1 | 12.5 → 16.4 (**+3.9**) | 11.2 → 14.4 (**+3.2**) | 14.4 → 15.7 (**+1.3**) | 15.1 → 17.2 (**+2.2**) |
| `virtual_ptr` | 2 | 6.8 → 8.2 (**+1.3**) | 6.9 → 9.3 (**+2.3**) | 6.5 → 8.2 (**+1.6**) | 14.5 → 15.9 (**+1.4**) |
| `virtual_` ref | 1 | 17.4 → 24.5 (**+7.0**) | 16.2 → 22.6 (**+6.4**) | 18.5 → 24.3 (**+5.8**) | 20.3 → 23.9 (**+3.6**) |
| `virtual_` ref | 2 | 12.7 → 15.0 (**+2.3**) | 14.0 → 16.6 (**+2.6**) | 13.6 → 15.5 (**+2.0**) | 13.2 → 15.4 (**+2.2**) |
| `inplace` ref | 1 | 12.1 → 17.3 (**+5.2**) | 10.4 → 16.4 (**+6.0**) | 12.3 → 17.8 (**+5.5**) | 12.6 → 18.8 (**+6.1**) |
| `inplace` ref | 2 | 6.8 → 8.8 (**+2.0**) | 6.9 → 8.3 (**+1.5**) | 6.5 → 8.5 (**+2.1**) | 6.9 → 8.7 (**+1.8**) |

#### Cold (`clflush`) — the extra load, as a cache miss

| call form | arity | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|---|
| `virtual_ptr` | 1 | 299 → 273 (**-26**) | 332 → 360 (**+28**) | 283 → 332 (**+49**) | 320 → 291 (**-29**) |
| `virtual_ptr` | 2 | 482 → 520 (**+38**) | 508 → 551 (**+43**) | 477 → 585 (**+108**) | 597 → 599 (**+2**) |
| `virtual_` ref | 1 | 858 → 1064 (**+206**) | 889 → 1057 (**+169**) | 843 → 1023 (**+180**) | 858 → 1079 (**+220**) |
| `virtual_` ref | 2 | 1105 → 1405 (**+300**) | 1136 → 1376 (**+240**) | 1125 → 1319 (**+194**) | 1104 → 1291 (**+186**) |
| `inplace` ref | 1 | 345 → 502 (**+157**) | 283 → 546 (**+263**) | 311 → 496 (**+184**) | 310 → 571 (**+261**) |
| `inplace` ref | 2 | 569 → 740 (**+171**) | 520 → 824 (**+304**) | 560 → 735 (**+174**) | 524 → 835 (**+311**) |

### Reading it

- **Warm, it costs 3 to 4 cycles through a `virtual_ptr` and 3 to 7 through a
  reference.** In yardstick terms that takes `virtual_ptr` dispatch from 0.88x a
  virtual function call to 1.13x on gcc/64 — from slightly cheaper than a
  virtual call to slightly dearer. For the reference form it is 1.30x → 1.69x.
- **It costs more on the reference path than the `virtual_ptr` path**, roughly
  double, in every build. On the reference path the extra load sits at the end
  of an already-long dependency chain (object → `type_info` → hash → vptr vector
  → **indirection** → v-table) with nothing left to overlap against. Through a
  `virtual_ptr` the chain is two loads long, so there is more slack.
- **Cold, the reference path pays a full cache miss**, consistently signed
  across all four builds. The indirection target is a
  separate line from everything else the dispatch touches, so it is a miss of
  its own.
- **Cold through a `virtual_ptr` it is nearly free at arity 1** — the four
  builds disagree even on the sign. That is partly noise (the cold spread is
  26% of ~300 cycles, so ±78, which swamps a ~100-cycle effect), but there is a
  real mechanism too: the indirection load and the method's slot load are
  independent of each other, so the two misses overlap. On the reference path
  they cannot, because the slot is needed only after the indirection resolves.
  Do not read a benefit into the negative numbers; read "below what this
  apparatus can resolve".

### With `inplace_vptr`

The same policy applies to an `inplace_vptr` hierarchy, where it changes the
*stored* member from a `vptr_type` to a `const vptr_type*`. The cost is the same
shape and slightly larger: warm on gcc/64, `inplace` goes 12.1 → 17.3 cycles at
arity 1 and 6.9 → 8.8 at arity 2. So an
indirect inplace dispatch lands about where a *direct* `vptr_vector` reference
dispatch does, giving up most of what `inplace_vptr` won.

### When it is worth it

Arity 2 through a `virtual_ptr` costs +1.1 to +2.2 cycles warm, on a dispatch
that is already 0.30x the cost of the double-dispatch idiom — so an
`indirect_registry` multi-method still beats hand-written double dispatch by a
wide margin. The policy is cheap where dispatch is cheap, and dearest exactly
where dispatch is already dearest (cold, through a reference). If the program
needs `initialize()` to be callable more than once, the price is a load; if it
does not, `indirect_vptr` is pure cost.

## Compiler and bitness

Four builds — g++ 13.3 and clang++ 18.1, each at `-m64` and `-m32` — measured by
`./matrix.sh` and rendered by `report.py`. Cells are `<ratio>x (<cycles>)`: the
ratio to the virtual-function yardstick **of the same arity, from the same
build**, and the `disp` cycles it came from. Within-build ratios are what make
the columns comparable at all: gcc and clang generate different code for the
yardstick itself, so raw cycles across columns would not be.

What the bitness axis actually changes, from each binary's own header line:

```
build: g++ 13.3, 64-bit; sizeof void* 8, word 8, virtual_ptr 16
build: g++ 13.3, 32-bit; sizeof void* 4, word 4, virtual_ptr 8
```

`detail::word` is the dispatch-table cell, so at `-m32` every v-table, every
dispatch table and every `vptr_vector` entry is half the size, and `virtual_ptr`
goes from two words to one. **All four builds pass `--verify`** — worth noting,
since upstream CI covers 32-bit on MSVC only (`ADDRMD: '32,64'` in
`.drone.jsonnet`), not on gcc or clang.

#### Caches cold (`clflush`)

| dispatch | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|
| **1 virtual argument** |  |  |  |  |
| `vf (yardstick)` | 1.00x (307) | 1.00x (304) | 1.00x (308) | 1.00x (302) |
| `om vptr / vptr_vector` | 0.96x (299) | 1.08x (332) | 0.86x (283) | 1.10x (320) |
| `om ref / vptr_vector` | 2.73x (858) | 2.92x (889) | 2.80x (843) | 2.98x (858) |
| `om vptr / indirect` | 0.89x (273) | 1.18x (360) | 1.08x (332) | 0.96x (291) |
| `om ref / indirect` | 3.56x (1064) | 3.47x (1057) | 3.24x (1023) | 3.59x (1079) |
| `om vptr / vptr_map` | 0.88x (279) | 0.92x (269) | 0.88x (266) | 0.81x (234) |
| `om ref / vptr_map` | 1.69x (513) | 2.01x (608) | 1.57x (488) | 2.12x (619) |
| `om vptr / flat_map` | 0.83x (279) | 0.93x (272) | 0.95x (310) | 0.88x (250) |
| `om ref / flat_map` | 1.67x (506) | 2.10x (626) | 1.53x (469) | 2.00x (595) |
| `om ref / inplace` | 1.21x (345) | 0.98x (283) | 1.06x (311) | 1.06x (310) |
| `om ref / inplace_ind` | 1.76x (502) | 2.00x (546) | 1.78x (496) | 1.94x (571) |
| **2 virtual arguments** |  |  |  |  |
| `vf+vf (yardstick)` | 1.00x (343) | 1.00x (339) | 1.00x (400) | 1.00x (380) |
| `om vptr / vptr_vector` | 1.46x (482) | 1.41x (508) | 1.35x (477) | 1.55x (597) |
| `om ref / vptr_vector` | 3.21x (1105) | 3.10x (1136) | 2.83x (1125) | 2.92x (1104) |
| `om vptr / indirect` | 1.52x (520) | 1.61x (551) | 1.43x (585) | 1.58x (599) |
| `om ref / indirect` | 3.90x (1405) | 3.95x (1376) | 3.13x (1319) | 3.45x (1291) |
| `om vptr / vptr_map` | 1.35x (474) | 1.31x (449) | 1.25x (502) | 1.32x (514) |
| `om ref / vptr_map` | 2.44x (838) | 2.29x (823) | 2.08x (815) | 2.20x (834) |
| `om vptr / flat_map` | 1.30x (446) | 1.34x (494) | 1.26x (500) | 1.32x (518) |
| `om ref / flat_map` | 2.40x (826) | 2.45x (866) | 2.01x (803) | 2.15x (799) |
| `om ref / inplace` | 1.87x (569) | 1.71x (520) | 1.39x (560) | 1.51x (524) |
| `om ref / inplace_ind` | 2.42x (740) | 2.58x (824) | 2.06x (735) | 2.51x (835) |

Median of 7 passes. Spread across passes: median 29%, p90 52%.

#### Warm caches

| dispatch | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|
| **1 virtual argument** |  |  |  |  |
| `vf (yardstick)` | 1.00x (13.8) | 1.00x (13.6) | 1.00x (11.8) | 1.00x (15.2) |
| `om vptr / vptr_vector` | 0.89x (12.5) | 0.80x (11.2) | 1.22x (14.4) | 0.99x (15.1) |
| `om ref / vptr_vector` | 1.30x (17.4) | 1.23x (16.2) | 1.57x (18.5) | 1.33x (20.3) |
| `om vptr / indirect` | 1.13x (16.4) | 1.13x (14.4) | 1.33x (15.7) | 1.14x (17.2) |
| `om ref / indirect` | 1.69x (24.5) | 1.72x (22.6) | 2.05x (24.3) | 1.59x (23.9) |
| `om vptr / vptr_map` | 0.90x (12.1) | 0.77x (10.4) | 1.03x (12.2) | 0.90x (13.5) |
| `om ref / vptr_map` | 1.95x (26.8) | 1.83x (24.8) | 2.29x (27.3) | 1.73x (26.0) |
| `om vptr / flat_map` | 0.91x (13.1) | 0.77x (10.7) | 1.05x (12.2) | 0.94x (14.1) |
| `om ref / flat_map` | 1.92x (27.0) | 1.89x (25.5) | 2.61x (30.8) | 2.05x (31.1) |
| `om ref / inplace` | 0.89x (12.1) | 0.77x (10.4) | 0.90x (12.3) | 1.00x (12.6) |
| `om ref / inplace_ind` | 1.24x (17.3) | 1.22x (16.4) | 1.32x (17.8) | 1.56x (18.8) |
| **2 virtual arguments** |  |  |  |  |
| `vf+vf (yardstick)` | 1.00x (18.5) | 1.00x (17.9) | 1.00x (16.5) | 1.00x (25.4) |
| `om vptr / vptr_vector` | 0.39x (6.8) | 0.39x (6.9) | 0.40x (6.5) | 0.57x (14.5) |
| `om ref / vptr_vector` | 0.70x (12.7) | 0.77x (14.0) | 0.85x (13.6) | 0.53x (13.2) |
| `om vptr / indirect` | 0.46x (8.2) | 0.52x (9.3) | 0.50x (8.2) | 0.65x (15.9) |
| `om ref / indirect` | 0.83x (15.0) | 0.93x (16.6) | 0.93x (15.5) | 0.61x (15.4) |
| `om vptr / vptr_map` | 0.39x (6.8) | 0.39x (6.9) | 0.40x (6.5) | 0.57x (14.5) |
| `om ref / vptr_map` | 1.50x (27.8) | 1.37x (24.4) | 1.61x (26.5) | 1.07x (27.2) |
| `om vptr / flat_map` | 0.39x (6.8) | 0.39x (6.9) | 0.40x (6.6) | 0.58x (14.7) |
| `om ref / flat_map` | 1.40x (25.6) | 1.48x (26.3) | 1.72x (28.4) | 1.24x (31.4) |
| `om ref / inplace` | 0.42x (6.8) | 0.40x (6.9) | 0.38x (6.5) | 0.27x (6.9) |
| `om ref / inplace_ind` | 0.56x (8.8) | 0.48x (8.3) | 0.38x (8.5) | 0.34x (8.7) |

Median of 7 passes. Spread across passes: median 11%, p90 41%.

### What it shows

- **Bitness buys a little warm, and nothing cold.** Warm, `om ref /
  vptr_vector` goes 1.26x -> 1.20x (gcc) and 1.67x -> 1.35x (clang) at `-m32`,
  with the absolute cycles dropping too (18.3 -> 16.6 on gcc). Cold, the
  *cycles* barely move while the ratios rise, because the yardstick got faster
  as well. Halving the tables does not help when misses are
  counted per line rather than per byte: at 100 classes the hot part of the
  dispatch data already occupies few enough lines that 4-byte cells buy no fewer
  misses.
- **The compilers' columns differ, but not in dispatch quality.** clang's
  `virtual_ptr` dispatch is three instructions to gcc's seven and measures the
  same; its reference dispatch is instruction-for-instruction equivalent to
  gcc's and measures the same. What differs is the *yardstick* they are divided
  by, which is dominated by indirect-branch misprediction. Worked through in
  "gcc against clang, instruction by instruction" below.
- **The arity-2 yardstick is itself compiler-sensitive**, which is why the
  arity-2 ratios move around more than the cycle counts do. `vf+vf` warm varies
  by more than 40% across the four builds — on the *denominator* alone — while
  the open-method side varies far less. Read the arity-2 cycle columns before
  the ratios.
- **`indirect_vptr` is the one registry choice that is uniformly worse**, and
  by a consistent amount across all four builds: warm, `om vptr` goes from
  cheaper than a virtual call to dearer than one in every column.
  That is the extra load, and it is the price of being able to replace v-tables
  after `initialize()` without rebuilding every `virtual_ptr` in flight.
- **`inplace` is the fastest dispatch in the table, in every build**, and the
  only one that reaches `virtual_ptr` speed through a plain reference, in every
  column, against roughly half again as much for a reference on `vptr_vector`.
- **The one outlier is a benchmark artifact, not a library cost.** clang/32
  reports `om vptr` at arity 2 as roughly twice what the other three builds do,
  consistently across all four registries and all seven passes. The disassembly
  says why: at `-m32` clang marshals the two `virtual_ptr` arguments through the
  stack *inside* the timed region — eight loads and stores plus two `vmovsd`
  pairs — where the 64-bit build passes them in registers. gcc at `-m32` does
  not (6.9 cycles, matching its own 64-bit build). That is the i386 ABI meeting
  this harness's by-value `args` struct, not dispatch.
### gcc against clang, instruction by instruction

The two compilers' columns differ, and it is worth being precise about what the
difference is, because the obvious reading — that one generates better dispatch
than the other — is wrong.

**`virtual_ptr`: clang's code is strictly better, and the time is the same.**
The timed region, arity 1, `vptr_vector`:

```asm
; gcc -- spills the virtual_ptr and reloads it twice
mov  rcx, QWORD PTR [rsp]                mov  rax, QWORD PTR [rip+slot]
mov  edx, edi                            mov  edx, ecx
mov  rax, QWORD PTR [rsp]                call QWORD PTR [rdi+rax*8]
mov  rdi, rcx
mov  rcx, QWORD PTR [rip+slot]
call QWORD PTR [rax+rcx*8]
```

Seven instructions against three. clang keeps the `virtual_ptr` in registers and
emits exactly the sequence `performance.adoc:91-93` documents; gcc round-trips it
through the stack. They measure within a cycle of each other, because both are L1
hits and the critical path is the dependent load plus the indirect call, not the
instruction count.

**`virtual_` reference: the two are equivalent.** Nine instructions each, same
dependency chain — object v-table pointer, `type_info`, multiply, shift, index
the vptr vector, load the method slot, call:

```asm
; gcc                                    ; clang
mov  rax, QWORD PTR [rdi]                mov  rax, QWORD PTR [rdi]
mov  rdx, QWORD PTR [rip+shift]          mov  rcx, QWORD PTR [rip+mult]
mov  rax, QWORD PTR [rax-0x8]            imul rcx, QWORD PTR [rax-0x8]
imul rax, QWORD PTR [rip+mult]           movzx eax, BYTE PTR [rip+shift]
shrx rax, rax, rdx                       shrx rax, rcx, rax
mov  rdx, QWORD PTR [rip+vptrs]          mov  rcx, QWORD PTR [rip+vptrs]
mov  rax, QWORD PTR [rdx+rax*8]          mov  rax, QWORD PTR [rcx+rax*8]
mov  rdx, QWORD PTR [rip+slot]           mov  rcx, QWORD PTR [rip+slot]
call QWORD PTR [rax+rdx*8]               call QWORD PTR [rax+rcx*8]
```

If anything clang is tighter: it folds the `type_info` load into the multiply's
memory operand, and loads `shift` as a **byte** — legitimate, since `shrx` reads
only the low six bits of its count. Neither difference lengthens the chain.

**So where does the ratio gap come from? The yardstick.** `x vf` divides by a
virtual call measured in the same build, and that divisor is dominated not by the
call but by *indirect-branch misprediction* — 100 leaf types drawn at random give
the predictor nothing to work with. Rebuilding with a single leaf, so the call
always goes to the same target, separates the two:

| `disp` cycles, warm | gcc | clang |
|---|---|---|
| `vf`, 1 class (predictable) | 2.4 | 6.3 |
| `vf`, 100 classes (mispredicted) | 14.6 | 11.6 |
| `om ref`, 1 class | 9.0 | 9.3 |
| `om ref`, 100 classes | 18.7 | 18.2 |

Two things fall out. The virtual call is mostly misprediction — 2.4 cycles when
predicted, 14.6 when not. And **`om ref` is the same on both compilers at either
class count**; it is the yardstick that differs, and it differs in *opposite
directions* depending on how many targets there are. That signature is
branch-target-buffer behaviour differing between two binaries with different
layouts, not one compiler dispatching better than the other.

The practical consequence: compare cycle columns across compilers, and treat a
cross-compiler difference in `x vf` as a statement about the denominator until
shown otherwise.

### Two flags that had to be equalised

Both were found by reading disassembly, and both had been silently biasing the
compiler axis:

- **`-fcf-protection=none`.** Ubuntu's gcc defaults to `=full`, clang to none, so
  the gcc binary carried 7391 `endbr64` landing pads against clang's 5 — one at
  the top of every indirect-call target, including every overrider and every
  virtual function. The axis was comparing a distribution's hardening policy, not
  code generation. Both builds now pass `-fcf-protection=none` and the callees are
  byte-identical:

  ```asm
  ; gcc, before          ; gcc and clang, now
  endbr64                lea eax, [rsi+0x1]
  lea eax, [rsi+0x1]     ret
  ret
  ```

- **How the timestamp is assembled** — see the next section.

### Why the timestamp is assembled afterwards

`rdtsc` returns the counter in `edx:eax`, and `rdtscp` overwrites both, so the
first reading has to be moved out of those registers before the second one runs.
With the `__rdtsc()` intrinsic the *whole* 64-bit assembly — `shl`, `mov`, `or` —
ended up between the two readings, and each compiler placed it differently: gcc
right after the second `lfence`, inside the measured window; clang before that
`lfence`; and in `nvf` clang sank the `or` past the work. Three instructions
either way, but scheduled differently per compiler and, worse, differently
between a variant and the baseline subtracted from it — so they did not cancel in
`disp`.

`timing.hpp` now captures the raw `lo`/`hi` pair in inline asm and assembles the
64-bit values only after the second reading, leaving nothing to schedule:

```asm
; gcc                          ; clang
lfence                         lfence
rdtsc                          rdtsc
lfence                         lfence
mov  r12d, eax                 mov  r14d, edx
<work, 2nd mov interleaved>    neg  r14d
rdtscp                         shl  r14, 0x20
                               mov  r15d, eax
                               <work>
                               rdtscp
```

gcc saves the two halves and interleaves them with the work; clang additionally
pre-negates the high half so the final subtraction is cheaper. Different, but now
*identical across every variant within a compiler*, which is what makes it cancel
when `nvf` is subtracted.

This did not change the compiler gap — it was the misprediction effect above all
along — but it removed a confound that would have made that impossible to
demonstrate.

### Reproducibility, and why seven passes

A single cold pass moves by a median of 11% between repeats (p90 40%, max 73%)
— more than the differences between the columns. Every cell above is therefore
the **median of 7 passes**, and `matrix.sh` loops the whole matrix rather than
repeating each build in place, so thermal and background drift lands on all four
columns alike instead of favouring whichever ran first.

Even so, read the cold table for the large effects only. The built-in control
says how far to trust it: the three *direct* registries' `om vptr` rows must
agree, since the vptr policy is off that call path (`indirect` is excluded — it
adds a load there by design). Cold they agree to within 9% on the gcc columns
and 17-25% on the clang ones. Warm is far better behaved — a few percent, and
the arity-2 control is exact (6.9 / 6.9 / 6.9 cycles on gcc/64).

### Shielding would not help

Isolating the benchmark from other processes — a cgroup cpuset, `isolcpus`, the
`cgexec -g memory,cpu:shield` that [Boost.Unordered's
benchmarks](https://github.com/boostorg/boost_unordered_benchmarks) run under —
buys nothing measurable here. That was worth checking rather than assuming.
Three cold runs of `om ref / vptr_vector`, `disp` cycles, under load conditions
imposed on purpose (a separate one-off probe, not part of `results/`):

| condition | three runs |
|---|---|
| idle | 907 / 848 / 868 |
| eight spinners on other cores | 879 / 873 / 882 |
| a spinner on the measurement core itself | 818 / 904 / 866 |
| four 256 MiB memory streamers on other cores | 916 / 842 / 914 |

A co-tenant on the measurement core — exactly what a cpuset shield evicts —
moves the result less than idle run-to-run variation does. Memory-bandwidth
contention, the one condition that does register, shifts it about 5%, against
the 11% the same measurement drifts between passes anyway.

The reason is that the trimmed mean already does the shield's job from the other
end. A preemption is orders of magnitude larger than a dispatch, so it lands in
the discarded top 5% rather than perturbing the body. Boost.Unordered needs the
cgroup because it measures multithreaded throughput over seconds, where stolen
time biases the mean directly and nothing trims it; and it runs on self-hosted
bare metal, where shielding is actually achievable.

What is left over is not schedulable: DRAM timing and prefetcher state, the core
clock — invisible and uncontrollable under this hypervisor — and Windows
descheduling the vCPU, which no cgroup inside the VM can reach. Frequency
pinning on bare metal would cut the cold spread; quieting this box would not.

## Method

### Timing

Each measurement brackets exactly one call:

```
asm barrier; lfence; rdtsc; lfence; asm barrier
    <the one call>
asm barrier; rdtscp; lfence; asm barrier
```

- The brackets are *inside* a `noinline` function whose parameters are the call
  arguments, so the call and prologue are not timed while the arguments still
  arrive through the ABI and cannot be constant-folded or devirtualized.
- Arguments are built by `prepare()` **outside** the timed region — in
  particular each `virtual_ptr` is constructed up front, so the `virtual_ptr`
  rows measure dispatch alone, not construction.
- **Every variant replays the identical sequence of receivers**: the RNG is
  reseeded at the start of each variant. `disp` is a difference of two separate
  measurements, and in the cold modes both are dominated by DRAM latency that
  depends on *which* objects were drawn. Drawing from one shared stream left
  each variant on different objects, so that term did not cancel; pairing the
  draws cut the run-to-run spread of `disp` for the fastest variants from a 2x
  range down to about ±13%.
- The `asm volatile ... "memory"` are *compiler* barriers, separate from the
  lfences. `_mm_lfence()` emits an instruction but does not stop the optimizer
  hoisting a load above it, and every dispatch here loads from loop-invariant
  globals (hash factors, table base, method slot) that are prime hoisting
  candidates. (Checked: gcc 13 does not hoist them even without the barriers.)
- Both compilers are given `-fcf-protection=none` and the same timestamp
  capture, because their defaults differed in ways that biased the comparison —
  see "Two flags that had to be equalised".
- `lfence` is dispatch-serializing by default on Zen, so no `CPUID` is needed —
  which is just as well, since `CPUID` traps to the hypervisor here and would
  cost more than the thing being measured.

### Cache states

| mode | what it does |
|---|---|
| `warm` | nothing, after a warm-up call — lower bound |
| `clflush` | `clflushopt` over the object, its C++ v-table, the method's `fn` object, the registry's dispatch-table arena, and the vptr storage; then `mfence` |
| `sweep` | one store per 64-byte line over a 64 MiB buffer (2x L3); then `mfence` |

`sweep` uses ordinary stores, not `_mm_stream_*`: non-temporal stores bypass the
cache hierarchy and would evict nothing. It is sequential rather than
pointer-chasing because sweeping is bandwidth-bound (~1.6 ms for 64 MiB), while
1M dependent DRAM loads would cost ~80 ms per repetition.

**`sweep` is the mode to quote.** It treats every variant identically.
`clflush` is a lower-noise diagnostic, but it **cannot reach the interior of
`std::unordered_map` / `unordered_flat_map`** — those nodes are only reachable
at run time — so the map registries keep bucket data resident and look better
than they are. Compare `virtual_` ref arity 1: under `clflush` `vptr_map` (1.59x)
appears to *beat* `vptr_vector` (2.13x); under `sweep`, which really does evict
both, the order reverses to 1.87x vs 1.55x. That inversion is why both modes are
reported.

### Statistics

Reported statistic is the **mean, trimmed of its top 5%**, with the standard
error of the difference against `ovh` in the `+/-` column.

The median is shown but should not be read: an lfence-bracketed `rdtsc` pair
costs **25 or 50 cycles, bimodally** on this machine (the raw counter does
resolve to 1 — it is the read that is coarse), so every individual sample is
quantized far above the cost of a warm dispatch, and the median of warm samples
is always exactly 50. The proportion of samples landing in each mode does shift
with the work done, so the mean resolves well below one tick. The top 5% is
trimmed because scheduler preemptions land there and a single one moves the mean
by more than the quantity being measured.

**Cold absolutes vary ~25% run to run** (DRAM, prefetcher and hypervisor
scheduling); the *ratio to the yardstick measured in the same run* is far more
stable. Three runs of `om ref` arity 1 gave nets of 1648 / 2049 / 1983 but `x
net` ratios of 1.55x / 1.50x / 1.51x. Quote ratios, not cycle counts.

### Which compensation to read

`net` and `disp` answer different questions and have different noise.

| mode | object-touch cost | `disp` reliable? |
|---|---|---|
| `warm` | ~1 cycle | yes — `disp` ≈ `net`, both stable to ~1 cycle |
| `clflush` | ~45% of what `vf` appears to cost | yes — the compensation is large and reproducible |
| `sweep` | ~45% of what `vf` appears to cost | **not for the fast variants** |

Under a full sweep, `disp` for a `virtual_ptr` row is a ~250-cycle residue left
by subtracting two ~800-cycle DRAM-bound numbers, and its run-to-run spread is
comparable to its value (`x disp` for `om vptr` arity 1 measured 0.42x / 0.51x /
0.47x over three runs, and 0.13x / 0.26x / 0.23x before the draws were paired).
The `x net` ratios stay stable there because the shared miss sits in both terms.

So: **read `x disp` warm and under `clflush`; under `sweep` read `x net`** and
treat it as a floor on the true ratio, since the shared miss biases it toward
1.00x.

### Correctness gate

`--verify` calls every open-method path — four registries x two call forms x two
arities, plus the two inplace dispatches x two arities — against the virtual
function and the non-virtual baseline of the matching hierarchy, over a sample
of receiver pairs, and checks they all agree. A registry wired to
the wrong method, or an overrider that silently failed to register, would otherwise
produce a plausible but meaningless table. It runs automatically at startup and
as the CMake test.

## Caveats

- **The 2-argument yardstick is a cost model, not the literal idiom.** The
  textbook double dispatch declares one `dd_with_DerivedK` per leaf in the base;
  with 100 leaves that is unwritable, and it would not change the cost, which is
  two chained virtual calls either way. `Base` declares `dd` and `dd_with`, and
  `Derived<N>::dd` calls `other.dd_with(*this, x)`. Two virtual calls, as the
  idiom costs.
- **Warm-mode differences of a few cycles are not trustworthy.** The signal
  (8–28 cycles) sits on a floor whose own mean is 41.5 with 25-cycle
  quantization. The trimmed mean is statistically stable, but the mapping from
  "mean of quantized samples" to true latency depends on where the work falls
  relative to the tick boundary, and that phase differs between instruction
  schedules. Symptom: warm mode reports arity 2 as *cheaper* than arity 1 for
  `virtual_` ref (12.8 vs 18.3), which is impossible — the 2-argument dispatch
  strictly does more work, as the disassembly below shows. The cold modes order
  them correctly. Use
  warm mode to separate things that differ by a lot, not by a few cycles.
- **`nvf` is a lower bound on "reaching the receiver", not an exact one.** It
  reads `tag` at offset 8; a dispatch reads the v-table pointer at offset 0.
  Same cache line, so the same one miss — but a `virtual_` reference dispatch
  then chases that pointer to the `type_info`, which is a *second* miss that
  `disp` charges to dispatch. That is the right attribution (it is work the
  dispatch causes), but it means `disp` is not purely "arithmetic and an
  indirect call".
- **Discard runs where the control fails.** One 6000-rep run came out with a
  sweep block ~50% slower than its neighbours and a broken `virtual_ptr` control
  (1525 cycles for `vptr_vector` against 922 and 800 for the map registries,
  when all three must agree). Deliberate CPU and memory contention reproduce
  nothing like that magnitude (see "Shielding would not help"), so the cause was
  more likely a frequency excursion or hypervisor scheduling than a noisy
  neighbour — which is precisely why the control, not a tidy `uptime`, is the
  thing to check.
- **gcc passes the 16-byte `virtual_ptr` via the stack**, so the `virtual_ptr`
  rows pay two extra L1 loads inside the timed region that `vf` does not. This
  biases them *against* `virtual_ptr`, so those numbers are conservative.
- Under WSL2/Hyper-V the TSC ticks at the nominal ~2.37 GHz, so these are
  *reference* cycles, not core cycles, and the core clock is not observable.
  `run.sh` requests `SCHED_FIFO` and falls back to normal priority without it.
- `registry_regions()` reaches into `boost::openmethod::detail` for the dispatch
  table arena (`registry_state_type::dispatch_data`). Deliberate: the point of
  `clflush` mode is to evict exactly what the dispatch reads.

## Generated code

Confirming the shapes in `performance.adoc`. Timed region only, gcc 13, `-O2`.

`virtual_ptr`, arity 1 — the three-instruction dispatch (plus the ABI reload):

```asm
mov    rcx, QWORD PTR [rsp]              ; virtual_ptr, passed in memory
mov    rax, QWORD PTR [rsp]
mov    rcx, QWORD PTR [rip+...]          ; poke_vp<R>::fn slot
call   QWORD PTR [rax+rcx*8]
```

`virtual_` reference, arity 1 — hash, then index the vptr vector:

```asm
mov    rax, QWORD PTR [rdi]              ; object's C++ v-table pointer
mov    rdx, QWORD PTR [rip+...]          ; shift
mov    rax, QWORD PTR [rax-0x8]          ; std::type_info*
imul   rax, QWORD PTR [rip+...]          ; * mult
shrx   rax, rax, rdx                     ; >> shift
mov    rdx, QWORD PTR [rip+...]          ; vptr_vector data
mov    rax, QWORD PTR [rdx+rax*8]        ; the open-method v-table
mov    rdx, QWORD PTR [rip+...]          ; poke_ref<R>::fn slot
call   QWORD PTR [rax+rdx*8]
```

`virtual_` reference, arity 2 — two independent hash chains, then the
two-dimensional index:

```asm
mov    r10, QWORD PTR [rsi]
mov    rax, QWORD PTR [rip+...]          ; mult
mov    r9,  QWORD PTR [rip+...]          ; shift
mov    rdx, QWORD PTR [rdi]
mov    rcx, QWORD PTR [rip+...]          ; vptr_vector data
mov    r11, QWORD PTR [rdx-0x8]
imul   r11, rax
imul   rax, QWORD PTR [r10-0x8]
shrx   rdx, r11, r9
mov    rdx, QWORD PTR [rcx+rdx*8]
shrx   rax, rax, r9
mov    rax, QWORD PTR [rcx+rax*8]
mov    rcx, QWORD PTR [rip+...]          ; slot 0
mov    rax, QWORD PTR [rax+rcx*8]
mov    rcx, QWORD PTR [rip+...]          ; slot 1
imul   rax, QWORD PTR [rip+...]          ; * stride
mov    rcx, QWORD PTR [rdx+rcx*8]
call   QWORD PTR [rcx+rax*8]
```

## Files

| file | |
|---|---|
| `src/timing.hpp` | rdtsc harness, cache modes, TSC calibration, statistics |
| `src/hierarchy.hpp` | `Base` + `Derived<N>` and the `inplace_vptr` hierarchy `IBase<R>` + `IDerived<R,N>`, plus the virtual-function yardsticks |
| `src/registries.hpp` | the six dispatch configurations, the six methods, bulk overrider registration |
| `src/main.cpp` | variant table, measurement loop, verification, reporting |
| `matrix.sh` | builds and verifies all four compiler x bitness combinations, N passes each |
| `report.py` | renders the two matrix tables from `results/run*/` |
| `include` | symlink into a Boost checkout's `libs/openmethod/include` (not committed) |

Overriders are registered in bulk over the generated hierarchy through the core
API rather than the `BOOST_OPENMETHOD_*` macros, since `Derived<N>` is a
template. `method<...>::override` takes a *pack* of functions, so one static
registrar object carries all 100:

```cpp
template<class R, std::size_t... I>
auto poke_ref_registrar(std::index_sequence<I...>)
    -> typename poke_ref<R>::template override<poke_ref_impl<R, I>...>;

BOOST_OPENMETHOD_REGISTER(decltype(poke_ref_registrar<R>(all_indices{})));
BOOST_OPENMETHOD_REGISTER(mp::mp_apply<om::use_classes, class_list<R>>);
```
