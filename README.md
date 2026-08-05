# Boost.OpenMethod dispatch benchmarks

This benchmark measures the cost of an open-method dispatch, using a virtual
function call as a yardstick, with caches warm or deliberately scrubbed. The
headline, on this machine: a `virtual_ptr` call costs what a virtual function
call costs once the caches are cold (1.03x), which is the comparison that
reproduces; warm it is 1.14x on the reference build and 1.00x on gcc, not
because the compilers emit different code — the virtual call is the same two
instructions in both — but because warm the yardstick is mostly branch
misprediction, and that moves with binary layout.
`virtual_` reference dispatch costs about 2x a virtual call cold — a figure
stable within 3% across two compilers and two bitnesses — and at two virtual
arguments the winner depends on temperature: an open multi-method through
`virtual_ptr` costs about half the double-dispatch idiom warm, while cold the
idiom's two lean v-table chains win — the multi-method costs a modest 1.25x.

Boost.OpenMethod's documentation cites "micro- and RDTSC-based benchmarks" for
its performance claims; this repository is the RDTSC benchmark, built to be
auditable: every table is generated from the committed data by `report.py`, and
every dispatch path is checked against contract-derived oracles before a single
measurement runs.

## Where the v-table pointer lives

The two call forms differ in one structural fact, and most of the numbers
below follow from it.

A virtual function call finds the v-table *through the object*: the dispatch
chain starts with a load from the receiver, whether or not the body needs it.
`virtual_` reference dispatch — an open-method call through a plain
reference — shares the constraint: the library recovers the receiver's
dispatch table by hashing its `type_info`, a chain that starts from that
same embedded pointer.

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

| axis      | values                                                                                                                                                                                                                                                                                                                                                                                         |
| --------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| dispatch  | how the v-table pointer is reached: `vptr_vector` (the default registry: `std_rtti` + `fast_perfect_hash` + `vptr_vector`), `indirect` (the default plus `indirect_vptr`), `vptr_map` (over `std::unordered_map`), `flat_map` (`vptr_map` over `boost::unordered_flat_map`), `inplace` (`inplace_vptr`: the pointer stored in the object), `inplace_ind` (`inplace_vptr` plus `indirect_vptr`) |
| call form | `virtual_<const Base&>` (the v-table pointer is looked up at the call site) vs `virtual_ptr<const Base, R>` (already carries it)                                                                                                                                                                                                                                                               |
| arity     | 1 and 2 virtual arguments                                                                                                                                                                                                                                                                                                                                                                      |
| body      | `const` — bodies return a compile-time constant, the receiver is touched only where the mechanism requires it; `use` — every body reads a member of every receiver                                                                                                                                                                                                                             |
| compiler  | clang++ 22.1 vs g++ 13.3                                                                                                                                                                                                                                                                                                                                                                       |
| bitness   | 64-bit vs 32-bit (`-m32`)                                                                                                                                                                                                                                                                                                                                                                      |

All six dispatch values are registries in the code; the last two have no
`vptr` policy at all — the pointer lives in the object via the `inplace_vptr`
mixin, so those two are measured through a reference only (a `virtual_ptr`
would carry a pointer the object already holds). They also need their own
class hierarchy — an inplace class binds to exactly one registry — with their
own yardsticks and baselines. Every inplace ratio divides by its own
hierarchy's yardstick; the yardsticks agree warm to a fraction of a cycle on
most builds (2.3 cycles on clang/32), and cold differ ~10% — which is exactly
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

- **`net` = mean − `probe`**: the whole call — call site to arrival in the
  body — with only the measurement apparatus removed.
- **`disp`**: the dispatch machinery alone — the row minus a baseline that
  does everything *except* dispatch. That baseline is a plain call, plus the
  receiver load when the row performs one: rows whose timed region loads the
  receiver — for dispatch or in the body — subtract `touch`; the const-body
  `om vptr` rows, which never load it (their v-table pointer arrives in the
  fat pointer), subtract `direct`. Subtracting `touch` from them would
  fabricate a credit for a load they never pay — cold, a whole cache miss.

Worked example — warm, clang/64, `om ref` against the `vf` yardstick:

|                          | `vf` | `om ref` | ratio                |
| ------------------------ | ---- | -------- | -------------------- |
| whole call (`net`)       | 9.0  | 15.5     | **1.72x = `x net`**  |
| − the `touch` baseline   | −4.1 | −4.1     |                      |
| machinery alone (`disp`) | 4.9  | 11.3     | **2.31x = `x disp`** |

Same two rows, two honest ratios, answering different questions. `x net`
answers the caller's: *if I replace this virtual call with an open-method
call, how much slower is the call?* — three-quarters again as slow. `x disp`
answers the implementer's: *how much more work does the lookup machinery
itself do?* — well over twice as much. The second always reads larger,
mechanically: subtracting the same 4.1 cycles from both sides of a division
pushes the
ratio away from 1. Neither is wrong; **`x net` is the headline in every
table, `x disp` the diagnostic.** Every published cell is its own median
over the passes — ratio cells are medians of the per-pass ratios, not
quotients of the cycle cells — so recomputed differences and quotients drift
in the last digit: 15.5 − 4.1 reads 11.3 here, and 15.5 / 9.0 is 1.72x where
the table below puts this row at 1.74x.

## Results

AMD Ryzen 9 9955HX (Zen 5), 32 MiB L3, WSL2, 100 classes, 4096 objects
shuffled in memory, 6000 reps per pass, median of 7 interleaved passes. This
section is the clang/64 column; the full compiler x bitness matrix follows.
Cycles are TSC reference cycles (~2.50 GHz here — the core clock is not
observable under the hypervisor). Rows read `om <form> / <registry>`:
`om vptr` is the `virtual_ptr` call form, `om ref` the `virtual_` reference
form.

### Warm caches — the finest resolution

Warm mode resolves the mechanisms' few-cycle differences: reaching the receiver
costs 0.1 cycles here, and rows repeat within a build to a percent or two.
But the yardstick is mostly indirect-branch misprediction, which depends on the
binary's layout — across the four builds its net ranges severalfold — so warm
*ratios* are build-local. For figures that transfer, read the cold table below.
The `inplace` rows divide by their own hierarchy's yardstick.

clang/64, median of 7 passes.

For scale: a direct call to a stamping body measures 4.0 cycles net here.

| dispatch                  | arity | net  | x net | disp |
| ------------------------- | ----- | ---- | ----- | ---- |
| `vf`                      | 1     | 9.0  | 1.00x | 4.9  |
| `om vptr / vptr_vector`   | 1     | 10.2 | 1.14x | 6.4  |
| `om ref / vptr_vector`    | 1     | 15.5 | 1.74x | 11.3 |
| `om vptr / indirect`      | 1     | 10.4 | 1.15x | 6.7  |
| `om ref / indirect`       | 1     | 18.3 | 2.04x | 14.0 |
| `om vptr / vptr_map`      | 1     | 9.8  | 1.08x | 5.7  |
| `om ref / vptr_map`       | 1     | 24.3 | 2.70x | 20.1 |
| `om vptr / flat_map`      | 1     | 9.9  | 1.09x | 5.9  |
| `om ref / flat_map`       | 1     | 26.6 | 2.85x | 22.4 |
| `om ref / inplace`        | 1     | 10.1 | 1.05x | 5.9  |
| `om ref / inplace_ind`    | 1     | 11.2 | 1.18x | 7.1  |
| `vf+vf (double dispatch)` | 2     | 20.5 | 1.00x | 17.2 |
| `om vptr / vptr_vector`   | 2     | 11.1 | 0.54x | 7.0  |
| `om ref / vptr_vector`    | 2     | 16.9 | 0.82x | 13.0 |
| `om vptr / indirect`      | 2     | 12.2 | 0.58x | 8.0  |
| `om ref / indirect`       | 2     | 19.0 | 0.91x | 15.1 |
| `om vptr / vptr_map`      | 2     | 11.1 | 0.53x | 7.0  |
| `om ref / vptr_map`       | 2     | 32.0 | 1.55x | 27.9 |
| `om vptr / flat_map`      | 2     | 11.1 | 0.53x | 7.0  |
| `om ref / flat_map`       | 2     | 29.5 | 1.44x | 25.6 |
| `om ref / inplace`        | 2     | 11.1 | 0.54x | 7.0  |
| `om ref / inplace_ind`    | 2     | 12.8 | 0.63x | 8.5  |

### Caches cold (`clflush`) — the steadiest ratios

Flushed, the first touch of the receiver is a cache miss in its own right: the
`touch` baseline nets 257 cycles, against 542 for the whole `vf`
yardstick. So 47% of a virtual call's `net` is reaching the object rather than
dispatching on it. `x net` — a row's total call cost divided by the yardstick's —
is the headline, and the most reproducible figure this benchmark produces:
misses dominate, and misses do not care about code layout. `disp` and
`x disp` are the mechanism-excess diagnostics. The `inplace` rows again
divide by their own hierarchy's yardstick — 516 and 492 cycles here at
arity 1, 683 and 670 at arity 2 — not the yardstick rows shown.

clang/64, median of 7 passes.

| dispatch                  | arity | net  | disp | x net | x disp |
| ------------------------- | ----- | ---- | ---- | ----- | ------ |
| `vf`                      | 1     | 542  | 291  | 1.00x | 1.00x  |
| `om vptr / vptr_vector`   | 1     | 565  | 560  | 1.03x | 1.93x  |
| `om ref / vptr_vector`    | 1     | 1150 | 883  | 2.08x | 3.02x  |
| `om vptr / indirect`      | 1     | 585  | 581  | 1.08x | 2.13x  |
| `om ref / indirect`       | 1     | 1331 | 1082 | 2.45x | 3.85x  |
| `om vptr / vptr_map`      | 1     | 528  | 524  | 0.97x | 1.74x  |
| `om ref / vptr_map`       | 1     | 887  | 630  | 1.60x | 2.08x  |
| `om vptr / flat_map`      | 1     | 500  | 496  | 0.90x | 1.71x  |
| `om ref / flat_map`       | 1     | 807  | 558  | 1.47x | 1.87x  |
| `om ref / inplace`        | 1     | 587  | 358  | 1.16x | 1.29x  |
| `om ref / inplace_ind`    | 1     | 801  | 584  | 1.57x | 2.07x  |
| `vf+vf (double dispatch)` | 2     | 696  | 373  | 1.00x | 1.00x  |
| `om vptr / vptr_vector`   | 2     | 877  | 872  | 1.25x | 2.17x  |
| `om ref / vptr_vector`    | 2     | 1501 | 1199 | 2.14x | 2.99x  |
| `om vptr / indirect`      | 2     | 949  | 944  | 1.36x | 2.51x  |
| `om ref / indirect`       | 2     | 1701 | 1381 | 2.44x | 3.72x  |
| `om vptr / vptr_map`      | 2     | 798  | 793  | 1.16x | 2.18x  |
| `om ref / vptr_map`       | 2     | 1165 | 853  | 1.71x | 2.34x  |
| `om vptr / flat_map`      | 2     | 850  | 846  | 1.25x | 2.28x  |
| `om ref / flat_map`       | 2     | 1146 | 831  | 1.72x | 2.31x  |
| `om ref / inplace`        | 2     | 791  | 500  | 1.18x | 1.33x  |
| `om ref / inplace_ind`    | 2     | 1188 | 896  | 1.62x | 2.11x  |

