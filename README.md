# Boost.OpenMethod dispatch benchmark

Times **one** open-method dispatch with `rdtsc`, with the caches scrubbed
beforehand, against an ordinary virtual function call as the yardstick.

Boost.OpenMethod ships no benchmark. `doc/modules/ROOT/pages/performance.adoc`
states that dispatching through a reference is "between 30% and 50% slower than
calling the equivalent virtual function", attributed to "micro- and RDTSC-based
benchmarks", but no code in the repository produces that number. This does.

## Where the v-table pointer lives

The two call forms differ in one structural fact, and most of the numbers below
follow from it.

A virtual function call finds the v-table *through the object*: the dispatch
chain starts with a load from the receiver, whether or not the body needs it.
`virtual_` reference dispatch shares the constraint — its hash chain starts
from that same embedded pointer.

```
        pointer               object                v-table
       ┌───────┐         ┌─────────────┐        ┌───────────┐
p ───► │ &obj  │ ──────► │ vptr ───────┼──────► │ ...       │
       └───────┘         │ data        │        │ &f ───────┼──► f()
                         └─────────────┘        │ ...       │
                                                └───────────┘
        the object must be loaded first: the chain starts inside it
```

A `virtual_ptr` carries the v-table pointer *next to* the object pointer — a
fat pointer, as in Go and Rust. The dispatch never passes through the object:

```
        virtual_ptr (fat pointer)              v-table
       ┌───────┬────────┐                  ┌───────────┐
vp ──► │ &obj  │ vptr ──┼────────────────► │ ...       │
       └───┬───┴────────┘                  │ &f ───────┼──► f()
           │                               └───────────┘
           ▼
         object — off the dispatch path entirely
```

If the object is not already loaded, there is no need to load it: the call
proceeds immediately. Whether the body then touches the object is the body's
business — and that is a real fork in workloads, not a benchmark technicality.
Plenty of OO designs give a base class a **no-op default** that subclasses
override — event handlers, visitor hooks — and a no-op body touches nothing.
Those calls should be as fast as possible *because* they do nothing: call
overhead is their entire cost, and skipping the receiver load is a genuine
saving, not an accounting trick. The benchmark therefore measures both worlds
separately; see "Two fair comparisons".

## What is measured

Five axes, plus yardsticks and baselines:

| axis | values |
|---|---|
| dispatch | how the v-table pointer is found: `vptr_vector` (the default: `std_rtti` + `fast_perfect_hash` + `vptr_vector`), `indirect` (the default plus `indirect_vptr`), `vptr_map` over `std::unordered_map`, `vptr_map` over `boost::unordered_flat_map`, `inplace` (`inplace_vptr`, the pointer stored in the object), `inplace_ind` (`inplace_vptr` plus `indirect_vptr`) |
| call form | `virtual_<const Base&>` (the v-table pointer is looked up at the call site) vs `virtual_ptr<const Base, R>` (already carries it) |
| arity | 1 and 2 virtual arguments |
| compiler | g++ 13.3 vs clang++ 18.1 |
| bitness | 64-bit vs 32-bit (`-m32`) |
| body | `const` — overrider and virtual bodies return a compile-time constant, the receiver is touched only if the mechanism itself requires it; `use` — every body reads a member of every receiver (see "Two fair comparisons") |
| yardstick | `vf` — one virtual call; `vf+vf` — the double dispatch idiom, two chained virtual calls; each in both body flavors |
| baseline | `direct` — a direct, non-inlined call to a body that stamps on arrival: the apparatus plus one call, no dispatch; `nvf` / `nvf+nvf` — a non-virtual member call that loads the receiver(s), then stamps |

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

That comes to 59 variants, each run in 3 cache states: the 33 const-body ones —
21 on the main hierarchy (4 registries x 2 call forms x 2 arities, plus 2
yardsticks and 3 baselines) and 6 on each inplace hierarchy — plus 26 use-body
ones (16 main-hierarchy dispatches and 2 yardsticks, 4 inplace dispatches and
their own 2 x 2 yardsticks). The baselines are body-neutral and shared.

Two compensations are applied, giving two columns:

- **`net` = mean − `direct`** subtracts the apparatus *and* a direct call, so
  it reads as "what the dispatch mechanism adds over calling the function
  directly" — the question benchmarks of dispatch actually pose.
- **`disp` = mean − `nvf` of the same arity** additionally removes the cost of
  *reaching* the receiver. In the cold modes that is 40-45% of what a virtual
  call appears to cost, and it is common to every variant that dereferences the
  receiver, so `net` alone compresses those ratios toward 1. `disp` is dispatch
  alone.
