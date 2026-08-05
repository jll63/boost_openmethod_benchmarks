# Boost.OpenMethod dispatch benchmarks

Times **one** open-method dispatch with `rdtsc`, caches deliberately scrubbed,
against a virtual function call as the yardstick. The headline, on this
machine: a `virtual_ptr` call costs exactly what a virtual function call costs
(1.00x warm, 1.03x cold), `virtual_` reference dispatch costs about 2x a
virtual call cold — a figure stable within 4% across two compilers and two
bitnesses — and at two virtual arguments the winner depends on temperature: an
open multi-method through `virtual_ptr` halves the double-dispatch idiom's
cost warm, while cold the idiom's two lean chains win back a modest 14%.

Boost.OpenMethod's documentation cites "micro- and RDTSC-based benchmarks" for
its performance claims; this repository is such a benchmark, built to be
auditable: every table is generated from the committed data by `report.py`,
and every dispatch path is checked against contract-derived oracles before a
single measurement runs.

## Where the v-table pointer lives

The two call forms differ in one structural fact, and most of the numbers
below follow from it.

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
saving. The benchmark therefore measures two body worlds; see "Two fair
comparisons".

## What is measured

Six axes:

| axis | values |
|---|---|
| dispatch | how the v-table pointer is reached: `vptr_vector` (the default registry: `std_rtti` + `fast_perfect_hash` + `vptr_vector`), `indirect` (the default plus `indirect_vptr`), `vptr_map` over `std::unordered_map`, `vptr_map` over `boost::unordered_flat_map`, `inplace` (`inplace_vptr`: the pointer stored in the object), `inplace_ind` (`inplace_vptr` plus `indirect_vptr`) |
| call form | `virtual_<const Base&>` (the v-table pointer is looked up at the call site) vs `virtual_ptr<const Base, R>` (already carries it) |
| arity | 1 and 2 virtual arguments |
| body | `const` — bodies return a compile-time constant, the receiver is touched only where the mechanism requires it; `use` — every body reads a member of every receiver |
| compiler | g++ 13.3 vs clang++ 18.1 |
| bitness | 64-bit vs 32-bit (`-m32`) |

All six dispatch values are registries in the code; the last two have no
`vptr` policy at all — the pointer lives in the object via the `inplace_vptr`
mixin, so those two are measured through a reference only (a `virtual_ptr`
would carry a pointer the object already holds). They also need their own
class hierarchy — an inplace class binds to exactly one registry — with their
own yardsticks and baselines. Every inplace ratio divides by its own
hierarchy's yardstick; the yardsticks agree warm to a fraction of a cycle on
most builds (2.6 cycles on clang/32), and cold differ ~10% — which is exactly
why per-hierarchy yardsticks exist.

Two yardsticks, in both body flavors: `vf`, one virtual call, and `vf+vf`, the
double-dispatch idiom — two chained virtual calls. (The idiom is modeled, not
spelled out: one `dd_with` per leaf in the base is unwritable at 100 leaves
and would cost the same two dependent dispatches.)

Four baselines calibrate everything:

- **`probe`** — the timing machinery with nothing inside it. Its cost is the
  measurement apparatus; subtracting it from a row gives **`net`, the call's
  true cost**.
- **`direct`** — a plain (non-virtual, non-inlined) function call, quoted with
  the tables for scale. It is never subtracted from the headline.
- **`touch` / `touch+touch`** — a plain call that also loads the receiver(s).
  Subtracting it isolates dispatch from the cost of reaching the object
  (**`disp`**).

That comes to 60 variants per cache state: 26 const-body and 26 use-body
dispatch-and-yardstick rows (per world: 16 main-hierarchy dispatches, 2 main
yardsticks, and 2 dispatches + 2 yardsticks on each inplace hierarchy), plus
8 body-neutral baselines. Two cache states are published, warm and cold
(`clflush`). A third state, a 64 MiB
cache sweep, exists in the binary as a diagnostic but is not part of the
published dataset.

### What the ratios divide

Every row yields two costs, and each is divided by the yardstick's same cost
to make a ratio:

- **`net` = mean − `probe`**: the whole call, with only the measurement
  apparatus removed.
- **`disp`**: the dispatch machinery alone — the row minus a baseline that
  does everything *except* dispatch. Receiver-touching rows subtract `touch`
  (a plain call that loads the receiver); the const-body `om vptr` rows,
  which never touch the receiver, subtract `direct` (a plain call).

Worked example — warm, gcc/64, `om ref` against the `vf` yardstick:

| | `vf` | `om ref` | ratio |
|---|---|---|---|
| whole call (`net`) | 10.4 | 16.1 | **1.55x = `x net`** |
| − the `touch` baseline | −4.2 | −4.2 | |
| machinery alone (`disp`) | 6.1 | 11.9 | **1.94x = `x disp`** |

Same two rows, two honest ratios, answering different questions. `x net`
answers the caller's: *if I replace this virtual call with an open-method
call, how much slower is the call?* — half again as slow. `x disp` answers
the implementer's: *how much more work does the lookup machinery itself do?*
— nearly twice as much. The second always reads larger, mechanically:
subtracting the same 4.2 cycles from both sides of a division pushes the
ratio away from 1. Neither is wrong; **`x net` is the headline in every
table, `x disp` the diagnostic.**

## Results