### Reading it

Ratios below are `x net` unless marked `disp`.

- **A `virtual_ptr` call costs what a virtual function call costs cold**:
  1.03x. Not "no more than" — the same. Warm it reads 1.14x here (10.2 vs 9.0
  cycles net) and 1.00x on gcc/64, and neither compiler is generating better
  code for it: the virtual call is `mov rax, [rdi]` and `call [rax+0x10]` in
  both, and clang's `virtual_ptr` window is the *leaner* of the two, yet the
  ratio goes the other way. What moved is the yardstick's misprediction cost,
  which depends on where the code landed. This is what "warm ratios are
  build-local" means in practice. The three direct
  registries — `vptr_vector`, `vptr_map`, `flat_map`, the ones without
  `indirect_vptr` — agree to 5% warm (1.14x / 1.08x / 1.09x).
- **`virtual_` reference dispatch costs about 2x a virtual call cold — the
  figure that holds across every build** (2.08x / 2.12x / 2.07x / 2.12x), and
  above the 30-50% band the library's documentation cites, whose numbers date
  from a different harness on different hardware. Warm the ratio is 1.74x
  here but ranges 1.40x-1.74x across builds with the yardstick's
  misprediction cost — a build-local figure. The excess is the
  hash-and-look-up, ~6 extra cycles warm in the `disp` column.
- **The `vptr_map` probe is expensive warm**: 2.70x through a reference, and
  `boost::unordered_flat_map` measures much the same (2.85x) — the extra
  pointer chase matters, the probe strategy does not at this table size. Cold
  the gap narrows (1.60x) because everyone's misses dominate — but the map rows are
  flattered: `clflush` cannot reach their runtime-allocated bucket arrays, so
  interior state stays resident (see Caveats). And the sub-1.00x `om vptr`
  cells are not an open method beating a virtual function: the vptr policy is
  not on the `virtual_ptr` call path, so those cells measure the same call as
  `om vptr / vptr_vector`, within the control's cold disagreement (see
  Reproducibility).
- **`inplace_vptr` is indistinguishable from a virtual function** (1.05x warm,
  1.16x cold): its layout *is* the virtual function's layout.
- **At two virtual arguments, who wins depends on temperature.** Warm, the
  multi-method through `virtual_ptr` costs 0.54x the double-dispatch idiom —
  two independent lookups against two dependent virtual calls. Cold the idiom
  wins: 1.25x net for the `virtual_ptr` form and 2.14x for the reference form,
  because the two-dimensional dispatch data spans more cache lines than the
  idiom's two v-table chains. (In the map registries, even warm reference
  dispatch loses to the idiom.)
- **`indirect_vptr` prices its extra dereference at a fraction of a cycle warm
  through a `virtual_ptr`** (1.14x → 1.15x) and three cycles through a
  reference (1.74x → 2.04x); the section below itemizes it.

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

These four tables are the clang/64 column, like "Results" above; the use world
is not re-measured across the matrix.

#### Warm, receiver used

The member reads execute inside the timed window — every use body loads its
receiver(s) before the arrival stamp (see "Timing"). Warm that is visible: the
arity-1 rows sit several cycles above their delivery-world counterparts (`vf`
goes 9.0 → 14.3); at arity 2 the reads overlap the dispatch and the nets
barely move. Ratios divide by this table's own yardsticks, which pay the same
reads.

clang/64, median of 7 passes.

| dispatch                  | arity | net  | x net | disp |
| ------------------------- | ----- | ---- | ----- | ---- |
| `vf`                      | 1     | 14.3 | 1.00x | 10.2 |
| `om vptr / vptr_vector`   | 1     | 16.9 | 1.18x | 12.8 |
| `om ref / vptr_vector`    | 1     | 24.2 | 1.68x | 20.0 |
| `om vptr / indirect`      | 1     | 18.5 | 1.31x | 14.2 |
| `om ref / indirect`       | 1     | 27.2 | 1.90x | 23.0 |
| `om vptr / vptr_map`      | 1     | 16.5 | 1.16x | 12.4 |
| `om ref / vptr_map`       | 1     | 32.1 | 2.23x | 28.0 |
| `om vptr / flat_map`      | 1     | 17.0 | 1.18x | 12.7 |
| `om ref / flat_map`       | 1     | 33.5 | 2.31x | 29.4 |
| `om ref / inplace`        | 1     | 16.6 | 1.03x | 12.6 |
| `om ref / inplace_ind`    | 1     | 21.2 | 1.31x | 16.8 |
| `vf+vf (double dispatch)` | 2     | 23.3 | 1.00x | 18.9 |
| `om vptr / vptr_vector`   | 2     | 11.1 | 0.48x | 7.2  |
| `om ref / vptr_vector`    | 2     | 17.5 | 0.76x | 13.7 |
| `om vptr / indirect`      | 2     | 12.9 | 0.56x | 9.0  |
| `om ref / indirect`       | 2     | 19.7 | 0.85x | 15.7 |
| `om vptr / vptr_map`      | 2     | 11.1 | 0.48x | 7.2  |
| `om ref / vptr_map`       | 2     | 31.7 | 1.38x | 27.5 |
| `om vptr / flat_map`      | 2     | 11.1 | 0.48x | 7.2  |
| `om ref / flat_map`       | 2     | 30.1 | 1.29x | 26.1 |
| `om ref / inplace`        | 2     | 11.1 | 0.50x | 7.0  |
| `om ref / inplace_ind`    | 2     | 13.3 | 0.59x | 9.2  |

#### Cold (`clflush`), receiver used

clang/64, median of 7 passes.

| dispatch                  | arity | net  | disp | x net | x disp |
| ------------------------- | ----- | ---- | ---- | ----- | ------ |
| `vf`                      | 1     | 519  | 275  | 1.00x | 1.00x  |
| `om vptr / vptr_vector`   | 1     | 537  | 275  | 1.00x | 1.00x  |
| `om ref / vptr_vector`    | 1     | 1093 | 831  | 2.01x | 2.91x  |
| `om vptr / indirect`      | 1     | 600  | 335  | 1.18x | 1.33x  |
| `om ref / indirect`       | 1     | 1342 | 1076 | 2.56x | 4.22x  |
| `om vptr / vptr_map`      | 1     | 532  | 275  | 1.04x | 1.08x  |
| `om ref / vptr_map`       | 1     | 844  | 599  | 1.56x | 2.18x  |
| `om vptr / flat_map`      | 1     | 526  | 269  | 1.02x | 1.04x  |
| `om ref / flat_map`       | 1     | 817  | 555  | 1.55x | 2.06x  |
| `om ref / inplace`        | 1     | 608  | 357  | 1.18x | 1.38x  |
| `om ref / inplace_ind`    | 1     | 782  | 572  | 1.57x | 2.06x  |
| `vf+vf (double dispatch)` | 2     | 652  | 328  | 1.00x | 1.00x  |
| `om vptr / vptr_vector`   | 2     | 812  | 494  | 1.21x | 1.42x  |
| `om ref / vptr_vector`    | 2     | 1401 | 1079 | 2.22x | 3.32x  |
| `om vptr / indirect`      | 2     | 885  | 575  | 1.34x | 1.65x  |
| `om ref / indirect`       | 2     | 1686 | 1366 | 2.69x | 4.09x  |
| `om vptr / vptr_map`      | 2     | 833  | 512  | 1.26x | 1.46x  |
| `om ref / vptr_map`       | 2     | 1146 | 828  | 1.75x | 2.49x  |
| `om vptr / flat_map`      | 2     | 819  | 503  | 1.27x | 1.53x  |
| `om ref / flat_map`       | 2     | 1139 | 826  | 1.80x | 2.60x  |
| `om ref / inplace`        | 2     | 795  | 481  | 1.23x | 1.44x  |
| `om ref / inplace_ind`    | 2     | 1126 | 835  | 1.77x | 2.42x  |

### What the use world shows

- **Parity holds cold, in both columns**: `virtual_ptr` is 1.00x net and 1.00x
  disp of the virtual function — the receiver miss now paid by both sides and
  subtracted from both sides. Warm it is 1.18x, the same build-local yardstick
  effect as in the delivery world.
- **The deferred receiver miss costs nothing.** Cold net 565 with the body
  never touching the object, 537 with it read — against 257 for a lone
  receiver miss. (537 landing *below* 565 is scatter, not a saving: the use
  world's `vf` yardstick lands below the delivery world's by a similar
  margin. The signal is the absent 257.) The object address is in the fat
  pointer from the first cycle; the body's load completes under the table
  misses.
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