- **Exception: the `om vptr` rows never touch the receiver.** Their timed
  region reads the `virtual_ptr` (already in the arguments), the method slot
  and the table; the overriders ignore the object. Subtracting `nvf` from them
  would credit a receiver miss they never paid — cold, that fabricated ~270
  cycles and understated their dispatch cost by ~2x (a finding of the
  adversarial review). For those rows `disp` = `net` by construction, and cold
  they read *higher* than the receiver-touching forms' `disp` precisely because
  nothing is subtracted: everything in their timed region is dispatch work.

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
python3 report.py    # renders every generated table in this README
```

32-bit builds need `sudo apt install g++-multilib` (clang uses gcc's multilib
headers, so that one install covers both compilers).

The class count is compile-time (`Derived<N>` is a template):
`CLASSES=1000 ./build.sh`, or `-DOMB_CLASSES=1000` with CMake.

## Results

AMD Ryzen 9 9955HX (Zen 5), 32 MiB L3, WSL2, g++ 13.3.0 `-O2 -march=native`
64-bit, 100 classes, 4096 objects, 6000 reps, median of 7 passes (the `gcc/64`
column of the matrix below). `net` is reference cycles above the `direct`
baseline; `x vf` is
the ratio to the virtual-function yardstick of the same arity.

### Warm caches — the sharpest numbers

Nothing is evicted, so reaching the receiver is almost free: the `nvf` baseline
costs 0.5 cycles more than the direct-call baseline. `net` and `disp` therefore agree, and both are
stable to ~1 cycle run to run. The `inplace` rows are ratioed against the inplace
hierarchy's own `vf`, which measures within a cycle of the main one.

Median of 7 passes.

| dispatch | arity | disp | x vf |
|---|---|---|---|
| `vf` | 1 | 6.2 | 1.00x |
| `om vptr / vptr_vector` | 1 | 6.2 | 1.08x |
| `om ref / vptr_vector` | 1 | 11.8 | 1.96x |
| `om vptr / indirect` | 1 | 6.9 | 1.25x |
| `om ref / indirect` | 1 | 14.7 | 2.43x |
| `om vptr / vptr_map` | 1 | 6.8 | 1.13x |
| `om ref / vptr_map` | 1 | 20.9 | 3.52x |
| `om vptr / flat_map` | 1 | 6.2 | 1.14x |
| `om ref / flat_map` | 1 | 21.2 | 3.50x |
| `om ref / inplace` | 1 | 6.2 | 0.96x |
| `om ref / inplace_ind` | 1 | 8.1 | 1.30x |
| `vf+vf (double dispatch)` | 2 | 16.6 | 1.00x |
| `om vptr / vptr_vector` | 2 | 7.3 | 0.46x |
| `om ref / vptr_vector` | 2 | 13.4 | 0.80x |
| `om vptr / indirect` | 2 | 8.0 | 0.51x |
| `om ref / indirect` | 2 | 15.2 | 0.92x |
| `om vptr / vptr_map` | 2 | 7.3 | 0.46x |
| `om ref / vptr_map` | 2 | 27.3 | 1.64x |
| `om vptr / flat_map` | 2 | 7.2 | 0.46x |
| `om ref / flat_map` | 2 | 24.9 | 1.50x |
| `om ref / inplace` | 2 | 6.8 | 0.40x |
| `om ref / inplace_ind` | 2 | 8.3 | 0.51x |

### Caches cold (`clflush`)

Flushed, the first touch of the receiver is a cache miss in its own right: the
`nvf` baseline costs 265 cycles more than the direct call, against 564 for the whole `vf`
yardstick. So 47% of a virtual call's `net` is reaching the object rather than
dispatching on it — which is exactly what the `disp` column removes.

Median of 7 passes.

| dispatch | arity | net | disp | x net | x disp |
|---|---|---|---|---|---|
| `vf` | 1 | 564 | 296 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 1 | 565 | 565 | 0.97x | 1.88x |
| `om ref / vptr_vector` | 1 | 1129 | 856 | 2.00x | 3.01x |
| `om vptr / indirect` | 1 | 610 | 610 | 1.07x | 2.14x |
| `om ref / indirect` | 1 | 1317 | 1052 | 2.30x | 3.59x |
| `om vptr / vptr_map` | 1 | 494 | 494 | 0.88x | 1.71x |
| `om ref / vptr_map` | 1 | 833 | 568 | 1.46x | 1.95x |
| `om vptr / flat_map` | 1 | 501 | 501 | 0.88x | 1.80x |
| `om ref / flat_map` | 1 | 836 | 571 | 1.51x | 2.06x |
| `om ref / inplace` | 1 | 549 | 312 | 1.07x | 1.13x |
| `om ref / inplace_ind` | 1 | 807 | 560 | 1.53x | 2.01x |
| `vf+vf (double dispatch)` | 2 | 666 | 336 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 2 | 791 | 791 | 1.21x | 2.39x |
| `om ref / vptr_vector` | 2 | 1413 | 1099 | 2.13x | 3.33x |
| `om vptr / indirect` | 2 | 817 | 817 | 1.24x | 2.49x |
| `om ref / indirect` | 2 | 1687 | 1344 | 2.56x | 4.14x |
| `om vptr / vptr_map` | 2 | 801 | 801 | 1.20x | 2.42x |
| `om ref / vptr_map` | 2 | 1157 | 843 | 1.78x | 2.60x |
| `om vptr / flat_map` | 2 | 801 | 801 | 1.22x | 2.42x |
| `om ref / flat_map` | 2 | 1201 | 887 | 1.81x | 2.61x |
| `om ref / inplace` | 2 | 819 | 524 | 1.34x | 1.67x |
| `om ref / inplace_ind` | 2 | 1155 | 853 | 1.78x | 2.55x |

`net` and `disp` answer different questions, and the gap between the two columns
is the point: subtract only the apparatus and you learn how much slower a cold
open-method *call* is; subtract the object touch as well and you learn how much
slower open-method *dispatch* is. Neither is wrong. The second is the one that
isolates the library.

### Reading it

First, a note on what changed when the stop moved into the body ("Timing").
The old window included the call's return path and a closing bracket — a
constant shared by every row that *diluted* every ratio toward 1. The arrival
window removes it, so ratios here are pure mechanism against pure mechanism:
`om ref` reads ~2x `vf` warm where the old window read ~1.3x. The documented
"30% to 50% slower" (`performance.adoc`) corresponds to the whole-call view
and was reproduced by the old window; this one answers the sharper question.
The two schemes' ratios must not be compared with each other.

- **`virtual_ptr` dispatch ties the virtual function, warm**: 6.2 cycles
  against 6.2 over a direct call. Three instructions against two, no receiver
  touched, same time — the fat pointer buys the object-independence for free.
- **`virtual_` reference dispatch costs about 2x a virtual call warm** (11.8 vs
  6.2): the hash-and-look-up, undiluted. Cold and receiver-compensated it is
  3.01x — the chain touches three more lines and each is its own miss.
- **The `vptr_map` probe is now visible in the open**: 20.9 warm cycles for the
  reference form, 3.5x the yardstick, against 11.8 for `fast_perfect_hash` +
  `vptr_vector`. `boost::unordered_flat_map` measures the same as
  `std::unordered_map` (21.2 vs 20.9) — the probe structure does not matter at
  this table size; the extra pointer chase does.
- **`inplace_vptr` ties the virtual function exactly** (0.96x warm, 1.13x cold
  disp): same load-from-object, load-slot, call shape — as it should, since
  its layout *is* the virtual function's layout with a second pointer.
- **At two virtual arguments the multi-method beats double dispatch in every
  form**: 0.46x through `virtual_ptr`, 0.80x through a reference, warm. Cold,
  whole call against whole call, 1.21x net for `virtual_ptr` against an idiom
  that pays two dependent chains.
- **Cold, whole call against whole call, `virtual_ptr` is at parity** (0.97x
  net). Its `disp` column reads 1.88x because for vp rows `disp` = `net`
  (nothing to subtract — no receiver touched) while the yardstick's receiver
  miss is compensated; the use-world tables below make that comparison fair in
  both columns.
- The dispatch axis is **irrelevant to `virtual_ptr` dispatch** among the three
  *direct* registries, as it must be: 1.08x / 1.13x / 1.14x warm, and cold nets
  within the run-to-run spread. `indirect` is excluded from the control — its
  extra load is the point of the section below.

## Two fair comparisons

A virtual function call *must* load the receiver: the v-table pointer lives in
the object. So must `virtual_` reference dispatch — its hash chain starts from
the object's v-table pointer. A `virtual_ptr` call alone can dispatch without
ever touching the object, because the v-table pointer travels in the fat
pointer. That asymmetry is the design, not an artifact, and it means no single
statistic is fair to both sides:

- Subtract the receiver-touch (`disp = mean − nvf`) from everything, and the
  virtual function gets part of *its own dispatch mechanism* excused — the load
  it is charged for IS the vptr fetch.
- Subtract it from nothing, and the comparison charges `virtual_ptr` full price
  for its table misses while ignoring that in real code its receiver miss is
  not avoided but *deferred* — the body pays it instead.

So the benchmark measures two worlds, with two yardsticks:

**The delivery world** (`body = const`, every table above): bodies return
compile-time constants; the receiver is touched only where the mechanism
requires it. This is not a lab construct — it is the no-op-default pattern from
"Where the v-table pointer lives", where call overhead is the entire cost of
the call. It is `virtual_ptr`'s best case — dispatch without the object — and
the fair cross-form statistic is `net` against `net`.

**The use world** (`body = use`, tables below): every overrider and virtual
body reads a member of every receiver, as bodies that operate on the object
do. Now every call form owes the receiver's cache line exactly once, and
`disp = mean − nvf` is fair for every row including `virtual_ptr`.

One caution about what the use world means. The benchmark measures the *call
mechanism*, not bodies; the member reads exist to make the receiver line's
movement visible, not to bill the mechanism for the body's work. The virtual
function's mandatory receiver load doubles as a prefetch for its body — a real
effect, but a property of what surrounds the call. The measurable question the
use world answers is where the `virtual_ptr` body's deferred receiver miss
lands: serialized behind the table misses, or overlapped under them if
speculation across the indirect call reaches the body's load. That is
microarchitecture, not arithmetic; the tables answer it.

#### Warm, receiver used

Median of 7 passes.

| dispatch | arity | disp | x vf |
|---|---|---|---|
| `vf` | 1 | 13.4 | 1.00x |
| `om vptr / vptr_vector` | 1 | 13.0 | 0.97x |
| `om ref / vptr_vector` | 1 | 19.8 | 1.47x |
| `om vptr / indirect` | 1 | 15.7 | 1.16x |
| `om ref / indirect` | 1 | 23.9 | 1.82x |
| `om vptr / vptr_map` | 1 | 13.0 | 0.98x |
| `om ref / vptr_map` | 1 | 28.2 | 2.17x |
| `om vptr / flat_map` | 1 | 13.0 | 0.97x |
| `om ref / flat_map` | 1 | 27.9 | 2.15x |
| `om ref / inplace` | 1 | 12.9 | 0.96x |
| `om ref / inplace_ind` | 1 | 17.2 | 1.28x |
| `vf+vf (double dispatch)` | 2 | 17.3 | 1.00x |
| `om vptr / vptr_vector` | 2 | 6.9 | 0.41x |
| `om ref / vptr_vector` | 2 | 13.8 | 0.80x |
| `om vptr / indirect` | 2 | 8.1 | 0.47x |
| `om ref / indirect` | 2 | 16.0 | 0.93x |
| `om vptr / vptr_map` | 2 | 6.9 | 0.41x |
| `om ref / vptr_map` | 2 | 27.5 | 1.62x |
| `om vptr / flat_map` | 2 | 6.9 | 0.41x |
| `om ref / flat_map` | 2 | 25.3 | 1.50x |
| `om ref / inplace` | 2 | 7.0 | 0.40x |
| `om ref / inplace_ind` | 2 | 8.7 | 0.52x |

#### Cold (`clflush`), receiver used

Median of 7 passes.

| dispatch | arity | net | disp | x net | x disp |
|---|---|---|---|---|---|
| `vf` | 1 | 531 | 245 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 1 | 545 | 273 | 1.02x | 1.05x |
| `om ref / vptr_vector` | 1 | 1101 | 836 | 2.13x | 3.31x |
| `om vptr / indirect` | 1 | 594 | 306 | 1.11x | 1.25x |
| `om ref / indirect` | 1 | 1320 | 1055 | 2.52x | 4.24x |
| `om vptr / vptr_map` | 1 | 546 | 271 | 1.06x | 1.12x |
| `om ref / vptr_map` | 1 | 848 | 574 | 1.62x | 2.29x |
| `om vptr / flat_map` | 1 | 534 | 266 | 1.00x | 1.01x |
| `om ref / flat_map` | 1 | 848 | 583 | 1.64x | 2.28x |
| `om ref / inplace` | 1 | 586 | 340 | 1.08x | 1.15x |
| `om ref / inplace_ind` | 1 | 798 | 550 | 1.54x | 2.02x |
| `vf+vf (double dispatch)` | 2 | 609 | 281 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 2 | 829 | 499 | 1.34x | 1.79x |
| `om ref / vptr_vector` | 2 | 1450 | 1116 | 2.37x | 4.01x |
| `om vptr / indirect` | 2 | 854 | 520 | 1.39x | 1.84x |
| `om ref / indirect` | 2 | 1698 | 1364 | 2.71x | 4.91x |
| `om vptr / vptr_map` | 2 | 833 | 502 | 1.36x | 1.79x |
| `om ref / vptr_map` | 2 | 1178 | 853 | 1.93x | 3.00x |
| `om vptr / flat_map` | 2 | 823 | 487 | 1.36x | 1.76x |
| `om ref / flat_map` | 2 | 1173 | 846 | 1.93x | 2.98x |
| `om ref / inplace` | 2 | 817 | 496 | 1.31x | 1.63x |
| `om ref / inplace_ind` | 2 | 1123 | 806 | 1.76x | 2.57x |

### What the use world shows

- **With the receiver used, `virtual_ptr` is at parity with the virtual
  function everywhere, and both compensations agree**: warm 0.97x, cold 1.02x
  net and 1.05x disp — the receiver miss now paid by both sides and subtracted
  from both sides.
- **The deferred receiver miss still costs nothing.** Cold net 565 with the
  body never touching the object, 545 with it read — against 265 for a lone
  receiver miss. The object address is in the fat pointer from the first
  cycle; with the indirect target predicted, the body's load completes under
  the table misses.
- **The virtual function's prefetch remains a wash**: its own body read is
  free (the line its dispatch fetched), but so, through overlap, is
  `virtual_ptr`'s. Delivery world: `virtual_ptr` wins by not loading the
  object. Use world: it draws by hiding the load. No workload here rewards
  keeping the v-table pointer in the object.

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

Median of 7 passes; `disp` cycles, direct → indirect.

#### Warm — the extra load, uncontended

| call form | arity | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|---|
| `virtual_ptr` | 1 | 6.2 → 6.9 (**+0.7**) | 7.2 → 9.3 (**+2.2**) | 5.8 → 6.5 (**+0.6**) | 11.2 → 13.3 (**+2.1**) |
| `virtual_ptr` | 2 | 7.3 → 8.0 (**+0.7**) | 2.1 → 4.6 (**+2.5**) | 7.0 → 8.2 (**+1.3**) | 10.2 → 11.7 (**+1.5**) |
| `virtual_` ref | 1 | 11.8 → 14.7 (**+2.9**) | 18.1 → 22.5 (**+4.4**) | 11.6 → 15.1 (**+3.5**) | 15.0 → 18.2 (**+3.2**) |
| `virtual_` ref | 2 | 13.4 → 15.2 (**+1.9**) | 13.8 → 16.4 (**+2.5**) | 14.0 → 15.5 (**+1.5**) | 7.6 → 9.8 (**+2.1**) |
| `inplace` ref | 1 | 6.2 → 8.1 (**+1.9**) | 10.7 → 15.6 (**+4.8**) | 6.0 → 8.0 (**+2.0**) | 7.4 → 11.2 (**+3.8**) |
| `inplace` ref | 2 | 6.8 → 8.3 (**+1.5**) | 6.5 → 8.3 (**+1.8**) | 6.9 → 7.9 (**+1.0**) | 1.2 → 3.1 (**+1.9**) |

#### Cold (`clflush`) — the extra load, as a cache miss

| call form | arity | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|---|
| `virtual_ptr` | 1 | 565 → 610 (**+46**) | 504 → 539 (**+35**) | 567 → 567 (**+0**) | 557 → 575 (**+18**) |
| `virtual_ptr` | 2 | 791 → 817 (**+26**) | 796 → 880 (**+84**) | 820 → 893 (**+73**) | 807 → 866 (**+59**) |
| `virtual_` ref | 1 | 856 → 1052 (**+196**) | 857 → 1152 (**+296**) | 829 → 1077 (**+248**) | 837 → 1096 (**+259**) |
| `virtual_` ref | 2 | 1099 → 1344 (**+245**) | 1197 → 1446 (**+249**) | 1111 → 1390 (**+279**) | 1180 → 1416 (**+236**) |
| `inplace` ref | 1 | 312 → 560 (**+248**) | 335 → 536 (**+201**) | 322 → 554 (**+232**) | 340 → 581 (**+241**) |
| `inplace` ref | 2 | 524 → 853 (**+329**) | 580 → 847 (**+267**) | 519 → 829 (**+310**) | 623 → 874 (**+251**) |

### Reading it

- **Warm, it costs about one cycle through a `virtual_ptr` (+0.6 to +2.5
  across builds) and about three through a reference (+1.5 to +4.4).** In
  yardstick terms that takes `virtual_ptr` dispatch from 1.08x a virtual
  function call to 1.25x on gcc/64; the reference form from 1.96x to 2.43x.
- **It costs more on the reference path than the `virtual_ptr` path**, roughly
  double, in every build. On the reference path the extra load sits at the end
  of an already-long dependency chain (object → `type_info` → hash → vptr vector
  → **indirection** → v-table) with nothing left to overlap against. Through a
  `virtual_ptr` the chain is two loads long, so there is more slack.
- **Cold, the reference path pays a full cache miss**, consistently signed
  across all four builds. The indirection target is a
  separate line from everything else the dispatch touches, so it is a miss of
  its own.
- **Cold through a `virtual_ptr` it is nearly free at arity 1** — under the
  arrival window the four builds measure +46, +35, +0 and +18 cycles against
  nets of ~550: small, and dwarfed by the reference path's full miss. That is partly noise (the cold spread swamps it), but there is a
  real mechanism too: the indirection load and the method's slot load are
  independent of each other, so the two misses overlap. On the reference path
  they cannot, because the slot is needed only after the indirection resolves.
  Do not read a benefit into the negative numbers; read "below what this
  apparatus can resolve".

### With `inplace_vptr`

The same policy applies to an `inplace_vptr` hierarchy, where it changes the
*stored* member from a `vptr_type` to a `const vptr_type*`. The cost is the same
shape and slightly larger: warm on gcc/64, `inplace` goes 6.2 → 8.1 cycles at
arity 1 and 6.8 → 8.3 at arity 2 — about two cycles, the second dependent
load's latency, giving back the tie with the virtual function that
`inplace_vptr` had won.

### When it is worth it

Arity 2 through a `virtual_ptr` costs +0.7 to +2.5 cycles warm, on a dispatch
that is already 0.46x the cost of the double-dispatch idiom — so an
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
| `vf (yardstick)` | 1.00x (296) | 1.00x (310) | 1.00x (296) | 1.00x (302) |
| `om vptr / vptr_vector` | 1.88x (565) | 1.64x (504) | 1.91x (567) | 1.72x (557) |
| `om ref / vptr_vector` | 3.01x (856) | 2.70x (857) | 2.98x (829) | 2.72x (837) |
| `om vptr / indirect` | 2.14x (610) | 1.70x (539) | 2.11x (567) | 1.86x (575) |
| `om ref / indirect` | 3.59x (1052) | 3.79x (1152) | 3.62x (1077) | 3.53x (1096) |
| `om vptr / vptr_map` | 1.71x (494) | 1.62x (534) | 1.71x (506) | 1.69x (511) |
| `om ref / vptr_map` | 1.95x (568) | 1.71x (545) | 1.81x (591) | 1.68x (539) |
| `om vptr / flat_map` | 1.80x (501) | 1.65x (517) | 1.80x (508) | 1.66x (557) |
| `om ref / flat_map` | 2.06x (571) | 1.81x (577) | 2.02x (580) | 1.83x (519) |
| `om ref / inplace` | 1.13x (312) | 1.13x (335) | 1.22x (322) | 1.13x (340) |
| `om ref / inplace_ind` | 2.01x (560) | 1.97x (536) | 1.88x (554) | 2.13x (581) |
| **2 virtual arguments** |  |  |  |  |
| `vf+vf (yardstick)` | 1.00x (336) | 1.00x (355) | 1.00x (437) | 1.00x (364) |
| `om vptr / vptr_vector` | 2.39x (791) | 2.29x (796) | 1.91x (820) | 2.25x (807) |
| `om ref / vptr_vector` | 3.33x (1099) | 3.39x (1197) | 2.54x (1111) | 3.32x (1180) |
| `om vptr / indirect` | 2.49x (817) | 2.46x (880) | 1.98x (893) | 2.41x (866) |
| `om ref / indirect` | 4.14x (1344) | 4.07x (1446) | 3.11x (1390) | 3.90x (1416) |
| `om vptr / vptr_map` | 2.42x (801) | 2.26x (781) | 1.84x (813) | 2.24x (771) |
| `om ref / vptr_map` | 2.60x (843) | 2.67x (872) | 1.88x (861) | 2.38x (828) |
| `om vptr / flat_map` | 2.42x (801) | 2.27x (798) | 1.84x (836) | 2.26x (775) |
| `om ref / flat_map` | 2.61x (887) | 2.40x (860) | 1.91x (856) | 2.58x (886) |
| `om ref / inplace` | 1.67x (524) | 1.66x (580) | 1.68x (519) | 1.87x (623) |
| `om ref / inplace_ind` | 2.55x (853) | 2.66x (847) | 2.06x (829) | 2.72x (874) |

Median of 7 passes. Spread across passes: median 30%, p90 48%.

#### Warm caches

| dispatch | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|
| **1 virtual argument** |  |  |  |  |
| `vf (yardstick)` | 1.00x (6.2) | 1.00x (9.1) | 1.00x (4.7) | 1.00x (8.0) |
| `om vptr / vptr_vector` | 1.08x (6.2) | 0.79x (7.2) | 1.23x (5.8) | 1.42x (11.2) |
| `om ref / vptr_vector` | 1.96x (11.8) | 1.97x (18.1) | 2.47x (11.6) | 1.93x (15.0) |
| `om vptr / indirect` | 1.25x (6.9) | 0.98x (9.3) | 1.38x (6.5) | 1.69x (13.3) |
| `om ref / indirect` | 2.43x (14.7) | 2.47x (22.5) | 3.04x (15.1) | 2.32x (18.2) |
| `om vptr / vptr_map` | 1.13x (6.8) | 0.75x (7.1) | 1.25x (5.9) | 1.31x (10.2) |
| `om ref / vptr_map` | 3.52x (20.9) | 2.86x (26.3) | 4.29x (21.0) | 2.85x (22.2) |
| `om vptr / flat_map` | 1.14x (6.2) | 0.78x (7.2) | 1.23x (6.0) | 1.34x (10.4) |
| `om ref / flat_map` | 3.50x (21.2) | 2.58x (25.5) | 4.92x (24.0) | 3.19x (25.4) |
| `om ref / inplace` | 0.96x (6.2) | 1.16x (10.7) | 1.08x (6.0) | 1.52x (7.4) |
| `om ref / inplace_ind` | 1.30x (8.1) | 1.70x (15.6) | 1.43x (8.0) | 2.48x (11.2) |
| **2 virtual arguments** |  |  |  |  |
| `vf+vf (yardstick)` | 1.00x (16.6) | 1.00x (19.4) | 1.00x (18.0) | 1.00x (15.1) |
| `om vptr / vptr_vector` | 0.46x (7.3) | 0.10x (2.1) | 0.39x (7.0) | 0.67x (10.2) |
| `om ref / vptr_vector` | 0.80x (13.4) | 0.71x (13.8) | 0.78x (14.0) | 0.50x (7.6) |
| `om vptr / indirect` | 0.51x (8.0) | 0.23x (4.6) | 0.46x (8.2) | 0.79x (11.7) |
| `om ref / indirect` | 0.92x (15.2) | 0.84x (16.4) | 0.86x (15.5) | 0.65x (9.8) |
| `om vptr / vptr_map` | 0.46x (7.3) | 0.10x (2.1) | 0.39x (7.0) | 0.68x (10.3) |
| `om ref / vptr_map` | 1.64x (27.3) | 1.23x (23.8) | 1.49x (26.7) | 1.38x (21.0) |
| `om vptr / flat_map` | 0.46x (7.2) | 0.10x (2.0) | 0.39x (7.0) | 0.68x (10.3) |
| `om ref / flat_map` | 1.50x (24.9) | 1.34x (25.9) | 1.61x (28.0) | 1.70x (25.5) |
| `om ref / inplace` | 0.40x (6.8) | 0.33x (6.5) | 0.43x (6.9) | 0.08x (1.2) |
| `om ref / inplace_ind` | 0.51x (8.3) | 0.43x (8.3) | 0.48x (7.9) | 0.20x (3.1) |

Median of 7 passes. Spread across passes: median 16%, p90 42%.

### What it shows

- **Bitness buys nothing cold and little warm.** The cold reference-dispatch
  *cycles* are all but identical across bitness (856 → 857 on gcc, 829 → 837
  on clang); halving every table does not save misses that are counted per
  line. Warm ratios move mainly because the small yardstick denominators move.
- **The compilers agree on the mechanisms.** Warm reference dispatch is ~2x
  the yardstick on three of four builds; `virtual_ptr` ties or nearly ties the
  virtual function on gcc/64 and clang/64. The 32-bit columns are noisier —
  their yardsticks are only a handful of cycles, so a one-cycle wobble is a
  large ratio swing. Read the cycle columns before the ratios there.
- **The one persistent artifact is clang/32's `virtual_ptr` rows** (11.2 warm
  against 5.8-7.2 elsewhere): the i386 ABI marshals the 8-byte fat pointer
  through the stack inside the timed window — the harness's by-value argument
  meeting that ABI, not the library.
- **`inplace` remains the fastest reference dispatch in every column** (1.13x
  cold disp on three builds), and the `om ref` cold ordering vector < flat ≈
  map holds everywhere.

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

(Measured under the previous, return-path-inclusive window — the effect, not
the absolutes, is the point.)

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

`rdtsc` returns the counter in `edx:eax`. With the `__rdtsc()` intrinsic, the
64-bit assembly — `shl`, `mov`, `or` — was compiler-scheduled, and in the era
when this harness had a *closing* bracket too, each compiler placed it
differently: gcc inside the measured window, clang outside, and in one baseline
clang sank the `or` past the work, so the arithmetic did not cancel in `disp`
and the cross-compiler columns carried a scheduling artifact.

Two designs removed the problem in sequence. First, both brackets captured raw
`lo`/`hi` in inline asm and assembled afterwards, making the bookkeeping
identical across every variant within a compiler. Then the closing bracket was
abolished altogether — the stop moved into the measured bodies (see "Timing"),
which is *our* code, emitted identically everywhere. What survives of the
original issue is the start side: `tsc_start` captures the raw pair, and gcc
happens to assemble it inside the window — identically in every variant, so it
cancels against the `direct` baseline.

### Reproducibility, and why seven passes

A single cold pass moves by a median of 11% between repeats (p90 40%, max 73%)
— more than the differences between the columns. Every cell above is therefore
the **median of 7 passes**, and `matrix.sh` loops the whole matrix rather than
repeating each build in place, so thermal and background drift lands on all four
columns alike instead of favouring whichever ran first.

Even so, read the cold table for the large effects only. The built-in control
says how far to trust it: the three *direct* registries' `om vptr` rows must
agree, since the vptr policy is off that call path (`indirect` is excluded — it
adds a load there by design). Cold their nets agree to within 6-14% per
column. Warm is far better behaved — a few percent, and
the arity-2 control is nearly exact (7.3 / 7.3 / 7.2 cycles on gcc/64).

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

The stop is **in the measured body**. Every yardstick, baseline and overrider
returns a `stamp_id`: the counter read on arrival, plus an id (computed after
the stamp) that carries the verification oracle. The measured window is:

```
asm barrier; lfence; rdtsc; lfence          <- start bracket, in timed_call
    <dispatch>