AMD Ryzen 9 9955HX (Zen 5), 32 MiB L3, WSL2, 100 classes, 4096 objects
shuffled in memory, 6000 reps per pass, median of 7 interleaved passes. This
section is the gcc/64 column; the full compiler x bitness matrix follows.
Cycles are TSC reference cycles (~2.45 GHz here — the core clock is not
observable under the hypervisor).

### Warm caches — the finest resolution

Warm mode resolves the mechanisms' few-cycle differences: reaching the receiver
costs 0.2 cycles here, and rows repeat within a build to a percent or two.
But the yardstick is mostly indirect-branch misprediction, which depends on the
binary's layout — across the four builds its net ranges severalfold — so warm
*ratios* are build-local. For figures that transfer, read the cold table below.
The `inplace` rows are ratioed against the inplace hierarchy's own `vf`.

Median of 7 passes.

For scale: a direct call to a stamping body measures 3.7 cycles net here.

| dispatch | arity | net | x vf | disp |
|---|---|---|---|---|
| `vf` | 1 | 10.4 | 1.00x | 6.1 |
| `om vptr / vptr_vector` | 1 | 10.2 | 1.00x | 6.1 |
| `om ref / vptr_vector` | 1 | 16.1 | 1.56x | 11.9 |
| `om vptr / indirect` | 1 | 10.9 | 1.08x | 6.8 |
| `om ref / indirect` | 1 | 18.8 | 1.85x | 14.6 |
| `om vptr / vptr_map` | 1 | 10.3 | 1.01x | 6.3 |
| `om ref / vptr_map` | 1 | 24.6 | 2.46x | 20.6 |
| `om vptr / flat_map` | 1 | 10.2 | 1.00x | 6.2 |
| `om ref / flat_map` | 1 | 25.6 | 2.48x | 21.5 |
| `om ref / inplace` | 1 | 10.3 | 1.00x | 6.1 |
| `om ref / inplace_ind` | 1 | 12.2 | 1.21x | 8.1 |
| `vf+vf (double dispatch)` | 2 | 21.0 | 1.00x | 16.9 |
| `om vptr / vptr_vector` | 2 | 10.8 | 0.52x | 7.0 |
| `om ref / vptr_vector` | 2 | 17.4 | 0.83x | 13.2 |
| `om vptr / indirect` | 2 | 11.7 | 0.55x | 7.9 |
| `om ref / indirect` | 2 | 19.4 | 0.92x | 15.3 |
| `om vptr / vptr_map` | 2 | 10.8 | 0.52x | 6.9 |
| `om ref / vptr_map` | 2 | 31.3 | 1.48x | 27.0 |
| `om vptr / flat_map` | 2 | 10.8 | 0.51x | 7.0 |
| `om ref / flat_map` | 2 | 28.7 | 1.36x | 24.5 |
| `om ref / inplace` | 2 | 10.8 | 0.53x | 6.6 |
| `om ref / inplace_ind` | 2 | 12.5 | 0.60x | 8.4 |

### Caches cold (`clflush`) — the steadiest ratios

Flushed, the first touch of the receiver is a cache miss in its own right: the
`touch` baseline nets 264 cycles, against 567 for the whole `vf`
yardstick. So 47% of a virtual call's `net` is reaching the object rather than
dispatching on it. `x net` compares whole call with whole call — the headline,
and the most reproducible figure this benchmark produces: misses dominate, and
misses do not care about code layout. `disp` and `x disp` are the
mechanism-excess diagnostics.

Median of 7 passes.

| dispatch | arity | net | disp | x net | x disp |
|---|---|---|---|---|---|
| `vf` | 1 | 567 | 305 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 1 | 587 | 583 | 1.03x | 1.93x |
| `om ref / vptr_vector` | 1 | 1126 | 861 | 1.98x | 2.94x |
| `om vptr / indirect` | 1 | 661 | 658 | 1.17x | 2.15x |
| `om ref / indirect` | 1 | 1300 | 1023 | 2.29x | 3.52x |
| `om vptr / vptr_map` | 1 | 490 | 486 | 0.86x | 1.62x |
| `om ref / vptr_map` | 1 | 807 | 546 | 1.44x | 1.88x |
| `om vptr / flat_map` | 1 | 498 | 494 | 0.89x | 1.65x |
| `om ref / flat_map` | 1 | 827 | 565 | 1.51x | 1.95x |
| `om ref / inplace` | 1 | 578 | 330 | 1.13x | 1.25x |
| `om ref / inplace_ind` | 1 | 800 | 554 | 1.59x | 2.13x |
| `vf+vf (double dispatch)` | 2 | 665 | 339 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 2 | 767 | 763 | 1.14x | 2.23x |
| `om ref / vptr_vector` | 2 | 1421 | 1092 | 2.15x | 3.28x |
| `om vptr / indirect` | 2 | 830 | 826 | 1.21x | 2.39x |
| `om ref / indirect` | 2 | 1624 | 1297 | 2.41x | 3.79x |
| `om vptr / vptr_map` | 2 | 783 | 779 | 1.16x | 2.28x |
| `om ref / vptr_map` | 2 | 1139 | 813 | 1.69x | 2.35x |
| `om vptr / flat_map` | 2 | 787 | 783 | 1.16x | 2.27x |
| `om ref / flat_map` | 2 | 1176 | 848 | 1.77x | 2.51x |
| `om ref / inplace` | 2 | 805 | 502 | 1.33x | 1.67x |
| `om ref / inplace_ind` | 2 | 1128 | 822 | 1.83x | 2.59x |