| call form      | arity | clang/64               | gcc/64                 | clang/32               | gcc/32                 |
| -------------- | ----- | ---------------------- | ---------------------- | ---------------------- | ---------------------- |
| `virtual_ptr`  | 1     | 6.4 → 6.7 (**+0.3**)   | 6.0 → 7.0 (**+1.0**)   | 11.6 → 14.8 (**+3.2**) | 6.9 → 9.2 (**+2.3**)   |
| `virtual_ptr`  | 2     | 7.0 → 8.0 (**+1.0**)   | 7.3 → 8.1 (**+0.8**)   | 11.5 → 13.4 (**+1.9**) | 2.2 → 4.5 (**+2.3**)   |
| `virtual_` ref | 1     | 11.3 → 14.0 (**+2.7**) | 11.5 → 13.6 (**+2.1**) | 15.0 → 17.9 (**+2.9**) | 18.1 → 23.0 (**+4.9**) |
| `virtual_` ref | 2     | 13.0 → 15.1 (**+2.1**) | 13.2 → 15.3 (**+2.1**) | 7.7 → 9.7 (**+2.0**)   | 13.9 → 16.7 (**+2.8**) |
| `inplace` ref  | 1     | 5.9 → 7.1 (**+1.2**)   | 6.1 → 7.4 (**+1.3**)   | 7.9 → 11.1 (**+3.2**)  | 11.0 → 16.1 (**+5.1**) |
| `inplace` ref  | 2     | 7.0 → 8.5 (**+1.5**)   | 6.8 → 8.2 (**+1.4**)   | 0.9 → 3.2 (**+2.3**)   | 6.5 → 8.2 (**+1.7**)   |

#### Cold (`clflush`) — the extra load, as a cache miss

| call form      | arity | clang/64               | gcc/64                 | clang/32               | gcc/32                 |
| -------------- | ----- | ---------------------- | ---------------------- | ---------------------- | ---------------------- |
| `virtual_ptr`  | 1     | 560 → 581 (**+21**)    | 561 → 618 (**+57**)    | 542 → 535 (**-7**)     | 511 → 551 (**+40**)    |
| `virtual_ptr`  | 2     | 872 → 944 (**+72**)    | 899 → 874 (**-25**)    | 840 → 825 (**-15**)    | 844 → 896 (**+52**)    |
| `virtual_` ref | 1     | 883 → 1082 (**+199**)  | 926 → 1001 (**+75**)   | 884 → 1069 (**+185**)  | 893 → 1132 (**+239**)  |
| `virtual_` ref | 2     | 1199 → 1381 (**+182**) | 1130 → 1308 (**+178**) | 1185 → 1358 (**+173**) | 1210 → 1447 (**+237**) |
| `inplace` ref  | 1     | 358 → 584 (**+226**)   | 385 → 568 (**+183**)   | 332 → 559 (**+227**)   | 346 → 545 (**+199**)   |
| `inplace` ref  | 2     | 500 → 896 (**+396**)   | 490 → 845 (**+355**)   | 602 → 875 (**+273**)   | 592 → 785 (**+193**)   |

### Reading the cost

- **Warm, a fraction of a cycle through a `virtual_ptr` and about three
  through a reference**: 1.14x → 1.15x and 1.74x → 2.04x on clang/64. On the
  reference
  path the load lands at the end of an already-long dependency chain (object →
  `type_info` → hash → vptr vector → **indirection** → v-table) with nothing
  to overlap against; through a `virtual_ptr` the chain is short and there is
  slack.
- **Cold, the reference path pays a full miss for it** — the indirection
  target is a line of its own. Through a `virtual_ptr` at arity 1 the four
  builds measure +21, +57, −7 and +40 cycles against nets of ~550: small, and
  inconsistently sized, because the independent slot load gives the miss
  something to hide under.
- **On an `inplace_vptr` hierarchy the same policy costs one to two cycles
  warm** (10.1 → 11.2 net at arity 1, 11.1 → 12.8 at arity 2) — the second
  dependent load's latency, giving back the tie with the virtual function that
  `inplace_vptr` had won.

If the program needs `initialize()` to be callable more than once, the price
is a load; if it does not, `indirect_vptr` is pure cost.

## Compiler and bitness

Four builds — clang++ 22.1 and g++ 13.3, each at `-m64` and `-m32` — measured
by `./matrix.sh`. Cells are `x net (net cycles)`: each row's total call cost
divided by the *same build's* yardstick, which is what makes columns
comparable — the compilers generate different code for the yardstick itself.
As everywhere, the `inplace` rows divide by their own hierarchy's yardstick,
not the `vf (yardstick)` rows shown.

The two are not the same vintage, and the reference column reflects that.
clang 22 is current; g++ 13.3 is what Ubuntu 24.04 still installs as `g++`,
released in 2023 and two major versions behind. It is kept because it is what
a great many people will actually compile this library with, but the
single-column sections above are the **clang/64** build, so the headline
numbers are a current compiler's. `matrix.sh` picks the newest `clang++-N` and
`g++-N` on the machine rather than whatever `clang++` and `g++` resolve to;
override with `CLANG=` and `GXX=`. Where the two disagree, it is worth asking
whether the answer is a code-generation fact or three years of gcc releases.

The bitness axis is real at the data-structure level: at `-m32`,
`sizeof(void*)` and the dispatch-table word halve to 4 bytes and
`virtual_ptr` halves to 8 bytes (still two words). All four builds of the
committed sources pass `--verify`. (Upstream CI exercises 32-bit builds for both MSVC and gcc, per
`.drone.jsonnet`; this benchmark adds measured 32-bit numbers.)

#### Caches cold (`clflush`)

| dispatch                | clang/64     | gcc/64       | clang/32     | gcc/32       |
| ----------------------- | ------------ | ------------ | ------------ | ------------ |
| **1 virtual argument**  |              |              |              |              |
| `vf (yardstick)`        | 1.00x (542)  | 1.00x (560)  | 1.00x (560)  | 1.00x (554)  |
| `om vptr / vptr_vector` | 1.03x (565)  | 0.99x (564)  | 0.99x (549)  | 0.96x (521)  |
| `om ref / vptr_vector`  | 2.08x (1150) | 2.12x (1184) | 2.07x (1149) | 2.12x (1142) |
| `om vptr / indirect`    | 1.08x (585)  | 1.11x (623)  | 0.96x (542)  | 1.01x (561)  |
| `om ref / indirect`     | 2.45x (1331) | 2.29x (1264) | 2.35x (1329) | 2.53x (1403) |
| `om vptr / vptr_map`    | 0.97x (528)  | 0.93x (520)  | 0.97x (539)  | 0.95x (524)  |
| `om ref / vptr_map`     | 1.60x (887)  | 1.62x (869)  | 1.45x (802)  | 1.50x (839)  |
| `om vptr / flat_map`    | 0.90x (500)  | 0.93x (505)  | 0.97x (540)  | 0.97x (544)  |
| `om ref / flat_map`     | 1.47x (807)  | 1.54x (829)  | 1.44x (799)  | 1.45x (789)  |
| `om ref / inplace`      | 1.16x (587)  | 1.29x (602)  | 1.09x (561)  | 1.09x (597)  |
| `om ref / inplace_ind`  | 1.57x (801)  | 1.60x (800)  | 1.57x (788)  | 1.54x (796)  |
| **2 virtual arguments** |              |              |              |              |
| `vf+vf (yardstick)`     | 1.00x (696)  | 1.00x (666)  | 1.00x (650)  | 1.00x (650)  |
| `om vptr / vptr_vector` | 1.25x (877)  | 1.36x (903)  | 1.29x (847)  | 1.32x (854)  |
| `om ref / vptr_vector`  | 2.14x (1501) | 2.22x (1444) | 2.31x (1503) | 2.37x (1518) |
| `om vptr / indirect`    | 1.36x (949)  | 1.35x (878)  | 1.26x (832)  | 1.43x (905)  |
| `om ref / indirect`     | 2.44x (1701) | 2.50x (1631) | 2.61x (1690) | 2.73x (1741) |
| `om vptr / vptr_map`    | 1.16x (798)  | 1.23x (796)  | 1.28x (825)  | 1.23x (799)  |
| `om ref / vptr_map`     | 1.71x (1165) | 1.79x (1150) | 1.85x (1182) | 1.80x (1164) |
| `om vptr / flat_map`    | 1.25x (850)  | 1.22x (781)  | 1.19x (788)  | 1.21x (766)  |
| `om ref / flat_map`     | 1.72x (1146) | 1.75x (1179) | 1.79x (1136) | 1.86x (1164) |
| `om ref / inplace`      | 1.18x (791)  | 1.29x (784)  | 1.47x (885)  | 1.46x (890)  |
| `om ref / inplace_ind`  | 1.62x (1188) | 1.97x (1137) | 1.93x (1176) | 1.78x (1096) |

Median of 7 passes. Spread across passes: median 18%, p90 29%.

#### Warm caches

| dispatch                | clang/64     | gcc/64       | clang/32     | gcc/32       |
| ----------------------- | ------------ | ------------ | ------------ | ------------ |
| **1 virtual argument**  |              |              |              |              |
| `vf (yardstick)`        | 1.00x (9.0)  | 1.00x (9.7)  | 1.00x (17.9) | 1.00x (14.6) |
| `om vptr / vptr_vector` | 1.14x (10.2) | 1.00x (9.6)  | 1.06x (18.5) | 1.15x (16.7) |
| `om ref / vptr_vector`  | 1.74x (15.5) | 1.58x (15.6) | 1.40x (25.0) | 1.60x (23.3) |
| `om vptr / indirect`    | 1.15x (10.4) | 1.08x (10.6) | 1.19x (21.8) | 1.32x (19.2) |
| `om ref / indirect`     | 2.04x (18.3) | 1.83x (17.7) | 1.57x (27.9) | 1.89x (27.6) |
| `om vptr / vptr_map`    | 1.08x (9.8)  | 1.01x (10.3) | 1.00x (18.1) | 1.16x (16.9) |
| `om ref / vptr_map`     | 2.70x (24.3) | 2.55x (24.3) | 1.80x (32.2) | 2.16x (31.1) |
| `om vptr / flat_map`    | 1.09x (9.9)  | 0.99x (9.5)  | 1.05x (19.4) | 1.20x (17.8) |
| `om ref / flat_map`     | 2.85x (26.6) | 2.56x (24.9) | 1.98x (35.2) | 2.14x (31.2) |
| `om ref / inplace`      | 1.05x (10.1) | 0.98x (10.4) | 1.10x (17.5) | 1.11x (16.2) |
| `om ref / inplace_ind`  | 1.18x (11.2) | 1.18x (11.3) | 1.36x (20.8) | 1.46x (20.9) |
| **2 virtual arguments** |              |              |              |              |
| `vf+vf (yardstick)`     | 1.00x (20.5) | 1.00x (19.6) | 1.00x (25.4) | 1.00x (24.6) |
| `om vptr / vptr_vector` | 0.54x (11.1) | 0.56x (10.9) | 0.75x (18.7) | 0.49x (11.9) |
| `om ref / vptr_vector`  | 0.82x (16.9) | 0.89x (17.4) | 0.71x (18.0) | 0.76x (18.6) |
| `om vptr / indirect`    | 0.58x (12.2) | 0.60x (11.7) | 0.82x (20.6) | 0.59x (14.4) |
| `om ref / indirect`     | 0.91x (19.0) | 0.99x (19.5) | 0.79x (20.0) | 0.88x (21.1) |
| `om vptr / vptr_map`    | 0.53x (11.1) | 0.55x (10.9) | 0.74x (18.6) | 0.49x (11.8) |
| `om ref / vptr_map`     | 1.55x (32.0) | 1.61x (31.5) | 1.21x (30.8) | 1.16x (28.4) |
| `om vptr / flat_map`    | 0.53x (11.1) | 0.56x (10.9) | 0.75x (18.8) | 0.48x (11.8) |
| `om ref / flat_map`     | 1.44x (29.5) | 1.48x (28.7) | 1.31x (33.1) | 1.26x (31.2) |
| `om ref / inplace`      | 0.54x (11.1) | 0.56x (11.0) | 0.44x (11.3) | 0.46x (11.1) |
| `om ref / inplace_ind`  | 0.63x (12.8) | 0.62x (12.4) | 0.54x (13.5) | 0.54x (13.0) |