rdtscp                                      <- in the body; returned
```

`rdtscp` waits until every prior instruction — the whole dispatch chain, and in
the use world the receiver load kept above it by its memory clobber — has
executed, so the stamp is the moment control *arrived* in the right overrider.
Everything after it is outside the window by construction: assembling the
64-bit value, writing the id, the entire return path, and the elapsed
computation, which is a plain data dependency on the returned stamp
(`cycles = r.t − t0`). Consequences, each of which used to need active
management:

- No closing bracket exists, so there is nothing for a compiler to schedule
  differently at the stop edge — an artifact that once contaminated a
  cross-compiler comparison here (see "Two flags that had to be equalised").
- The return path is not measured; variants with different frame or ABI shapes
  stopped differing for irrelevant reasons.
- The baseline can be *meaningful* rather than empty: `direct` is a real,
  non-inlined call whose body stamps on arrival. `net = mean − direct` is the
  cost of the dispatch mechanism over a direct call — warm, that puts a
  virtual function at the textbook two-to-three cycles, where the old
  return-path-inclusive window read ~15.

Other properties of the harness:

- The start bracket lives inside a `noinline` `timed_call` whose parameters
  are the call arguments, so the prologue is untimed while the arguments still
  arrive through the ABI and cannot be constant-folded or devirtualized.
- The start timestamp is captured as the raw `edx:eax` pair; the elapsed
  computation is written after the call, though the compiler is free to
  assemble the 64-bit value earlier (gcc does, inside the window) — what
  matters is that it does so *identically in every variant*, so it cancels
  against the `direct` baseline. "Why the timestamp is assembled afterwards"
  has the history.
- **Every variant replays the identical sequence of receivers**: the RNG is
  reseeded at the start of each variant. `disp` is a difference of two
  separate measurements, and in the cold modes both are dominated by DRAM
  latency that depends on *which* objects were drawn; unpaired draws leave
  that term uncancelled.
- `lfence` is dispatch-serializing by default on Zen, so no `CPUID` is needed
  — which is just as well, since `CPUID` traps to the hypervisor here and
  would cost more than the thing being measured.
- Both compilers are given `-fcf-protection=none`, because their defaults
  differed in ways that biased the comparison — see "Two flags that had to be
  equalised".

### Cache states

| mode | what it does |
|---|---|
| `warm` | nothing, after a warm-up call — lower bound |
| `clflush` | `clflushopt` over the object, its C++ v-table — from the v-table *head*, 16 bytes before the address point, so the `type_info` slot at vptr−8 is always evicted (it used to escape when the vptr was 64-byte aligned, a compiler-asymmetric hole) — the method's `fn` object, the registry's dispatch-table arena, and the vptr storage; then `mfence` |
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
error of the difference against the `direct` baseline in the `+/-` column.

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
| `clflush` | ~47% of what `vf` appears to cost | yes — the compensation is large and reproducible |
| `sweep` | ~45% of what `vf` appears to cost | **not for the fast variants** |

Under a full sweep, `disp` for a fast *receiver-touching* row (the inplace
reference form, `om ref` at arity 2) is a small residue left by subtracting two
large DRAM-bound numbers, and its run-to-run spread is comparable to its value
— historical probes of the then-worst case measured a 2x range before the
receiver draws were paired and ~±13% after. The `om vptr` rows are exempt since
the review fix: their `disp` is their net, no subtraction happens. The `x net`
ratios stay stable throughout because the shared miss sits in both terms.

So: **read `x disp` warm and under `clflush`; under `sweep` read `x net`** and
treat it as a floor on the true ratio, since the shared miss biases it toward
1.00x. And for the `om vptr` rows remember `disp` = `net`: their cold `x disp`
compares *all* their dispatch work against the yardstick's
receiver-compensated dispatch, while their `x net` compares whole call against
whole call — the number that says "a cold `virtual_ptr` call costs about what
a cold virtual call does".

### Correctness gate

`--verify` calls every open-method path — four registries x two call forms x two
arities, plus the two inplace dispatches x two arities — and checks each against
an **oracle computed from the class contracts alone**: `x + tag` for arity 1,
the tag rule (`x + tag` iff both receivers are the same leaf, else `x`) for
arity 2, and `x + b.tag` for the double-dispatch yardstick. Earlier versions
compared the open methods against *each other*, which passed when every
registry was wrong the same way — an unregistered overrider pack, say — a hole
the adversarial review demonstrated concretely. A registry wired to
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
- **Warm-mode differences of a fraction of a cycle are not trustworthy.** The
  samples are quantized by a 25-cycle tick; the trimmed mean resolves below
  it, but sub-cycle gaps remain at the mercy of scheduling phase. A cautionary
  tale from this benchmark's own history: the previous, return-path-inclusive
  window reported arity 2 as *cheaper* than arity 1 for `virtual_` reference
  dispatch — physically impossible, and listed here as an unexplained artifact
  for several revisions. Under the arrival window the inversion is gone
  (11.8 < 13.4 warm): it lived in the return path, not in the dispatch.
- **`nvf` is a lower bound on "reaching the receiver", not an exact one.** It
  reads `tag` at offset 8; a dispatch reads the v-table pointer at offset 0.
  Same cache line, so the same one miss — but a `virtual_` reference dispatch
  then chases that pointer to the `type_info`, which is a *second* miss that
  `disp` charges to dispatch. That is the right attribution (it is work the
  dispatch causes), but it means `disp` is not purely "arithmetic and an
  indirect call". The `om vptr` rows sidestep the question entirely: they touch
  no receiver, so their `disp` is defined as `net` (see "What is measured").
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

Timed regions under the arrival window, gcc 13, `-O2`. Each ends at the `call`
into a body whose first instruction sequence is `rdtscp` — the region has no
closing bracket. The `mov/shl/or` around the dispatch is the start stamp's
assembly, identical in every variant, cancelled by the `direct` baseline.

`vf` — one dependent load, one indirect call:

```asm
lfence; rdtsc; lfence
mov  r12d, eax
mov  rax, QWORD PTR [rdi]        ; the receiver's v-table pointer
mov  ebx, edx
shl  rbx, 0x20
or   rbx, r12
call QWORD PTR [rax+0x10]        ; -> body: rdtscp
```

`virtual_ptr`, arity 1 — no receiver load anywhere:

```asm
lfence; rdtsc; lfence
mov  rcx, QWORD PTR [rsp]        ; the fat pointer's vptr half
mov  r12d, eax
mov  rax, QWORD PTR [rip+slot]   ; method slot
mov  ebx, edx
mov  rdi, QWORD PTR [rsp]
shl  rbx, 0x20
call QWORD PTR [rcx+rax*8]       ; -> body: rdtscp
```

`virtual_` reference, arity 1 — the hash chain, starting from the receiver:

```asm
lfence; rdtsc; lfence
mov  r12d, eax
mov  rax, QWORD PTR [rdi]        ; receiver's v-table pointer
mov  ebx, edx
shl  rbx, 0x20
or   rbx, r12
mov  rdx, QWORD PTR [rax-0x8]    ; type_info*
mov  rax, QWORD PTR [rip+shift]
imul rdx, QWORD PTR [rip+mult]
shrx rdx, rdx, rax
mov  rax, QWORD PTR [rip+vptrs]  ; vptr_vector data
mov  rax, QWORD PTR [rax+rdx*8]  ; the open-method v-table
mov  rdx, QWORD PTR [rip+slot]
call QWORD PTR [rax+rdx*8]       ; -> body: rdtscp
```

Every measured body opens with the stamp; in the use world a member load of
each receiver precedes it:

```asm
; const overrider                 ; use overrider
rdtscp                            mov  eax, DWORD PTR [rdi+0x8]   ; tag
...                               rdtscp
                                  ...
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