### Reading it

`x vf` / `x net` divide whole call by whole call. For scale, a direct call
measures 3.7 cycles net warm. Ratios from earlier revisions of this README
were computed under different window and baseline schemes and are not
comparable; [HISTORY.md](HISTORY.md) has the lineage.

- **A `virtual_ptr` call costs exactly what a virtual function call costs**:
  1.00x warm (10.2 vs 10.4 cycles net), 1.03x cold. Not "no more than" — the
  same, and the three direct registries agree (1.00x / 1.01x / 1.00x warm).
- **`virtual_` reference dispatch costs about 2x a virtual call cold — the
  figure that holds across every build** (1.98x / 2.05x / 1.98x / 2.01x), and
  above the 30-50% band the library's documentation cites, whose numbers date
  from a different harness on different hardware. Warm the ratio is 1.56x on
  gcc/64 but ranges 1.42x-2.10x across builds with the yardstick's
  misprediction cost — a build-local figure. The excess is the
  hash-and-look-up, ~6 extra cycles warm in the `disp` column.
- **The `vptr_map` probe is expensive warm**: 2.46x through a reference, and
  `boost::unordered_flat_map` measures the same (2.48x) — the extra pointer
  chase matters, the probe strategy does not at this table size. Cold the gap
  narrows (1.44x) because everyone's misses dominate.
- **`inplace_vptr` is indistinguishable from a virtual function** (1.00x warm,
  1.13x cold): its layout *is* the virtual function's layout.
- **At two virtual arguments, who wins depends on temperature.** Warm, the
  multi-method through `virtual_ptr` costs 0.52x the double-dispatch idiom —
  two independent lookups against two dependent virtual calls. Cold the idiom
  wins: 1.14x net for the `virtual_ptr` form and 2.15x for the reference form,
  because the two-dimensional dispatch data spans more cache lines than the
  idiom's two v-table chains. (In the map registries, even warm reference
  dispatch loses to the idiom.)
- **`indirect_vptr` prices its extra dereference at about one cycle warm
  through a `virtual_ptr`** (1.00x → 1.08x) and three through a reference
  (1.56x → 1.85x); the section below itemizes it.

## Two fair comparisons

The delivery world above (const bodies) is `virtual_ptr`'s best case: dispatch
without the object, the no-op-default workload. The use world makes every body
read a member of every receiver, so every call form owes the receiver's cache
line exactly once and `disp` subtracts `touch` legitimately for all rows —
including `virtual_ptr`, which pays its receiver miss in the body instead of
in the dispatch.

One caution: the benchmark measures the *call mechanism*, not bodies. The
member reads exist to make the receiver line's movement visible. The
measurable question is where the `virtual_ptr` body's deferred receiver miss
lands — serialized behind the table misses, or overlapped under them.

#### Warm, receiver used

Median of 7 passes.

| dispatch | arity | net | x vf | disp |
|---|---|---|---|---|
| `vf` | 1 | 17.8 | 1.00x | 13.3 |
| `om vptr / vptr_vector` | 1 | 17.2 | 0.98x | 13.1 |
| `om ref / vptr_vector` | 1 | 23.8 | 1.37x | 19.8 |
| `om vptr / indirect` | 1 | 19.5 | 1.14x | 15.5 |
| `om ref / indirect` | 1 | 28.1 | 1.59x | 23.8 |
| `om vptr / vptr_map` | 1 | 17.1 | 0.98x | 13.1 |
| `om ref / vptr_map` | 1 | 32.2 | 1.84x | 27.9 |
| `om vptr / flat_map` | 1 | 17.3 | 0.98x | 13.3 |
| `om ref / flat_map` | 1 | 32.1 | 1.85x | 28.0 |
| `om ref / inplace` | 1 | 17.3 | 0.97x | 13.1 |
| `om ref / inplace_ind` | 1 | 21.4 | 1.21x | 17.2 |
| `vf+vf (double dispatch)` | 2 | 22.1 | 1.00x | 18.0 |
| `om vptr / vptr_vector` | 2 | 11.1 | 0.51x | 7.0 |
| `om ref / vptr_vector` | 2 | 18.0 | 0.82x | 13.8 |
| `om vptr / indirect` | 2 | 12.2 | 0.56x | 8.2 |
| `om ref / indirect` | 2 | 20.0 | 0.91x | 15.9 |
| `om vptr / vptr_map` | 2 | 11.1 | 0.51x | 7.0 |
| `om ref / vptr_map` | 2 | 31.7 | 1.46x | 27.7 |
| `om vptr / flat_map` | 2 | 11.1 | 0.51x | 7.0 |
| `om ref / flat_map` | 2 | 29.7 | 1.34x | 25.5 |
| `om ref / inplace` | 2 | 11.1 | 0.54x | 7.0 |
| `om ref / inplace_ind` | 2 | 12.9 | 0.59x | 8.8 |

#### Cold (`clflush`), receiver used

Median of 7 passes.