Median of 7 passes. Spread across passes: median 9%, p90 26%.

### What it shows

- **Bitness buys nothing cold**: reference-dispatch nets are 1150 → 1149 on
  clang and 1184 → 1142 on gcc — halving every table does not save misses
  that are counted per line, not per byte.
- **The compilers agree on the mechanisms.** Cold, reference dispatch costs
  2.07x-2.12x the virtual function's total call cost on all four builds. Warm
  ratios scatter (the small yardstick denominators are layout-sensitive); read
  the cycle columns before the ratios there.
- **clang/32's `virtual_ptr` rows carry a harness artifact**: the i386 ABI
  marshals the 8-byte fat pointer through the stack inside the timed window,
  and clang 22 materializes the PIC base there too (~11.6 warm disp against
  6.0-6.9 on the other builds) — the by-value argument meeting that ABI, not
  the library.
- **`inplace` is the fastest reference dispatch in every column** — cold,
  1.09x-1.29x net at arity 1 (1.18x-1.47x at arity 2) across the four builds.

## gcc against clang, instruction by instruction

The obvious reading of compiler differences — that one generates better
dispatch — is wrong. The current timed regions, arity 1, `vptr_vector`,
`virtual_ptr` (start-stamp bookkeeping included, since it is what the window
contains):

```asm
; gcc — spills the fat pointer          ; clang — keeps it in registers
mov  rcx, QWORD PTR [rsp]               mov  r14d, edx
mov  r12d, eax                          neg  r14d
mov  rax, QWORD PTR [rip+slot]          mov  eax, eax
mov  ebx, edx                           shl  r14, 0x20
mov  rdi, QWORD PTR [rsp]               sub  r14, rax
shl  rbx, 0x20                          mov  rax, QWORD PTR [rip+slot]
call QWORD PTR [rcx+rax*8]              call QWORD PTR [rdi+rax*8]
```

Different register strategies, same critical path: one slot load and one
indirect call. They measure the same. The reference form is equivalent too:
nine dispatch instructions on both compilers, as the generated table below
reports once the start stamp's bookkeeping is set aside. What differs
across builds is the *yardstick*: a virtual call's warm cost is dominated by
indirect-branch misprediction (100 random targets), and predictor behavior
depends on binary layout — which is why warm ratios are build-local while
cold ratios agree. A side experiment with a single leaf class (perfectly
predicted) put the predicted virtual call at a few cycles and the mispredicted
one severalfold higher; the data is archived in [HISTORY.md](HISTORY.md).

## Reproducibility

- Every published cell is the **median of 7 passes**; `matrix.sh` interleaves
  the whole matrix per pass so drift lands on all columns alike, and clears
  stale run directories first (the results are committed, so stale runs are
  the norm, not an accident).