| dispatch | arity | net | disp | x net | x disp |
|---|---|---|---|---|---|
| `vf` | 1 | 518 | 257 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 1 | 522 | 257 | 1.02x | 1.05x |
| `om ref / vptr_vector` | 1 | 1107 | 847 | 2.10x | 3.30x |
| `om vptr / indirect` | 1 | 677 | 403 | 1.27x | 1.52x |
| `om ref / indirect` | 1 | 1257 | 997 | 2.39x | 4.08x |
| `om vptr / vptr_map` | 1 | 529 | 253 | 1.02x | 1.04x |
| `om ref / vptr_map` | 1 | 825 | 559 | 1.59x | 2.24x |
| `om vptr / flat_map` | 1 | 509 | 247 | 0.98x | 0.96x |
| `om ref / flat_map` | 1 | 832 | 574 | 1.61x | 2.23x |
| `om ref / inplace` | 1 | 663 | 426 | 1.28x | 1.52x |
| `om ref / inplace_ind` | 1 | 793 | 540 | 1.56x | 2.09x |
| `vf+vf (double dispatch)` | 2 | 619 | 290 | 1.00x | 1.00x |
| `om vptr / vptr_vector` | 2 | 821 | 496 | 1.32x | 1.64x |
| `om ref / vptr_vector` | 2 | 1428 | 1099 | 2.31x | 3.79x |
| `om vptr / indirect` | 2 | 849 | 522 | 1.37x | 1.75x |
| `om ref / indirect` | 2 | 1643 | 1313 | 2.65x | 4.53x |
| `om vptr / vptr_map` | 2 | 789 | 462 | 1.27x | 1.58x |
| `om ref / vptr_map` | 2 | 1164 | 837 | 1.92x | 2.98x |
| `om vptr / flat_map` | 2 | 832 | 506 | 1.29x | 1.63x |
| `om ref / flat_map` | 2 | 1159 | 831 | 1.91x | 3.00x |
| `om ref / inplace` | 2 | 773 | 466 | 1.30x | 1.65x |
| `om ref / inplace_ind` | 2 | 1102 | 803 | 1.86x | 2.74x |

### What the use world shows

- **Parity holds in both columns**: `virtual_ptr` is 0.98x warm and 1.02x net
  / 1.05x disp cold of the virtual function — the receiver miss now paid by
  both sides and subtracted from both sides.
- **The deferred receiver miss costs nothing.** Cold net 587 with the body
  never touching the object, 522 with it read — against 264 for a lone
  receiver miss. The object address is in the fat pointer from the first
  cycle; the body's load completes under the table misses.
- **The virtual function's prefetch buys it nothing in return**: its own body
  read is free (the line its dispatch fetched), but so, through overlap, is
  `virtual_ptr`'s. Delivery world: `virtual_ptr` matches without loading the
  object. Use world: it matches while hiding the load. No workload here
  rewards keeping the v-table pointer in the object.

## Cost of `indirect_vptr`

`indirect_vptr` is a marker policy (`boost/openmethod/preamble.hpp`). With it,
`vptr_vector` stores `const vptr_type*` instead of `vptr_type`, and a
`virtual_ptr` holds a pointer *to* the v-table pointer. What you buy: calling
`initialize()` again — after loading a shared library, say — revalidates every
`virtual_ptr` already in flight instead of dangling it. What you pay: one more
dependent load on every dispatch.

The `indirect` rows above are the library's `indirect_registry` — the default
policies plus `indirect_vptr` — so the comparison against `vptr_vector`
isolates the one policy.

### What it costs in instructions: exactly one

Timed regions from the committed scheme's binaries (gcc 13, `-O2`), direct on
the left, indirect on the right; the only difference is the marked load:

```asm
mov  rcx, QWORD PTR [rsp]            mov  rcx, QWORD PTR [rsp]
mov  rax, QWORD PTR [rip+slot]       mov  rdx, QWORD PTR [rip+slot]
mov  rdi, QWORD PTR [rsp]            mov  rdi, QWORD PTR [rsp]
                                     mov  rax, QWORD PTR [rcx]   ; <-- deref
call QWORD PTR [rcx+rax*8]           call QWORD PTR [rax+rdx*8]
```

(The start stamp's register bookkeeping is elided; the compiler schedules a
sub-cycle portion of it differently between the two.)

### What it costs in cycles

Median of 7 passes; `disp` cycles, direct → indirect.

#### Warm — the extra load, uncontended

| call form | arity | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|---|
| `virtual_ptr` | 1 | 6.1 → 6.8 (**+0.7**) | 6.9 → 9.6 (**+2.7**) | 5.8 → 6.2 (**+0.5**) | 10.7 → 12.2 (**+1.5**) |
| `virtual_ptr` | 2 | 7.0 → 7.9 (**+0.9**) | 3.0 → 4.4 (**+1.4**) | 6.8 → 8.0 (**+1.2**) | 10.1 → 11.8 (**+1.7**) |
| `virtual_` ref | 1 | 11.9 → 14.6 (**+2.7**) | 18.0 → 21.4 (**+3.4**) | 11.7 → 14.0 (**+2.3**) | 14.8 → 18.3 (**+3.4**) |
| `virtual_` ref | 2 | 13.2 → 15.3 (**+2.0**) | 13.9 → 16.4 (**+2.6**) | 13.7 → 15.4 (**+1.7**) | 7.8 → 9.7 (**+1.9**) |
| `inplace` ref | 1 | 6.1 → 8.1 (**+2.0**) | 10.6 → 14.7 (**+4.0**) | 6.0 → 7.2 (**+1.3**) | 6.5 → 10.5 (**+4.0**) |
| `inplace` ref | 2 | 6.6 → 8.4 (**+1.8**) | 6.5 → 8.1 (**+1.6**) | 7.1 → 8.0 (**+0.9**) | 1.2 → 3.0 (**+1.8**) |

#### Cold (`clflush`) — the extra load, as a cache miss

| call form | arity | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|---|
| `virtual_ptr` | 1 | 583 → 658 (**+74**) | 513 → 539 (**+26**) | 579 → 573 (**-6**) | 536 → 558 (**+22**) |
| `virtual_ptr` | 2 | 763 → 826 (**+64**) | 794 → 875 (**+81**) | 825 → 867 (**+41**) | 802 → 865 (**+63**) |
| `virtual_` ref | 1 | 861 → 1023 (**+162**) | 891 → 1138 (**+248**) | 874 → 1061 (**+187**) | 889 → 1065 (**+176**) |
| `virtual_` ref | 2 | 1092 → 1297 (**+205**) | 1178 → 1461 (**+283**) | 1086 → 1351 (**+265**) | 1114 → 1371 (**+257**) |
| `inplace` ref | 1 | 330 → 554 (**+224**) | 310 → 557 (**+247**) | 312 → 538 (**+227**) | 368 → 568 (**+200**) |
| `inplace` ref | 2 | 502 → 822 (**+320**) | 571 → 842 (**+272**) | 497 → 839 (**+342**) | 570 → 813 (**+243**) |

### Reading the cost

- **Warm, about one cycle through a `virtual_ptr` and about three through a
  reference**: 1.00x → 1.08x and 1.56x → 1.85x on gcc/64. On the reference
  path the load lands at the end of an already-long dependency chain (object →
  `type_info` → hash → vptr vector → **indirection** → v-table) with nothing
  to overlap against; through a `virtual_ptr` the chain is short and there is
  slack.
- **Cold, the reference path pays a full miss for it** — the indirection
  target is a line of its own. Through a `virtual_ptr` at arity 1 the four
  builds measure +74, +26, −6 and +22 cycles against nets of ~550: small, and
  inconsistently sized, because the independent slot load gives the miss
  something to hide under.
- **On an `inplace_vptr` hierarchy the same policy costs about two cycles
  warm** (10.3 → 12.2 net at arity 1, 10.8 → 12.5 at arity 2) — the second
  dependent load's latency, giving back the tie with the virtual function that
  `inplace_vptr` had won.

If the program needs `initialize()` to be callable more than once, the price
is a load; if it does not, `indirect_vptr` is pure cost.

## Compiler and bitness

Four builds — g++ 13.3 and clang++ 18.1, each at `-m64` and `-m32` — measured
by `./matrix.sh`. Cells are `x net (net cycles)`: whole call against the same
build's own yardstick, which is what makes columns comparable — the compilers
generate different code for the yardstick itself.

The bitness axis is real at the data-structure level: at `-m32`,
`sizeof(void*)` and the dispatch-table word halve to 4 bytes and
`virtual_ptr` halves to 8 bytes (still two words). All four builds of the
committed sources pass `--verify`. (Upstream CI exercises 32-bit builds for both MSVC and gcc, per
`.drone.jsonnet`; this benchmark adds measured 32-bit numbers.)

#### Caches cold (`clflush`)

| dispatch | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|
| **1 virtual argument** |  |  |  |  |
| `vf (yardstick)` | 1.00x (567) | 1.00x (555) | 1.00x (579) | 1.00x (575) |
| `om vptr / vptr_vector` | 1.03x (587) | 0.93x (523) | 1.00x (581) | 0.96x (544) |
| `om ref / vptr_vector` | 1.98x (1126) | 2.05x (1155) | 1.98x (1147) | 2.01x (1156) |
| `om vptr / indirect` | 1.17x (661) | 0.97x (549) | 1.01x (575) | 1.03x (566) |
| `om ref / indirect` | 2.29x (1300) | 2.51x (1401) | 2.31x (1337) | 2.36x (1339) |
| `om vptr / vptr_map` | 0.86x (490) | 0.99x (564) | 0.86x (498) | 0.97x (555) |
| `om ref / vptr_map` | 1.44x (807) | 1.41x (786) | 1.46x (844) | 1.36x (774) |
| `om vptr / flat_map` | 0.89x (498) | 0.97x (521) | 0.87x (496) | 0.98x (552) |
| `om ref / flat_map` | 1.51x (827) | 1.44x (794) | 1.46x (837) | 1.34x (769) |
| `om ref / inplace` | 1.13x (578) | 1.09x (557) | 1.10x (562) | 1.10x (608) |
| `om ref / inplace_ind` | 1.59x (800) | 1.53x (793) | 1.50x (780) | 1.53x (801) |
| **2 virtual arguments** |  |  |  |  |
| `vf+vf (yardstick)` | 1.00x (665) | 1.00x (663) | 1.00x (761) | 1.00x (673) |
| `om vptr / vptr_vector` | 1.14x (767) | 1.20x (804) | 1.09x (827) | 1.24x (810) |
| `om ref / vptr_vector` | 2.15x (1421) | 2.27x (1486) | 1.87x (1409) | 2.15x (1428) |
| `om vptr / indirect` | 1.21x (830) | 1.35x (885) | 1.15x (868) | 1.30x (873) |
| `om ref / indirect` | 2.41x (1624) | 2.62x (1771) | 2.22x (1668) | 2.54x (1677) |
| `om vptr / vptr_map` | 1.16x (783) | 1.18x (765) | 1.09x (817) | 1.19x (780) |
| `om ref / vptr_map` | 1.69x (1139) | 1.72x (1130) | 1.54x (1160) | 1.71x (1138) |
| `om vptr / flat_map` | 1.16x (787) | 1.18x (784) | 1.10x (830) | 1.18x (795) |
| `om ref / flat_map` | 1.77x (1176) | 1.71x (1162) | 1.53x (1150) | 1.72x (1147) |
| `om ref / inplace` | 1.33x (805) | 1.40x (872) | 1.16x (792) | 1.39x (874) |
| `om ref / inplace_ind` | 1.83x (1128) | 1.87x (1137) | 1.65x (1135) | 1.82x (1107) |