- Across passes, a single variant's `net` moves by a median of **14% cold (p90
  24%, worst 48%)** and **8% warm (p90 19%)** on this machine — an
  un-isolatable WSL2 guest. (The matrix captions quote a slightly larger
  spread — that one is measured on the *ratios*, so it also absorbs the
  yardstick's own motion.) Quote medians, not passes.
- **The built-in control**: the three direct registries' `om vptr` rows must
  agree — the vptr policy is not on that call path. Warm they agree to 0-8%
  per column (exactly 11.1 / 11.1 / 11.1 net on clang/64 at arity 2; the worst
  is gcc/64 at arity 1, ~8%); cold to 2-15%. `indirect` is excluded by
  design — its
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

| mode      | what it does                                                                                                                                                                                                                                                                                       |
| --------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `warm`    | nothing, after the warm-up call                                                                                                                                                                                                                                                                    |
| `clflush` | `clflushopt` over the receiver(s), the C++ v-table from its head (vptr−16, so the `type_info` slot at vptr−8 is always evicted), the method's `fn` object, the registry's dispatch-table arena, the vptr storage, and — for indirect registries — the per-class `static_vptr` cells; then `mfence` |
| `sweep`   | one store per line over a 64 MiB buffer (2x L3); diagnostic only, not in the published dataset                                                                                                                                                                                                     |

### Statistics

The reported statistic is the **mean of each variant's samples, trimmed of the
top 5%** (where preemptions land), with the standard error of the difference
in the console's `+/-` column. Individual samples are quantized: an
lfence-bracketed counter read costs 25 or 50 cycles bimodally here, so warm
medians sit on a tick (50, sometimes 75) while the trimmed mean resolves well
below one — which is also why sub-cycle warm differences should not be read
at all. TSC frequency is calibrated against `CLOCK_MONOTONIC` at startup;
the committed data implies ~2.50 GHz.

### Correctness gate

`--verify` runs before any measurement, and its oracles are computed **from
the class contracts alone**, carried in each body's returned id: `tag` for
arity 1 (every world), `tag`-if-same-leaf-else-−1 for const arity 2,
`a.tag + b.tag` / `−a.tag − b.tag − 1` for use arity 2 (distinct values, so
the gate can tell which overrider ran), `b.tag` for the dd yardstick. Every
dispatch path in every registry and both worlds is checked, plus the
yardsticks and the `touch` baseline. The oracles are deliberately independent
of the paths under test: comparing paths against each other would pass
whenever every registry is wrong the same way.

## Caveats

- **The receiver-touch baseline is a lower bound.** `touch` reads a member on
  the same cache line as the object's v-table pointer, so it prices the line's
  miss; a `virtual_` dispatch then chases that pointer to the `type_info` — a
  second miss `disp` rightly charges to dispatch, but `disp` is not purely
  "arithmetic plus an indirect call".
- **Warm ratios do not transfer across binaries.** The warm yardstick is
  mostly misprediction and its cost is layout-dependent (9.0-17.9 cycles net
  across the four builds). Cold `x net` is the portable figure.
- **`net` and `disp` differ by the plain-call cost by construction** (~4
  cycles warm): `net` subtracts the empty `probe`, `disp` subtracts a real
  call. Neither is wrong; they answer the two questions in "What the ratios
  divide".
- Cycles are TSC *reference* cycles at ~2.50 GHz, not core cycles; the core
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

These three are annotated by hand, and only the gcc/64 column: they are the
teaching version. "Every timed region, four builds side by side" below prints
the same windows for every dispatch and every build, straight from the
binaries — go there for anything the hand annotations do not cover, and if the
two ever disagree, the generated one is the one that was regenerated.

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

## Every timed region, four builds side by side

The listings above are gcc/64 and hand-picked. `asmtab.py` takes the same
window from all four binaries and every dispatch: the instructions
`timed_call<V>` executes between the start bracket and the `call` into the
body. Each variant is its own `noinline` instantiation, so the window is a
contiguous range inside a symbol the script finds by name — nothing here is
transcribed by hand, and `python3 asmtab.py` regenerates this section from
whatever is in `bin/`.

Operands that read a global are rewritten to the role the value plays —
`[rip+slot]`, `[got+mult]` — inferred from what the value is *used for* rather
than from its symbol offset, because the registry's fields sit at different
offsets in the 32- and 64-bit builds. The map registries are left out; their
probe inlines a container walk rather than the arithmetic the others do.

`got` is literal. i386 has no instruction-pointer-relative addressing, so
position-independent code has to find out where it is: it makes a call whose
*return address* is the wanted code address — gcc to a one-instruction helper,
clang to the next instruction — then pops it and adds a link-time constant,
after which every global is read off that register. That is the `call
pc_thunk` / `pop` / `add` sequence in the 32-bit columns, so named because
objdump renders clang's form as a call to the enclosing function plus an
offset, a hundred characters of mangled name. clang emits it **inside** the
timed window: on clang/32 every dispatch pays a call, a pop and an add before
it can read its first global. gcc sets its base up in the prologue, where it
costs the measurement nothing.

**What is shown is the dispatch, sliced out of the window.** Everything below
is the code that ran between the brackets, minus the instructions the timing
rig leaves there — identified by a backward slice from the call, so nothing
that reaches the call or its arguments can be dropped. Because a slice is a
heuristic and compilers move things, `probes.sh` compiles the same dispatches
standalone and `asmtab.py` fails the run if any instruction of the standalone
dispatch is missing from the slice.

**What the slice removes is the start stamp's bookkeeping.** `rdtsc` leaves
the counter split across `edx:eax`, and each compiler stitches the halves into
something the epilogue can subtract — gcc shifts and ors, clang negates and
subtracts, both spill to the stack at `-m32`. It is scheduled freely, it lands
anywhere in the window (per "Timing"), and left in it outnumbers the dispatch:
a virtual call on clang/64 is two instructions under five of harness. It is
identified by following the taint forward from `edx:eax` — an instruction is
bookkeeping when every value it reads is already part of the timestamp — so
nothing that touches the receiver, a global or an argument is ever dropped.

What is *not* elided is the i386 argument marshalling: at `-m32` the `lea`,
`sub esp` and `push` runs materialize the hidden `stamp_id` return slot and
the receivers inside the window, and the call really does pay for them. The
64-bit builds pass those in registers before the bracket opens.

### What the windows contain

Dispatch instructions, then the **dependent loads** — how many loads have to
complete one after another before the call has its target, counting the call's
own read of the v-table entry. (The listings earlier in this document count
that one separately, as "one dependent load, one indirect call"; the same `vf`
window is 1 + 1 there and 2 here.) The second number is the one that predicts
cost: the instruction count still includes the 32-bit marshalling, the chain
does not.

One row cannot be read like the others. The double-dispatch idiom's second
dispatch happens *inside* the first body — `Derived<N>::dd` calls
`other.dd_with(*this)` — so only its first call is in the window. Its two
dependent loads are half of what the `vf+vf` yardstick is timing; the rest is
compiled into the body.

| dispatch                | arity | clang/64 | gcc/64 | clang/32 | gcc/32 |
| ----------------------- | ----- | -------- | ------ | -------- | ------ |
| `vf (yardstick)`        | 1     | 2 / 2    | 2 / 2  | 9 / 2    | 5 / 2  |
| `om vptr / vptr_vector` | 1     | 2 / 2    | 3 / 2  | 12 / 2   | 9 / 2  |
| `om ref / vptr_vector`  | 1     | 9 / 4    | 9 / 4  | 16 / 4   | 12 / 4 |
| `om vptr / indirect`    | 1     | 3 / 2    | 4 / 3  | 15 / 3   | 10 / 2 |
| `om ref / indirect`     | 1     | 10 / 5   | 10 / 5 | 17 / 5   | 13 / 5 |
| `om ref / inplace`      | 1     | 3 / 2    | 3 / 2  | 8 / 2    | 6 / 2  |
| `om ref / inplace_ind`  | 1     | 4 / 3    | 4 / 3  | 11 / 3   | 7 / 3  |
| `vf+vf (yardstick)`     | 2     | 2 / 2    | 2 / 2  | 10 / 2   | 6 / 2  |
| `om vptr / vptr_vector` | 2     | 8 / 3    | 8 / 3  | 23 / 3   | 20 / 3 |
| `om ref / vptr_vector`  | 2     | 18 / 5   | 18 / 5 | 28 / 6   | 31 / 5 |
| `om vptr / indirect`    | 2     | 10 / 4   | 10 / 4 | 27 / 4   | 22 / 4 |
| `om ref / indirect`     | 2     | 20 / 6   | 20 / 6 | 30 / 7   | 34 / 6 |
| `om ref / inplace`      | 2     | 8 / 3    | 8 / 3  | 15 / 4   | 16 / 3 |
| `om ref / inplace_ind`  | 2     | 10 / 4   | 10 / 4 | 19 / 5   | 18 / 4 |

### The listings

`vf (yardstick)`, arity 1:

```asm
clang/64                   gcc/64                     clang/32                   gcc/32
-------------------------  -------------------------  -------------------------  -------------------------
mov rax, QWORD PTR [rdi]   mov rax, QWORD PTR [rdi]   call pc_thunk              mov eax, DWORD PTR [ecx]
call QWORD PTR [rax+0x10]  call QWORD PTR [rax+0x10]  pop ebx                    lea edx, [esp+0x4]
                                                      add ebx, 0x17ccf6          push ecx
                                                      mov eax, DWORD PTR [ecx]   push edx
                                                      sub esp, 0x8               call DWORD PTR [eax+0x8]
                                                      lea edx, [esp+0x8]
                                                      push ecx
                                                      push edx
                                                      call DWORD PTR [eax+0x8]
```

`om vptr / vptr_vector`, arity 1:

```asm
clang/64                           gcc/64                             clang/32                           gcc/32
---------------------------------  ---------------------------------  ---------------------------------  ---------------------------------
mov rax, QWORD PTR [rip+slot]      mov rcx, QWORD PTR [rsp]           add ebx, 0x17cb18                  mov ecx, DWORD PTR [got+slot]
call QWORD PTR [rdi+rax*8]         mov rax, QWORD PTR [rip+slot]      mov eax, DWORD PTR [esp+0x40]      lea edx, [esp+0x24]
                                   call QWORD PTR [rcx+rax*8]         mov ecx, DWORD PTR [esp+0x44]      sub esp, 0x4
                                                                      mov edx, DWORD PTR [got+slot]      mov DWORD PTR [esp+0x1c], esi
                                                                      mov edx, DWORD PTR [eax+edx*4]     mov DWORD PTR [esp+0x20], edi
                                                                      mov DWORD PTR [esp+0x18], eax      push DWORD PTR [esp+0x8]
                                                                      mov DWORD PTR [esp+0x1c], ecx      push DWORD PTR [esp+0x8]
                                                                      lea eax, [esp+0x20]                push edx
                                                                      vmovsd xmm0, QWORD PTR [esp+0x18]  call DWORD PTR [esi+ecx*4]
                                                                      vmovsd QWORD PTR [esp+0x4], xmm0
                                                                      mov DWORD PTR [esp], eax
                                                                      call edx
```

`om ref / vptr_vector`, arity 1:

```asm
clang/64                         gcc/64                           clang/32                         gcc/32
-------------------------------  -------------------------------  -------------------------------  -------------------------------
mov rsi, QWORD PTR [rdi]         mov rax, QWORD PTR [rdi]         call pc_thunk                    mov edx, DWORD PTR [edi]
mov rcx, QWORD PTR [rip+mult]    mov rdx, QWORD PTR [rax-0x8]     pop ebx                          lea eax, [esp+0x4]
movzx eax, BYTE PTR [rip+shift]  mov rax, QWORD PTR [rip+shift]   add ebx, 0x17c8aa                mov ecx, DWORD PTR [edx-0x4]
imul rcx, QWORD PTR [rsi-0x8]    imul rdx, QWORD PTR [rip+mult]   mov edx, DWORD PTR [got+mult]    mov edx, DWORD PTR [got+shift]
mov rsi, QWORD PTR [rip+vptrs]   shrx rdx, rdx, rax               mov eax, DWORD PTR [ebp+0x0]     imul ecx, DWORD PTR [got+mult]
shrx rax, rcx, rax               mov rax, QWORD PTR [rip+vptrs]   imul edx, DWORD PTR [eax-0x4]    shrx ecx, ecx, edx
mov rcx, QWORD PTR [rip+slot]    mov rax, QWORD PTR [rax+rdx*8]   movzx eax, BYTE PTR [got+shift]  mov edx, DWORD PTR [got+vptrs]
mov rax, QWORD PTR [rsi+rax*8]   mov rdx, QWORD PTR [rip+slot]    shrx eax, edx, eax               mov esi, DWORD PTR [got+slot]
call QWORD PTR [rax+rcx*8]       call QWORD PTR [rax+rdx*8]       mov edx, DWORD PTR [got+vptrs]   mov edx, DWORD PTR [edx+ecx*4]
                                                                  mov eax, DWORD PTR [edx+eax*4]   push edi
                                                                  mov edx, DWORD PTR [got+slot]    push eax
                                                                  sub esp, 0x8                     call DWORD PTR [edx+esi*4]
                                                                  lea ecx, [esp+0x8]
                                                                  push ebp
                                                                  push ecx
                                                                  call DWORD PTR [eax+edx*4]
```

`om vptr / indirect`, arity 1:

```asm
clang/64                           gcc/64                             clang/32                           gcc/32
---------------------------------  ---------------------------------  ---------------------------------  ---------------------------------
mov rcx, QWORD PTR [rip+slot]      mov rcx, QWORD PTR [rsp]           call pc_thunk                      lea edx, [esp+0x24]
mov rax, QWORD PTR [rdi]           mov rdx, QWORD PTR [rip+slot]      pop ebx                            sub esp, 0x4
call QWORD PTR [rax+rcx*8]         mov rax, QWORD PTR [rcx]           mov eax, DWORD PTR [esp+0x40]      mov DWORD PTR [esp+0x1c], esi
                                   call QWORD PTR [rax+rdx*8]         add ebx, 0x17ad3e                  mov DWORD PTR [esp+0x20], edi
                                                                      mov ebp, DWORD PTR [esp+0x44]      mov ecx, DWORD PTR [got+slot]
                                                                      mov edx, DWORD PTR [got+slot]      mov eax, DWORD PTR [esi]
                                                                      mov ecx, DWORD PTR [eax]           push DWORD PTR [esp+0x8]
                                                                      mov ecx, DWORD PTR [ecx+edx*4]     push DWORD PTR [esp+0x8]
                                                                      mov DWORD PTR [esp+0x1c], ebp      push edx
                                                                      mov DWORD PTR [esp+0x18], eax      call DWORD PTR [eax+ecx*4]
                                                                      lea eax, [esp+0x20]
                                                                      vmovsd xmm0, QWORD PTR [esp+0x18]
                                                                      vmovsd QWORD PTR [esp+0x4], xmm0
                                                                      mov DWORD PTR [esp], eax
                                                                      call ecx
```

`om ref / indirect`, arity 1:

```asm
clang/64                         gcc/64                           clang/32                         gcc/32
-------------------------------  -------------------------------  -------------------------------  -------------------------------
mov rsi, QWORD PTR [rdi]         mov rax, QWORD PTR [rdi]         call pc_thunk                    mov edx, DWORD PTR [edi]
mov rcx, QWORD PTR [rip+mult]    mov rdx, QWORD PTR [rax-0x8]     pop ebx                          lea eax, [esp+0x4]
movzx eax, BYTE PTR [rip+shift]  mov rax, QWORD PTR [rip+shift]   add ebx, 0x178b2a                mov ecx, DWORD PTR [edx-0x4]
imul rcx, QWORD PTR [rsi-0x8]    imul rdx, QWORD PTR [rip+mult]   mov edx, DWORD PTR [got+mult]    mov edx, DWORD PTR [got+shift]
mov rsi, QWORD PTR [rip+vptrs]   shrx rdx, rdx, rax               mov eax, DWORD PTR [ebp+0x0]     imul ecx, DWORD PTR [got+mult]
shrx rax, rcx, rax               mov rax, QWORD PTR [rip+vptrs]   imul edx, DWORD PTR [eax-0x4]    shrx ecx, ecx, edx
mov rcx, QWORD PTR [rip+slot]    mov rax, QWORD PTR [rax+rdx*8]   movzx eax, BYTE PTR [got+shift]  mov edx, DWORD PTR [got+vptrs]
mov rax, QWORD PTR [rsi+rax*8]   mov rdx, QWORD PTR [rip+slot]    shrx eax, edx, eax               mov edx, DWORD PTR [edx+ecx*4]
mov rax, QWORD PTR [rax]         mov rax, QWORD PTR [rax]         mov edx, DWORD PTR [got+vptrs]   mov ecx, DWORD PTR [got+slot]
call QWORD PTR [rax+rcx*8]       call QWORD PTR [rax+rdx*8]       mov eax, DWORD PTR [edx+eax*4]   mov edx, DWORD PTR [edx]
                                                                  mov edx, DWORD PTR [got+slot]    push edi
                                                                  mov eax, DWORD PTR [eax]         push eax
                                                                  sub esp, 0x8                     call DWORD PTR [edx+ecx*4]
                                                                  lea ecx, [esp+0x8]
                                                                  push ebp
                                                                  push ecx
                                                                  call DWORD PTR [eax+edx*4]
```

`om ref / inplace`, arity 1:

```asm
clang/64                       gcc/64                         clang/32                       gcc/32
-----------------------------  -----------------------------  -----------------------------  -----------------------------
mov rcx, QWORD PTR [rip+slot]  mov rax, QWORD PTR [rdi+0x8]   add ebx, 0x178194              mov edx, DWORD PTR [got+slot]
mov rax, QWORD PTR [rdi+0x8]   mov rdx, QWORD PTR [rip+slot]  mov edx, DWORD PTR [got+slot]  mov eax, DWORD PTR [ecx+0x4]
call QWORD PTR [rax+rcx*8]     call QWORD PTR [rax+rdx*8]     mov eax, DWORD PTR [ecx+0x4]   lea edi, [esp+0x4]
                                                              sub esp, 0x8                   push ecx
                                                              lea ebp, [esp+0x8]             push edi
                                                              push ecx                       call DWORD PTR [eax+edx*4]
                                                              push ebp
                                                              call DWORD PTR [eax+edx*4]
```

`om ref / inplace_ind`, arity 1:

```asm
clang/64                       gcc/64                         clang/32                       gcc/32
-----------------------------  -----------------------------  -----------------------------  -----------------------------
mov rcx, QWORD PTR [rdi+0x8]   mov rax, QWORD PTR [rdi+0x8]   call pc_thunk                  mov eax, DWORD PTR [ecx+0x4]
mov rax, QWORD PTR [rcx]       mov rdx, QWORD PTR [rip+slot]  pop ebx                        mov edx, DWORD PTR [got+slot]
mov rcx, QWORD PTR [rip+slot]  mov rax, QWORD PTR [rax]       add ebx, 0x1779ea              lea edi, [esp+0x4]
call QWORD PTR [rax+rcx*8]     call QWORD PTR [rax+rdx*8]     mov edx, DWORD PTR [got+slot]  mov eax, DWORD PTR [eax]
                                                              mov eax, DWORD PTR [ecx+0x4]   push ecx
                                                              mov eax, DWORD PTR [eax]       push edi
                                                              sub esp, 0x8                   call DWORD PTR [eax+edx*4]
                                                              lea ebp, [esp+0x8]
                                                              push ecx
                                                              push ebp
                                                              call DWORD PTR [eax+edx*4]
```

`vf+vf (yardstick)`, arity 2:

```asm
clang/64                   gcc/64                     clang/32                   gcc/32
-------------------------  -------------------------  -------------------------  -------------------------
mov rax, QWORD PTR [rdi]   mov rax, QWORD PTR [rdi]   call pc_thunk              mov eax, DWORD PTR [ecx]
call QWORD PTR [rax+0x20]  call QWORD PTR [rax+0x20]  pop ebx                    lea edx, [esp+0x4]
                                                      add ebx, 0x17cc96          push edi
                                                      mov eax, DWORD PTR [ecx]   push ecx
                                                      sub esp, 0x4               push edx
                                                      lea edx, [esp+0x4]         call DWORD PTR [eax+0x10]
                                                      push DWORD PTR [esp+0x28]
                                                      push ecx
                                                      push edx
                                                      call DWORD PTR [eax+0x10]
```

`om vptr / vptr_vector`, arity 2:

```asm
clang/64                           gcc/64                             clang/32                           gcc/32
---------------------------------  ---------------------------------  ---------------------------------  ---------------------------------
mov rdx, QWORD PTR [rsp+0x30]      mov rcx, QWORD PTR [rip+slot]      mov DWORD PTR [esp+0x1c], edx      mov DWORD PTR [esp+0xc], eax
mov r8, QWORD PTR [rip+slot]       mov rax, QWORD PTR [rsp+0x30]      add ebx, 0x17c748                  lea eax, [esp+0x24]
mov rdi, QWORD PTR [rsp+0x20]      mov rdi, QWORD PTR [rsp+0x20]      mov edx, DWORD PTR [esp+0x58]      sub esp, 0xc
mov rax, QWORD PTR [rip+slot]      mov rax, QWORD PTR [rax+rcx*8]     mov ecx, DWORD PTR [esp+0x50]      mov ebp, DWORD PTR [got+slot]
mov r8, QWORD PTR [rdx+r8*8]       mov rcx, QWORD PTR [rip+slot]      mov ebp, DWORD PTR [got+slot]      mov esi, DWORD PTR [got+slot]
imul r8, QWORD PTR [rip+stride]    imul rax, QWORD PTR [rip+stride]   mov eax, DWORD PTR [got+slot]      vmovq xmm2, QWORD PTR [esp+0x5c]
mov rax, QWORD PTR [rdi+rax*8]     mov r8, QWORD PTR [rdi+rcx*8]      mov ebp, DWORD PTR [edx+ebp*4]     vmovq xmm3, QWORD PTR [esp+0x64]
call QWORD PTR [rax+r8*8]          call QWORD PTR [r8+rax*8]          mov eax, DWORD PTR [ecx+eax*4]     mov edi, DWORD PTR [esp+0x64]
                                                                      imul ebp, DWORD PTR [got+stride]   mov edx, DWORD PTR [esp+0x5c]
                                                                      mov edi, DWORD PTR [eax+ebp*4]     vmovq QWORD PTR [esp+0x24], xmm2
                                                                      mov eax, DWORD PTR [esp+0x5c]      vmovq QWORD PTR [esp+0x1c], xmm3
                                                                      mov ebp, DWORD PTR [esp+0x54]      mov edi, DWORD PTR [edi+ebp*4]
                                                                      mov DWORD PTR [esp+0x20], edx      mov edx, DWORD PTR [edx+esi*4]
                                                                      mov DWORD PTR [esp+0x28], ecx      push DWORD PTR [esp+0x68]
                                                                      mov DWORD PTR [esp+0x2c], ebp      imul edi, DWORD PTR [got+stride]
                                                                      mov DWORD PTR [esp+0x24], eax      push DWORD PTR [esp+0x68]
                                                                      lea eax, [esp+0x30]                push DWORD PTR [esp+0x68]
                                                                      vmovsd xmm0, QWORD PTR [esp+0x20]  push DWORD PTR [esp+0x68]
                                                                      vmovsd QWORD PTR [esp+0xc], xmm0   push eax
                                                                      vmovsd xmm0, QWORD PTR [esp+0x28]  call DWORD PTR [edx+edi*4]
                                                                      vmovsd QWORD PTR [esp+0x4], xmm0
                                                                      mov DWORD PTR [esp], eax
                                                                      call edi
```

`om ref / vptr_vector`, arity 2:

```asm
clang/64                            gcc/64                              clang/32                            gcc/32
----------------------------------  ----------------------------------  ----------------------------------  ----------------------------------
mov rdx, QWORD PTR [rdi]            mov rax, QWORD PTR [rdi]            mov edi, DWORD PTR [esp+0x34]       lea edi, [esp+0x24]
mov rcx, QWORD PTR [rip+mult]       mov rcx, QWORD PTR [rip+mult]       mov DWORD PTR [esp+0xc], edx        mov ebp, DWORD PTR [got+vptrs]
mov r10, QWORD PTR [rsi]            mov r8, QWORD PTR [rip+shift]       call pc_thunk                       mov DWORD PTR [esp+0x10], eax
mov r8, QWORD PTR [rip+vptrs]       mov rdx, QWORD PTR [rip+vptrs]      pop ebx                             mov eax, DWORD PTR [got+mult]
mov r9, QWORD PTR [rip+slot]        mov r10, QWORD PTR [rax-0x8]        add ebx, 0x17c514                   mov DWORD PTR [esp+0x18], edi
mov rax, QWORD PTR [rdx-0x8]        imul r10, rcx                       mov eax, DWORD PTR [got+mult]       mov edi, DWORD PTR [ebx]
movzx edx, BYTE PTR [rip+shift]     shrx rax, r10, r8                   mov edx, DWORD PTR [ecx]            mov DWORD PTR [esp+0x14], edx
imul rax, rcx                       mov r9, QWORD PTR [rdx+rax*8]       mov ebp, DWORD PTR [edi]            mov edx, DWORD PTR [got+shift]
imul rcx, QWORD PTR [r10-0x8]       mov rax, QWORD PTR [rsi]            movzx ecx, BYTE PTR [got+shift]     sub esp, 0x4
shrx rax, rax, rdx                  imul rcx, QWORD PTR [rax-0x8]       mov edx, DWORD PTR [edx-0x4]        mov DWORD PTR [esp+0x10], ebp
mov rax, QWORD PTR [r8+rax*8]       shrx rcx, rcx, r8                   imul edx, eax                       mov ebp, DWORD PTR [edi-0x4]
shrx rcx, rcx, rdx                  mov rax, QWORD PTR [rdx+rcx*8]      imul eax, DWORD PTR [ebp-0x4]       imul ebp, eax
mov rdx, QWORD PTR [rip+slot]       mov rdx, QWORD PTR [rip+slot]       mov ebp, DWORD PTR [got+vptrs]      shrx edi, ebp, edx
mov rcx, QWORD PTR [r8+rcx*8]       mov rax, QWORD PTR [rax+rdx*8]      shrx edx, edx, ecx                  mov ebp, DWORD PTR [got+slot]
mov rax, QWORD PTR [rax+r9*8]       mov rdx, QWORD PTR [rip+slot]       shrx ecx, eax, ecx                  mov DWORD PTR [esp+0x20], ebp
mov rcx, QWORD PTR [rcx+rdx*8]      imul rax, QWORD PTR [rip+stride]    mov eax, DWORD PTR [ebp+edx*4+0x0]  mov ebp, DWORD PTR [esp+0x10]
imul rcx, QWORD PTR [rip+stride]    mov rdx, QWORD PTR [r9+rdx*8]       mov edx, DWORD PTR [got+slot]       mov edi, DWORD PTR [ebp+edi*4+0x0]
call QWORD PTR [rax+rcx*8]          call QWORD PTR [rdx+rax*8]          mov ecx, DWORD PTR [ebp+ecx*4+0x0]  mov ebp, DWORD PTR [esi]
                                                                        mov eax, DWORD PTR [eax+edx*4]      imul eax, DWORD PTR [ebp-0x4]
                                                                        mov edx, DWORD PTR [got+slot]       mov ebp, DWORD PTR [esp+0x10]
                                                                        mov edx, DWORD PTR [ecx+edx*4]      shrx eax, eax, edx
                                                                        imul edx, DWORD PTR [got+stride]    mov edx, DWORD PTR [got+slot]
                                                                        sub esp, 0x4                        mov eax, DWORD PTR [ebp+eax*4+0x0]
                                                                        lea ecx, [esp+0x14]                 mov ebp, DWORD PTR [esp+0x20]
                                                                        push edi                            mov eax, DWORD PTR [eax+edx*4]
                                                                        push DWORD PTR [esp+0x38]           mov edx, DWORD PTR [edi+ebp*4]
                                                                        push ecx                            push esi
                                                                        call DWORD PTR [eax+edx*4]          imul eax, DWORD PTR [got+stride]
                                                                                                            push ebx
                                                                                                            push DWORD PTR [esp+0x24]
                                                                                                            call DWORD PTR [edx+eax*4]
```

`om vptr / indirect`, arity 2:

```asm
clang/64                            gcc/64                              clang/32                            gcc/32
----------------------------------  ----------------------------------  ----------------------------------  ----------------------------------
mov rdi, QWORD PTR [rsp+0x20]       mov rax, QWORD PTR [rsp+0x30]       call pc_thunk                       mov DWORD PTR [esp+0xc], eax
mov rdx, QWORD PTR [rsp+0x30]       mov rdi, QWORD PTR [rsp+0x20]       pop ebx                             lea eax, [esp+0x24]
mov r8, QWORD PTR [rip+slot]        mov rcx, QWORD PTR [rip+slot]       mov DWORD PTR [esp+0x1c], edx       sub esp, 0xc
mov r9, QWORD PTR [rip+slot]        mov rax, QWORD PTR [rax]            mov ecx, DWORD PTR [esp+0x50]       mov ebp, DWORD PTR [got+slot]
mov rax, QWORD PTR [rdi]            mov rdx, QWORD PTR [rdi]            add ebx, 0x1789c0                   vmovq xmm2, QWORD PTR [esp+0x5c]
mov rax, QWORD PTR [rax+r8*8]       mov rax, QWORD PTR [rax+rcx*8]      mov edx, DWORD PTR [esp+0x58]       vmovq xmm3, QWORD PTR [esp+0x64]
mov r8, QWORD PTR [rdx]             mov rcx, QWORD PTR [rip+slot]       mov ebp, DWORD PTR [got+slot]       mov edx, DWORD PTR [esp+0x5c]
mov r8, QWORD PTR [r8+r9*8]         imul rax, QWORD PTR [rip+stride]    mov edi, DWORD PTR [got+slot]       mov edi, DWORD PTR [esp+0x64]
imul r8, QWORD PTR [rip+stride]     mov r8, QWORD PTR [rdx+rcx*8]       mov eax, DWORD PTR [ecx]            mov esi, DWORD PTR [got+slot]
call QWORD PTR [rax+r8*8]           call QWORD PTR [r8+rax*8]           mov eax, DWORD PTR [eax+ebp*4]      vmovq QWORD PTR [esp+0x24], xmm2
                                                                        mov ebp, DWORD PTR [edx]            mov edx, DWORD PTR [edx]
                                                                        mov edi, DWORD PTR [ebp+edi*4+0x0]  vmovq QWORD PTR [esp+0x1c], xmm3
                                                                        mov ebp, DWORD PTR [esp+0x5c]       mov edi, DWORD PTR [edi]
                                                                        imul edi, DWORD PTR [got+stride]    mov edx, DWORD PTR [edx+esi*4]
                                                                        mov eax, DWORD PTR [eax+edi*4]      mov edi, DWORD PTR [edi+ebp*4]
                                                                        mov edi, DWORD PTR [esp+0x54]       push DWORD PTR [esp+0x68]
                                                                        mov DWORD PTR [esp+0x28], ecx       push DWORD PTR [esp+0x68]
                                                                        mov DWORD PTR [esp+0x20], edx       imul edi, DWORD PTR [got+stride]
                                                                        mov DWORD PTR [esp+0x24], ebp       push DWORD PTR [esp+0x68]
                                                                        lea ecx, [esp+0x30]                 push DWORD PTR [esp+0x68]
                                                                        mov DWORD PTR [esp+0x2c], edi       push eax
                                                                        vmovsd xmm0, QWORD PTR [esp+0x20]   call DWORD PTR [edx+edi*4]
                                                                        vmovsd QWORD PTR [esp+0xc], xmm0
                                                                        vmovsd xmm0, QWORD PTR [esp+0x28]
                                                                        vmovsd QWORD PTR [esp+0x4], xmm0
                                                                        mov DWORD PTR [esp], ecx
                                                                        call eax
```

`om ref / indirect`, arity 2:

```asm
clang/64                            gcc/64                              clang/32                            gcc/32
----------------------------------  ----------------------------------  ----------------------------------  ----------------------------------
mov rdx, QWORD PTR [rdi]            mov rax, QWORD PTR [rdi]            mov edi, DWORD PTR [esp+0x34]       mov edi, DWORD PTR [got+shift]
mov rcx, QWORD PTR [rip+mult]       mov rcx, QWORD PTR [rip+mult]       mov DWORD PTR [esp+0xc], edx        mov ebp, DWORD PTR [ebx]
mov r10, QWORD PTR [rsi]            mov r8, QWORD PTR [rip+shift]       call pc_thunk                       mov DWORD PTR [esp+0x10], eax
mov r8, QWORD PTR [rip+vptrs]       mov rdx, QWORD PTR [rip+vptrs]      pop ebx                             mov eax, DWORD PTR [got+mult]
mov r9, QWORD PTR [rip+slot]        mov r10, QWORD PTR [rax-0x8]        add ebx, 0x178794                   mov DWORD PTR [esp+0x14], edx
mov rax, QWORD PTR [rdx-0x8]        imul r10, rcx                       mov eax, DWORD PTR [got+mult]       mov edx, DWORD PTR [got+vptrs]
movzx edx, BYTE PTR [rip+shift]     shrx rax, r10, r8                   mov edx, DWORD PTR [ecx]            mov DWORD PTR [esp+0xc], edi
imul rax, rcx                       mov rax, QWORD PTR [rdx+rax*8]      mov ebp, DWORD PTR [edi]            lea edi, [esp+0x24]
imul rcx, QWORD PTR [r10-0x8]       mov r9, QWORD PTR [rax]             movzx ecx, BYTE PTR [got+shift]     sub esp, 0x4
shrx rax, rax, rdx                  mov rax, QWORD PTR [rsi]            mov edx, DWORD PTR [edx-0x4]        mov DWORD PTR [esp+0x1c], edi
mov rax, QWORD PTR [r8+rax*8]       imul rcx, QWORD PTR [rax-0x8]       imul edx, eax                       mov edi, DWORD PTR [ebp-0x4]
shrx rcx, rcx, rdx                  shrx rcx, rcx, r8                   imul eax, DWORD PTR [ebp-0x4]       imul edi, eax
mov rdx, QWORD PTR [rip+slot]       mov rax, QWORD PTR [rdx+rcx*8]      mov ebp, DWORD PTR [got+vptrs]      mov ebp, edi
mov rcx, QWORD PTR [r8+rcx*8]       mov rdx, QWORD PTR [rip+slot]       shrx edx, edx, ecx                  movzx edi, BYTE PTR [esp+0x10]
mov rax, QWORD PTR [rax]            mov rax, QWORD PTR [rax]            shrx ecx, eax, ecx                  shrx ebp, ebp, edi
mov rcx, QWORD PTR [rcx]            mov rax, QWORD PTR [rax+rdx*8]      mov eax, DWORD PTR [ebp+edx*4+0x0]  mov ebp, DWORD PTR [edx+ebp*4]
mov rax, QWORD PTR [rax+r9*8]       mov rdx, QWORD PTR [rip+slot]       mov edx, DWORD PTR [got+slot]       mov edi, DWORD PTR [got+slot]
mov rcx, QWORD PTR [rcx+rdx*8]      imul rax, QWORD PTR [rip+stride]    mov ecx, DWORD PTR [ebp+ecx*4+0x0]  mov ebp, DWORD PTR [ebp+0x0]
imul rcx, QWORD PTR [rip+stride]    mov rdx, QWORD PTR [r9+rdx*8]       mov eax, DWORD PTR [eax]            mov DWORD PTR [esp+0x20], ebp
call QWORD PTR [rax+rcx*8]          call QWORD PTR [rdx+rax*8]          mov ecx, DWORD PTR [ecx]            mov ebp, DWORD PTR [esi]
                                                                        mov eax, DWORD PTR [eax+edx*4]      imul eax, DWORD PTR [ebp-0x4]
                                                                        mov edx, DWORD PTR [got+slot]       movzx ebp, BYTE PTR [esp+0x10]
                                                                        mov edx, DWORD PTR [ecx+edx*4]      shrx eax, eax, ebp
                                                                        imul edx, DWORD PTR [got+stride]    mov eax, DWORD PTR [edx+eax*4]
                                                                        sub esp, 0x4                        mov edx, DWORD PTR [got+slot]
                                                                        lea ecx, [esp+0x14]                 mov eax, DWORD PTR [eax]
                                                                        push edi                            mov eax, DWORD PTR [eax+edx*4]
                                                                        push DWORD PTR [esp+0x38]           mov edx, DWORD PTR [esp+0x20]
                                                                        push ecx                            imul eax, DWORD PTR [got+stride]
                                                                        call DWORD PTR [eax+edx*4]          mov edx, DWORD PTR [edx+edi*4]
                                                                                                            push esi
                                                                                                            push ebx
                                                                                                            push DWORD PTR [esp+0x24]
                                                                                                            call DWORD PTR [edx+eax*4]
```

`om ref / inplace`, arity 2:

```asm
clang/64                          gcc/64                            clang/32                          gcc/32
--------------------------------  --------------------------------  --------------------------------  --------------------------------
mov rdx, QWORD PTR [rdi+0x8]      mov rcx, QWORD PTR [rip+slot]     mov ebp, DWORD PTR [esp+0x24]     mov edx, DWORD PTR [got+slot]
mov rcx, QWORD PTR [rip+slot]     mov rax, QWORD PTR [rsi+0x8]      add ebx, 0x178004                 mov DWORD PTR [esp+0x8], eax
mov rax, QWORD PTR [rdx+rcx*8]    mov rdx, QWORD PTR [rdi+0x8]      mov edx, DWORD PTR [got+slot]     lea eax, [esp+0x14]
mov rcx, QWORD PTR [rsi+0x8]      mov rax, QWORD PTR [rax+rcx*8]    mov eax, DWORD PTR [ecx+0x4]      sub esp, 0x4
mov rdx, QWORD PTR [rip+slot]     mov rcx, QWORD PTR [rip+slot]     mov ecx, DWORD PTR [got+slot]     mov edi, DWORD PTR [got+slot]
mov rcx, QWORD PTR [rcx+rdx*8]    imul rax, QWORD PTR [rip+stride]  mov eax, DWORD PTR [eax+edx*4]    mov DWORD PTR [esp+0x10], edx
imul rcx, QWORD PTR [rip+stride]  mov rdx, QWORD PTR [rdx+rcx*8]    mov edx, DWORD PTR [ebp+0x4]      mov edx, DWORD PTR [esi+0x4]
call QWORD PTR [rax+rcx*8]        call QWORD PTR [rdx+rax*8]        mov edx, DWORD PTR [edx+ecx*4]    mov edi, DWORD PTR [edx+edi*4]
                                                                    imul edx, DWORD PTR [got+stride]  mov edx, DWORD PTR [esp+0x10]
                                                                    sub esp, 0x4                      imul edi, DWORD PTR [got+stride]
                                                                    lea ecx, [esp+0x4]                mov ecx, DWORD PTR [ebx+0x4]
                                                                    push ebp                          mov edx, DWORD PTR [ecx+edx*4]
                                                                    push DWORD PTR [esp+0x28]         push esi
                                                                    push ecx                          push ebx
                                                                    call DWORD PTR [eax+edx*4]        push eax
                                                                                                      call DWORD PTR [edx+edi*4]
```

`om ref / inplace_ind`, arity 2:

```asm
clang/64                          gcc/64                            clang/32                          gcc/32
--------------------------------  --------------------------------  --------------------------------  --------------------------------
mov rcx, QWORD PTR [rdi+0x8]      mov rax, QWORD PTR [rdi+0x8]      call pc_thunk                     lea edi, [esp+0x14]
mov rdx, QWORD PTR [rip+slot]     mov rcx, QWORD PTR [rip+slot]     pop ebx                           mov DWORD PTR [esp+0x4], eax
mov rax, QWORD PTR [rcx]          mov rdx, QWORD PTR [rax]          mov ebp, DWORD PTR [esp+0x24]     mov eax, DWORD PTR [ebx+0x4]
mov rcx, QWORD PTR [rip+slot]     mov rax, QWORD PTR [rsi+0x8]      add ebx, 0x1758ba                 mov ebp, DWORD PTR [got+slot]
mov rax, QWORD PTR [rax+rcx*8]    mov rax, QWORD PTR [rax]          mov edx, DWORD PTR [got+slot]     mov DWORD PTR [esp+0xc], edi
mov rcx, QWORD PTR [rsi+0x8]      mov rax, QWORD PTR [rax+rcx*8]    mov eax, DWORD PTR [ecx+0x4]      mov edi, DWORD PTR [esi+0x4]
mov rcx, QWORD PTR [rcx]          mov rcx, QWORD PTR [rip+slot]     mov eax, DWORD PTR [eax]          mov DWORD PTR [esp+0x8], edx
mov rcx, QWORD PTR [rcx+rdx*8]    imul rax, QWORD PTR [rip+stride]  mov eax, DWORD PTR [eax+edx*4]    mov edx, DWORD PTR [got+slot]
imul rcx, QWORD PTR [rip+stride]  mov rdx, QWORD PTR [rdx+rcx*8]    mov edx, DWORD PTR [ebp+0x4]      sub esp, 0x4
call QWORD PTR [rax+rcx*8]        call QWORD PTR [rdx+rax*8]        mov ebp, DWORD PTR [got+slot]     mov eax, DWORD PTR [eax]
                                                                    mov edx, DWORD PTR [edx]          mov edi, DWORD PTR [edi]
                                                                    mov edx, DWORD PTR [edx+ebp*4]    mov eax, DWORD PTR [eax+edx*4]
                                                                    imul edx, DWORD PTR [got+stride]  mov edi, DWORD PTR [edi+ebp*4]
                                                                    sub esp, 0x4                      push esi
                                                                    lea ecx, [esp+0x4]                push ebx
                                                                    push DWORD PTR [esp+0x28]         imul edi, DWORD PTR [got+stride]
                                                                    push DWORD PTR [esp+0x28]         push DWORD PTR [esp+0x18]
                                                                    push ecx                          call DWORD PTR [eax+edi*4]
                                                                    call DWORD PTR [eax+edx*4]
```

### What the windows show

- **The reference chain is the same depth on every build**: 4 dependent loads
  at arity 1 — receiver, `type_info`, vptr vector, open-method v-table — and 5
  through `indirect`. What differs across the four columns is register
  allocation and where the stamp bookkeeping lands, not the chain. That is the
  structural reason the cold ratios agree across builds while the warm ones do
  not.
- **A `virtual_ptr` call is 2 dependent loads, exactly what a virtual function
  call is** (2) — but not the same 2. The virtual call must read the receiver
  before it can read its v-table; the `virtual_ptr` call already has the
  v-table and spends its chain on the slot and the v-table entry the call
  reads through it. Nothing has to find the receiver first, which is why the
  two come out level. The parity in the results tables is not a coincidence of
  this machine; it is in the instruction stream.
- **`indirect_vptr` adds exactly one dependent load — where there is no slack
  to hide it.** Through a reference it deepens the chain in every column (4 →
  5), and on the inplace hierarchy too (2 → 3). Through a `virtual_ptr` at
  arity 1 it deepens gcc/64 and clang/32, which reach the fat pointer through
  memory, and disappears on clang/64 and gcc/32, which keep it in a register
  and so have the extra dereference to spend (2 → 2 there, 2 → 3 on the other
  two). At arity 2 the slack is gone and it costs a load everywhere (3 → 4).
  That is the same unevenness the cycle tables show, where the policy costs a
  `virtual_ptr` a fraction of what it costs a reference.
- **The 32-bit windows are longer but no deeper.** gcc/32's reference window
  is 12 instructions against gcc/64's 9, and all of the difference is argument
  marshalling and narrower registers — the same 4 dependent loads. Halving
  every pointer does not shorten the chain, which is the instruction-level
  version of "bitness buys nothing cold".
- **clang/32's `virtual_ptr` rows show the artifact the tables warn about**:
  the 8-byte fat pointer is passed by value, and the i386 ABI moves it through
  `vmovsd` and the stack inside the window — 12 and 23 instructions against 2
  and 8 on clang/64, for the same 2- and 3-load chains.
- **At two virtual arguments the multi-method's two lookups are independent**:
  each virtual argument's v-table is fetched on its own chain, one `imul` by
  the table stride combines them, and a single indexed call ends it — depth 3
  through a `virtual_ptr` against the idiom's two chained calls, which is the
  shape behind the multi-method's warm advantage over the idiom.

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
python3 report.py                # regenerates every measured table in this README
./probes.sh                      # the disassembly oracle -> bin/probe-*
python3 asmtab.py                # regenerates the timed regions, from bin/
```

Benchmark flags: `--reps --objects --sweep-mb --cpu --seed
--mode warm|clflush|sweep|all --csv --verify`. The class count is
compile-time: `CLASSES=1000 ./build.sh` or `-DOMB_CLASSES` with CMake.

## Files

| file                  |                                                                            |
| --------------------- | -------------------------------------------------------------------------- |
| `src/timing.hpp`      | brackets, stamps, cache scrubbing, TSC calibration, statistics             |
| `src/hierarchy.hpp`   | both class hierarchies, yardsticks, the `touch` baselines                  |
| `src/registries.hpp`  | the six dispatch configurations and the const-body methods                 |
| `src/use_methods.hpp` | the use-body methods and overriders                                        |
| `src/main.cpp`        | variants, measurement loop, verification, reporting                        |
| `matrix.sh`           | builds, verifies and measures all four compiler x bitness builds, N passes |
| `run.sh`              | single-build driver on a pinned core                                       |
| `report.py`           | regenerates every measured section of this README from `results/`          |
| `asmtab.py`           | regenerates "Every timed region" by disassembling the four binaries        |
| `src/dispatch_probe.cpp` | every dispatch compiled standalone: the oracle asmtab.py checks its slice against |
| `probes.sh`           | builds that oracle for all four configurations                             |
| `results/run1..7/`    | the committed dataset behind every table                                   |
| `include`             | symlink into a Boost checkout (not committed)                              |

Overriders are registered in bulk through the core API (`method<...>::override`
takes a pack), since the leaves are a class template.

## History

This benchmark was corrected into its current design through an adversarial
review and several measurement-scheme redesigns, each of which changed what
the timed window contains — and therefore what the numbers mean.
[HISTORY.md](HISTORY.md) is the chronicle.