Median of 7 passes. Spread across passes: median 12%, p90 22%.

#### Warm caches

| dispatch | gcc/64 | gcc/32 | clang/64 | clang/32 |
|---|---|---|---|---|
| **1 virtual argument** |  |  |  |  |
| `vf (yardstick)` | 1.00x (10.4) | 1.00x (13.8) | 1.00x (6.4) | 1.00x (16.8) |
| `om vptr / vptr_vector` | 1.00x (10.2) | 1.21x (16.6) | 1.14x (7.4) | 1.11x (18.9) |
| `om ref / vptr_vector` | 1.56x (16.1) | 1.66x (22.9) | 2.10x (13.3) | 1.42x (24.7) |
| `om vptr / indirect` | 1.08x (10.9) | 1.38x (19.3) | 1.25x (8.0) | 1.22x (20.5) |
| `om ref / indirect` | 1.85x (18.8) | 1.90x (26.1) | 2.45x (15.7) | 1.60x (28.1) |
| `om vptr / vptr_map` | 1.01x (10.3) | 1.20x (16.0) | 1.13x (7.6) | 1.04x (17.9) |
| `om ref / vptr_map` | 2.46x (24.6) | 2.21x (30.8) | 3.39x (22.0) | 1.86x (32.1) |
| `om vptr / flat_map` | 1.00x (10.2) | 1.19x (16.3) | 1.14x (7.3) | 1.06x (17.9) |
| `om ref / flat_map` | 2.48x (25.6) | 2.19x (31.9) | 3.92x (25.2) | 2.10x (35.6) |
| `om ref / inplace` | 1.00x (10.3) | 1.11x (15.5) | 1.06x (7.3) | 1.17x (16.5) |
| `om ref / inplace_ind` | 1.21x (12.2) | 1.42x (19.6) | 1.32x (8.8) | 1.46x (20.4) |
| **2 virtual arguments** |  |  |  |  |
| `vf+vf (yardstick)` | 1.00x (21.0) | 1.00x (23.7) | 1.00x (19.5) | 1.00x (24.7) |
| `om vptr / vptr_vector` | 0.52x (10.8) | 0.54x (12.8) | 0.44x (8.5) | 0.75x (18.3) |
| `om ref / vptr_vector` | 0.83x (17.4) | 0.78x (18.5) | 0.78x (15.3) | 0.72x (17.6) |
| `om vptr / indirect` | 0.55x (11.7) | 0.60x (14.4) | 0.49x (9.7) | 0.81x (20.0) |
| `om ref / indirect` | 0.92x (19.4) | 0.89x (21.2) | 0.86x (16.8) | 0.80x (19.6) |
| `om vptr / vptr_map` | 0.52x (10.8) | 0.49x (11.7) | 0.44x (8.5) | 0.75x (18.6) |
| `om ref / vptr_map` | 1.48x (31.3) | 1.20x (28.3) | 1.46x (28.1) | 1.27x (31.0) |
| `om vptr / flat_map` | 0.51x (10.8) | 0.50x (11.8) | 0.44x (8.5) | 0.75x (18.5) |
| `om ref / flat_map` | 1.36x (28.7) | 1.31x (31.0) | 1.52x (29.7) | 1.43x (35.2) |
| `om ref / inplace` | 0.53x (10.8) | 0.47x (11.2) | 0.49x (8.5) | 0.44x (11.1) |
| `om ref / inplace_ind` | 0.60x (12.5) | 0.55x (12.9) | 0.54x (9.5) | 0.51x (12.9) |

Median of 7 passes. Spread across passes: median 8%, p90 19%.

### What it shows

- **Bitness buys nothing cold**: reference-dispatch nets are 1126 → 1155 on
  gcc and 1147 → 1156 on clang — halving every table does not save misses
  that are counted per line, not per byte.
- **The compilers agree on the mechanisms.** Cold, whole call against whole
  call, reference dispatch is 1.98x-2.05x the virtual function on all four
  builds. Warm ratios scatter (the small yardstick denominators are
  layout-sensitive); read the cycle columns before the ratios there.
- **clang/32's `virtual_ptr` rows carry a harness artifact**: the i386 ABI
  marshals the 8-byte fat pointer through the stack inside the timed window
  (~10.7 warm disp against 5.8-6.9 on the other builds) — the by-value
  argument meeting that ABI, not the library.
- **`inplace` is the fastest reference dispatch in every column** — cold,
  1.09x-1.13x net at arity 1 (1.16x-1.40x at arity 2) across the four builds.

## gcc against clang, instruction by instruction

The obvious reading of compiler differences — that one generates better
dispatch — is wrong. The current timed regions, arity 1, `vptr_vector`,
`virtual_ptr` (start-stamp bookkeeping included, since it is what the window
contains):

```asm
; gcc — spills the fat pointer          ; clang — keeps it in registers
mov  rcx, QWORD PTR [rsp]               mov  r14d, edx
mov  r12d, eax                          neg  r14d
mov  rax, QWORD PTR [rip+slot]          shl  r14, 0x20
mov  ebx, edx                           mov  eax, eax
mov  rdi, QWORD PTR [rsp]               sub  r14, rax
shl  rbx, 0x20                          mov  rax, QWORD PTR [rip+slot]
call QWORD PTR [rcx+rax*8]              call QWORD PTR [rdi+rax*8]
```

Different register strategies, same critical path: one slot load and one
indirect call. They measure the same. The reference form is
instruction-for-instruction equivalent between the compilers. What differs
across builds is the *yardstick*: a virtual call's warm cost is dominated by
indirect-branch misprediction (100 random targets), and predictor behavior
depends on binary layout — which is why warm ratios are build-local while
cold ratios agree. A side experiment with a single leaf class (perfectly
predicted) put the predicted virtual call at a few cycles and the mispredicted
one severalfold higher; it was measured under a previous window scheme and is
archived in [HISTORY.md](HISTORY.md).

## Reproducibility

- Every published cell is the **median of 7 passes**; `matrix.sh` interleaves
  the whole matrix per pass so drift lands on all columns alike, and clears
  stale run directories first (the results are committed, so stale runs are
  the norm, not an accident).
- Across passes, a single variant's `net` moves by a median of **8% cold (p90
  18%, worst 60%)** and **7% warm (p90 14%)** on this machine — an
  un-isolatable WSL2 guest. (The matrix captions quote a slightly larger
  spread — that one is measured on the *ratios*, so it also absorbs the
  yardstick's own motion.) Quote medians, not passes.
- **The built-in control**: the three direct registries' `om vptr` rows must
  agree — the vptr policy is not on that call path. Warm they agree to 0-9%
  per column (exactly 10.8 / 10.8 / 10.8 net on gcc/64 at arity 2; the worst
  is gcc/32 at arity 2, ~9%); cold to 1-19%, arity 1 being the noisier. `indirect` is excluded by design — its
  extra load is the point. If the control diverges beyond that, discard the
  run.
- **Shielding does not help.** Deliberate co-tenancy — spinners on other
  cores, a spinner on the measurement core itself, 256 MiB memory streamers —
  moves the result less than idle pass-to-pass variance, because the trimmed
  mean already discards preemptions into its top 5%. What remains is not
  schedulable: DRAM and prefetcher state, and the hypervisor. (Details and
  the probe data: [HISTORY.md](HISTORY.md).)

## Method

### Timing

The stop is **in the measured body**. Every yardstick, baseline and overrider
returns a `stamp_id`: the counter read on arrival, plus an id — computed after
the stamp — that carries the verification oracle. The measured window is:

```
asm barrier; lfence; rdtsc; lfence          <- start bracket, in timed_call
    <dispatch>
rdtscp                                      <- in the body; returned
```

`rdtscp` waits until every prior instruction — the whole dispatch chain, and
in the use world the receiver load kept above it by its memory clobber — has
executed, so the stamp is the moment control *arrived* in the right overrider.
Everything after it is outside the window by construction: assembling the
stamp, writing the id, the entire return path, and the elapsed computation,
which is a data dependency on the returned value. There is no closing bracket,
so nothing is compiler-scheduled at the stop edge.

- The start bracket lives inside a `noinline` `timed_call` whose parameters
  are the call arguments: the prologue is untimed while the arguments still
  arrive through the ABI and cannot be constant-folded or devirtualized.
- The start timestamp is captured as the raw `edx:eax` pair; the compiler
  schedules the two or three ALU ops that assemble it as it pleases — inside
  the window for some variants, partly or wholly after the stop for others
  (`probe` carries none in-window). The asymmetry is bounded by a couple of
  register-ALU ops, sub-cycle on this core, and is the floor on how finely
  warm figures should be read.
- **Every variant replays the identical receiver sequence** (the RNG is
  reseeded per variant): `disp` is a difference of two measurements, and cold
  both are dominated by which objects were drawn — unpaired draws leave that
  term uncancelled.
- Each rep performs an untimed warm-up call, then scrubs (in the cold mode),
  then measures once.
- `lfence` is dispatch-serializing on Zen, so no `CPUID` — which traps to the
  hypervisor here and would cost more than the thing measured.
- Both compilers get `-fcf-protection=none`; their defaults differ (gcc
  `=full`) in ways that put an `endbr64` on every indirect-call target of one
  build and not the other ([HISTORY.md](HISTORY.md)).

### Cache states

| mode | what it does |
|---|---|
| `warm` | nothing, after the warm-up call |
| `clflush` | `clflushopt` over the receiver(s), the C++ v-table from its head (vptr−16, so the `type_info` slot at vptr−8 is always evicted), the method's `fn` object, the registry's dispatch-table arena, the vptr storage, and — for indirect registries — the per-class `static_vptr` cells; then `mfence` |
| `sweep` | one store per line over a 64 MiB buffer (2x L3); diagnostic only, not in the published dataset |

### Statistics

The reported statistic is the **mean of each variant's samples, trimmed of the
top 5%** (where preemptions land), with the standard error of the difference
in the console's `+/-` column. Individual samples are quantized: an
lfence-bracketed counter read costs 25 or 50 cycles bimodally here, so warm
medians sit on a tick (50, sometimes 75) while the trimmed mean resolves well
below one — which is also why sub-cycle warm differences should not be read
at all. TSC frequency is calibrated against `CLOCK_MONOTONIC` at startup;
the committed data implies ~2.45 GHz.

### Correctness gate

`--verify` runs before any measurement, and its oracles are computed **from
the class contracts alone**, carried in each body's returned id: `tag` for
arity 1 (every world), `tag`-if-same-leaf-else-−1 for const arity 2,
`a.tag + b.tag` / `−a.tag − b.tag − 1` for use arity 2 (distinct values, so
the gate can tell which overrider ran), `b.tag` for the dd yardstick. Every
dispatch path in every registry and both worlds is checked, plus the
yardsticks and the `touch` baseline. Comparing paths against each other
instead — which passes when every registry is wrong the same way — is a hole
this gate closed after an adversarial review demonstrated it
([HISTORY.md](HISTORY.md)).

## Caveats

- **The receiver-touch baseline is a lower bound.** `touch` reads a member on
  the same cache line as the object's v-table pointer, so it prices the line's
  miss; a `virtual_` dispatch then chases that pointer to the `type_info` — a
  second miss `disp` rightly charges to dispatch, but `disp` is not purely
  "arithmetic plus an indirect call".
- **Warm ratios do not transfer across binaries.** The warm yardstick is
  mostly misprediction and its cost is layout-dependent (6.4-16.8 cycles net
  across the four builds). Cold `x net` is the portable figure.
- **`net` and `disp` differ by the plain-call cost by construction** (~4
  cycles warm): `net` subtracts the empty `probe`, `disp` subtracts a real
  call. Neither is wrong; they answer the two questions in "What the ratios
  divide".
- Cycles are TSC *reference* cycles at ~2.45 GHz, not core cycles; the core
  clock is invisible under WSL2/Hyper-V. `run.sh` requests `SCHED_FIFO` and
  falls back to normal priority without it.
- The harness reaches into `boost::openmethod::detail` for the dispatch-table
  arena — deliberate: `clflush` must evict exactly what the dispatch reads.
- **`clflush` cannot reach the map registries' bucket arrays** — they are
  runtime-allocated, and only the container header is flushed — so the
  `vptr_map`/`flat_map` cold rows keep some interior state resident and read
  slightly better than a truly cold map would.
- On the -m32 builds, clang marshals the 8-byte `virtual_ptr` through the
  stack inside the window (see "What it shows") — read those rows as harness
  ABI cost, not dispatch cost.

## Generated code

Timed regions, gcc 13 `-O2`, current binaries. Each region ends at the `call`
into a body whose first instruction is `rdtscp`; the `mov`/`shl`/`or` around
the dispatch is the start stamp's bookkeeping, which the compiler schedules
freely (here inside the window; in other variants partly after the call) — a
sub-cycle asymmetry, per "Timing".

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
each receiver precedes it.

## Build and run

Boost (1.91+) must be installed system-wide, and `./include` must be a
symlink into a Boost checkout — it is machine-specific and not committed:

```sh
ln -s /path/to/boost/libs/openmethod/include include
```

```sh
./build.sh                       # -> bin/benchmark-g++-64
CXX=clang++ BITS=32 ./build.sh   # compiler/bitness; needs g++-multilib for -m32
DEBUG=1 CLASSES=4 ./build.sh     # -O0 -g build for stepping through the code
bin/benchmark-g++-64 --verify    # the correctness gate
./run.sh                         # one build, all three cache modes, pinned core
RUNS=7 ./matrix.sh               # the full matrix -> results/run1..7 (as published)
python3 report.py                # regenerates every table in this README
```

Benchmark flags: `--reps --objects --sweep-mb --cpu --seed
--mode warm|clflush|sweep|all --csv --verify`. The class count is
compile-time: `CLASSES=1000 ./build.sh` or `-DOMB_CLASSES` with CMake.

## Files

| file | |
|---|---|
| `src/timing.hpp` | brackets, stamps, cache scrubbing, TSC calibration, statistics |
| `src/hierarchy.hpp` | both class hierarchies, yardsticks, the `touch` baselines |
| `src/registries.hpp` | the six dispatch configurations and the const-body methods |
| `src/use_methods.hpp` | the use-body methods and overriders |
| `src/main.cpp` | variants, measurement loop, verification, reporting |
| `matrix.sh` | builds, verifies and measures all four compiler x bitness builds, N passes |
| `run.sh` | single-build driver on a pinned core |
| `report.py` | regenerates every generated section of this README from `results/` |
| `results/run1..7/` | the committed dataset behind every table |
| `include` | symlink into a Boost checkout (not committed) |

Overriders are registered in bulk through the core API (`method<...>::override`
takes a pack), since the leaves are a class template.

## History

This benchmark was corrected into its current design through an adversarial
review and several measurement-scheme redesigns, each of which changed what
the timed window contains — and therefore what the numbers mean.
[HISTORY.md](HISTORY.md) is the chronicle.
