# Boost.OpenMethod dispatch benchmarks

This benchmark measures the cost of an open-method dispatch, using a virtual
function call as a yardstick, with the caches deliberately scrubbed. The
headline, on this machine: a `virtual_ptr` call costs what a virtual function
call costs (1.01x), `virtual_` reference dispatch costs about 2x a virtual
call — 2.12x, 1.99x and 2.08x on the three compilers — and at two virtual
arguments the double-dispatch idiom's two lean v-table chains beat the open
multi-method, which costs a modest 1.32x.

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

Five axes:

| axis      | values                                                                                                                                                                                                                                                                                                                                                                                         |
| --------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| dispatch  | how the v-table pointer is reached: `vptr_vector` (the default registry: `std_rtti` + `fast_perfect_hash` + `vptr_vector`), `indirect` (the default plus `indirect_vptr`), `inplace` (`inplace_vptr`: the pointer stored in the object), `inplace_ind` (`inplace_vptr` plus `indirect_vptr`) |
| call form | `virtual_<const Base&>` (the v-table pointer is looked up at the call site) vs `virtual_ptr<const Base, R>` (already carries it)                                                                                                                                                                                                                                                               |
| arity     | 1 and 2 virtual arguments                                                                                                                                                                                                                                                                                                                                                                      |
| body      | `const` — bodies return a compile-time constant, the receiver is touched only where the mechanism requires it; `use` — every body reads a member of every receiver                                                                                                                                                                                                                             |
| compiler  | clang++ 22.1, g++ 15.2, g++ 13.3 — listed in `compilers.conf`; the first is the reference                                                                                                                                                                                                                                                                                                      |

Two more registries are measured but not published: `vptr_map` over
`std::unordered_map` and over `boost::unordered_flat_map`. `clflush` cannot
reach a hash map's bucket arrays — they are runtime-allocated, and only the
container header is flushed — so their cold rows keep interior state resident
and read better than a truly cold map would. They stay in the code, and in
`report.py`'s control check, because their `virtual_ptr` rows are how it
verifies that the vptr policy is off that call path.

All four published dispatch values are registries in the code; the last two
have no `vptr` policy at all — the pointer lives in the object via the `inplace_vptr`
mixin, so those two are measured through a reference only (a `virtual_ptr`
would carry a pointer the object already holds). They also need their own
class hierarchy — an inplace class binds to exactly one registry — with their
own yardsticks and baselines. Every inplace ratio divides by its own
hierarchy's yardstick; cold the two hierarchies' yardsticks differ by around
10% — which is exactly why per-hierarchy yardsticks exist.

Two yardsticks, in both body flavors: `vf`, one virtual call, and `vf+vf`, the
double-dispatch idiom — two chained virtual calls. (The idiom is modeled, not
spelled out: one `dd_with` per leaf in the base does not scale
and would cost the same two dependent dispatches.)

Twenty-five leaf classes, and the count is a measured choice rather than an
inherited one: sweeping it over 10, 25, 50, 100 and 200 moves no cold ratio
outside the pass-to-pass noise (the arity-2 reference row sits at 2.19x-2.24x
across the whole range). Cold, a dispatch misses on one line per level of its
chain no matter how many classes exist. A hundred was needed when warm
measurements were published, to keep the indirect call unpredictable; it is
not needed now, and it cost g++ 15 more than half an hour a build.

Four baselines calibrate everything:

- **`probe`** — the timing machinery with nothing inside it. Its cost is the
  measurement apparatus; subtracting it from a row gives **`net`, the call's
  true cost**.
- **`direct`** — a plain (non-virtual, non-inlined) function call, quoted with
  the tables for scale. It is never subtracted from the headline.
- **`touch` / `touch+touch`** — a plain call that also loads the receiver(s).
  Subtracting it isolates dispatch from the cost of reaching the object
  (**`disp`**).

That comes to 60 variants per cache state, the two unpublished map registries
included: 26 const-body and 26 use-body dispatch-and-yardstick rows (per
world: 16 main-hierarchy dispatches, 2 main yardsticks, and 2 dispatches + 2
yardsticks on each inplace hierarchy), plus 8 body-neutral baselines. One cache state is published, cold (`clflush`).
Warm and a 64 MiB cache sweep exist in the binary as diagnostics; see "Why
cold only".

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

Worked example — clang 22, `om ref` against the `vf` yardstick:

|                          | `vf` | `om ref` | ratio                |
| ------------------------ | ---- | -------- | -------------------- |
| whole call (`net`)       | 544  | 1148     | **2.12x = `x net`**  |
| − the `touch` baseline   | −250 | −250     |                      |
| machinery alone (`disp`) | 286  | 881      | **3.04x = `x disp`** |

Same two rows, two honest ratios, answering different questions. `x net`
answers the caller's: *if I replace this virtual call with an open-method
call, how much slower is the call?* — twice as slow. `x disp` answers the
implementer's: *how much more work does the lookup machinery itself do?* —
three times as much. The second reads larger partly by construction:
subtracting the same 250 cycles from both sides of a division pushes the ratio
away from 1.

That subtraction is only symmetric when both rows reach the receiver, and one
kind of row does not. A `virtual_ptr` call with a const body never reads the
object, so its `disp` removes a plain call, ~5 cycles, where the yardstick's
removes a 250-cycle receiver miss. The two are not on the same footing and
their quotient means nothing, so the tables leave `x disp` blank for those
rows — the honest comparison there is `x net`, which subtracts the same empty
`probe` from both. In the use world every body reads its receiver, both sides
lose `touch`, and every `x disp` is real; that is what "Two fair comparisons"
is for. Neither is wrong; **`x net` is the headline in every
table, `x disp` the diagnostic.** Every published cell is its own median
over the passes — ratio cells are medians of the per-pass ratios, not
quotients of the cycle cells — so recomputed differences and quotients drift
a little: 544 − 250 reads 286 here, and 1148 / 544 is 2.11x where the table
below puts this row at 2.12x.

## Results

AMD Ryzen 9 9955HX (Zen 5), 32 MiB L3, WSL2, 25 classes, 4096 objects
shuffled in memory, 6000 reps per pass, median of 7 interleaved passes. This
section is the clang 22 column; the comparison across compilers follows.
Cycles are TSC reference cycles (~2.50 GHz here — the core clock is not
observable under the hypervisor). Rows read `om <form> / <registry>`:
`om vptr` is the `virtual_ptr` call form, `om ref` the `virtual_` reference
form.

### Caches cold (`clflush`)

Flushed, the first touch of the receiver is a cache miss in its own right: the
`touch` baseline nets 250 cycles, against 544 for the whole `vf`
yardstick. So 46% of a virtual call's `net` is reaching the object rather than
dispatching on it. `x net` — a row's total call cost divided by the yardstick's —
is the headline, and the most reproducible figure this benchmark produces:
misses dominate, and misses do not care about code layout. `disp` and
`x disp` are the mechanism-excess diagnostics — and `x disp` is blank for the `vptr`
rows, which never read the receiver: their `disp` removes a plain call where the
yardstick's removes a receiver miss, so the two are not a ratio. The `inplace` rows
divide by their own hierarchy's yardstick — 514 and 514 cycles here at
arity 1, 614 and 678 at arity 2 — not the yardstick rows shown.

clang 22, median of 7 passes.

For scale: a plain call to a stamping body measures 5 cycles net —
it touches no object, so there is nothing for the scrub to take away from it.

| dispatch                  | arity | net  | disp | x net | x disp |
| ------------------------- | ----- | ---- | ---- | ----- | ------ |
| `vf`                      | 1     | 544  | 286  | 1.00x | 1.00x  |
| `vptr`                    | 1     | 558  | 553  | 1.01x | —      |
| `vptr / indirect`         | 1     | 560  | 556  | 1.02x | —      |
| `om ref / vptr_vector`    | 1     | 1148 | 881  | 2.12x | 3.04x  |
| `om ref / indirect`       | 1     | 1307 | 1046 | 2.43x | 3.66x  |
| `om ref / inplace`        | 1     | 599  | 351  | 1.16x | 1.34x  |
| `om ref / inplace_ind`    | 1     | 814  | 555  | 1.58x | 2.06x  |
| `vf+vf (double dispatch)` | 2     | 652  | 335  | 1.00x | 1.00x  |
| `vptr`                    | 2     | 858  | 853  | 1.32x | —      |
| `vptr / indirect`         | 2     | 854  | 849  | 1.32x | —      |
| `om ref / vptr_vector`    | 2     | 1517 | 1209 | 2.34x | 3.65x  |
| `om ref / indirect`       | 2     | 1622 | 1317 | 2.49x | 3.91x  |
| `om ref / inplace`        | 2     | 865  | 557  | 1.41x | 1.87x  |
| `om ref / inplace_ind`    | 2     | 1140 | 827  | 1.67x | 2.20x  |

### Reading it

Ratios below are `x net` unless marked `disp`.

- **A `virtual_ptr` call costs what a virtual function call costs**: 1.01x.
  Not "no more than" — the same. Structurally it has to be: both are two
  dependent loads, the virtual call reading the receiver to reach its v-table
  and the `virtual_ptr` call reading the slot to index one it already has.
- **`virtual_` reference dispatch costs about 2x a virtual call cold — the
  figure that holds across every compiler** (2.12x / 1.99x / 2.08x), and
  above the 30-50% band the library's documentation cites, whose numbers date
  from a different harness on different hardware. The excess is the
  hash-and-look-up: two more dependent loads than the `virtual_ptr` form,
  visible in the `disp` column.
- **`inplace_vptr` is the closest reference dispatch to a virtual function**
  (1.16x): its layout *is* the virtual function's layout.
- **At two virtual arguments the idiom wins**: 1.32x net for the
  `virtual_ptr` form and 2.34x for the reference form, because the
  two-dimensional dispatch data spans more cache lines than the idiom's two
  v-table chains — which are two dependent calls, but only two lines.
- **`indirect_vptr` puts one more dependent load on the path**; the section
  below itemizes what it costs.

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

This table is the clang 22 column, like "Results" above; the use world is not
re-measured across the compilers.

#### Cold (`clflush`), receiver used

The member reads execute inside the timed window — every use body loads its
receiver(s) before the arrival stamp (see "Timing"). Ratios divide by this
table's own yardsticks, which pay the same reads.

clang 22, median of 7 passes.

| dispatch                  | arity | net  | disp | x net | x disp |
| ------------------------- | ----- | ---- | ---- | ----- | ------ |
| `vf`                      | 1     | 545  | 295  | 1.00x | 1.00x  |
| `vptr`                    | 1     | 588  | 331  | 1.04x | 1.08x  |
| `vptr / indirect`         | 1     | 574  | 331  | 1.06x | 1.11x  |
| `om ref / vptr_vector`    | 1     | 1098 | 836  | 2.06x | 2.97x  |
| `om ref / indirect`       | 1     | 1327 | 1052 | 2.56x | 3.73x  |
| `om ref / inplace`        | 1     | 614  | 361  | 1.14x | 1.26x  |
| `om ref / inplace_ind`    | 1     | 808  | 565  | 1.48x | 1.87x  |
| `vf+vf (double dispatch)` | 2     | 638  | 319  | 1.00x | 1.00x  |
| `vptr`                    | 2     | 854  | 537  | 1.35x | 1.69x  |
| `vptr / indirect`         | 2     | 865  | 548  | 1.30x | 1.60x  |
| `om ref / vptr_vector`    | 2     | 1379 | 1056 | 2.23x | 3.32x  |
| `om ref / indirect`       | 2     | 1630 | 1317 | 2.61x | 4.23x  |
| `om ref / inplace`        | 2     | 877  | 572  | 1.37x | 1.69x  |
| `om ref / inplace_ind`    | 2     | 1136 | 828  | 1.64x | 2.15x  |

### What the use world shows

- **Parity survives the receiver read**: `virtual_ptr` is 1.04x net and 1.08x
  disp of the virtual function, with the miss now paid by both sides and
  subtracted from both sides.
- **The deferred receiver miss is mostly hidden, not free.** Making the body
  read the object costs `virtual_ptr` 30 cycles — 558 net to 588 — against
  250 for a lone receiver miss, so about seven-eighths of it disappears under
  the table misses; the object address is in the fat pointer from the first
  cycle, so the load can start immediately. The virtual function pays nothing
  at all for the same read (544 to 545): its dispatch had already fetched
  that line.
- **That prefetch is worth about 30 cycles of a 550-cycle call**, and only
  when the body reads the receiver at all. Delivery world: `virtual_ptr`
  matches without loading the object. Use world: it matches to within 4%
  while hiding most of the load. No workload here makes keeping the v-table
  pointer inside the object pay for itself.

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

Median of 7 passes; `disp` cycles, direct → indirect, caches cold.

| call form      | arity | clang 22               | gcc 15                 | gcc 13                 |
| -------------- | ----- | ---------------------- | ---------------------- | ---------------------- |
| `virtual_ptr`  | 1     | 553 → 556 (**+3**)     | 552 → 615 (**+63**)    | 525 → 610 (**+85**)    |
| `virtual_ptr`  | 2     | 853 → 849 (**-4**)     | 830 → 876 (**+46**)    | 871 → 874 (**+3**)     |
| `virtual_` ref | 1     | 881 → 1046 (**+165**)  | 879 → 1113 (**+234**)  | 867 → 1156 (**+289**)  |
| `virtual_` ref | 2     | 1209 → 1317 (**+108**) | 1193 → 1349 (**+156**) | 1163 → 1353 (**+190**) |
| `inplace` ref  | 1     | 351 → 555 (**+204**)   | 341 → 536 (**+195**)   | 356 → 544 (**+188**)   |
| `inplace` ref  | 2     | 557 → 827 (**+270**)   | 616 → 852 (**+236**)   | 593 → 859 (**+266**)   |

### Reading the cost

- **Through a reference the added load lands at the end of an already-long
  dependency chain** (object → `type_info` → hash → vptr vector →
  **indirection** → v-table) with nothing to overlap against, and it pays a
  full miss: the indirection target is a line of its own.
- **Through a `virtual_ptr` it is small and inconsistently sized**, because
  the independent slot load gives the miss something to hide under — the
  chain is short there and has slack.
- **On an `inplace_vptr` hierarchy it gives back the tie with the virtual
  function that `inplace_vptr` had won**: no slack there either.

If the program needs `initialize()` to be callable more than once, the price
is a load; if it does not, `indirect_vptr` is pure cost.

## Compilers

Three builds — clang++ 22.1, g++ 15.2 and g++ 13.3 — measured by
`./matrix.sh`, which reads the list from `compilers.conf`; adding a compiler
or a version is adding a line there. Cells are `x net (net cycles)`: each
row's total call cost divided by the *same build's* yardstick, which is what
makes columns comparable. As everywhere, the `inplace` rows divide by their
own hierarchy's yardstick, not the `vf (yardstick)` rows shown.

The three are not the same vintage, and that is the point of the axis. clang
22 and g++ 15 are current; g++ 13.3 is what Ubuntu 24.04 still installs as
`g++`, released in 2023 and two major versions behind — and it is what a great
many people will actually compile this library with. The dispatch is the same
handful of instructions either way (see "Every timed region"), so this axis
asks what the compiler around it costs: how well it keeps a fat pointer in
registers, where it puts the loads it cannot avoid, what it does with the
timing rig. The single-column sections above are the first column, so the
headline numbers are a current compiler's.

#### The compilers

| dispatch                | clang 22     | gcc 15       | gcc 13       |
| ----------------------- | ------------ | ------------ | ------------ |
| **1 virtual argument**  |              |              |              |
| `vf (yardstick)`        | 1.00x (544)  | 1.00x (570)  | 1.00x (551)  |
| `vptr`                  | 1.01x (558)  | 1.04x (555)  | 0.98x (528)  |
| `vptr / indirect`       | 1.02x (560)  | 1.10x (619)  | 1.11x (614)  |
| `om ref / vptr_vector`  | 2.12x (1148) | 1.99x (1140) | 2.08x (1136) |
| `om ref / indirect`     | 2.43x (1307) | 2.54x (1370) | 2.54x (1426) |
| `om ref / inplace`      | 1.16x (599)  | 1.11x (586)  | 1.20x (607)  |
| `om ref / inplace_ind`  | 1.58x (814)  | 1.51x (790)  | 1.61x (788)  |
| **2 virtual arguments** |              |              |              |
| `vf+vf (yardstick)`     | 1.00x (652)  | 1.00x (756)  | 1.00x (675)  |
| `vptr`                  | 1.32x (858)  | 1.10x (834)  | 1.31x (875)  |
| `vptr / indirect`       | 1.32x (854)  | 1.14x (879)  | 1.35x (878)  |
| `om ref / vptr_vector`  | 2.34x (1517) | 2.00x (1496) | 2.19x (1490) |
| `om ref / indirect`     | 2.49x (1622) | 2.19x (1658) | 2.54x (1674) |
| `om ref / inplace`      | 1.41x (865)  | 1.34x (896)  | 1.42x (891)  |
| `om ref / inplace_ind`  | 1.67x (1140) | 1.84x (1129) | 1.95x (1150) |

Median of 7 passes. Spread across passes: median 17%, p90 26%.

### What it shows

- **The compilers agree on the mechanisms.** Cold, reference dispatch costs
  1.99x-2.12x the virtual function's total call cost on every compiler.
- **`inplace` is the fastest reference dispatch in every column** — cold,
  1.11x-1.20x net at arity 1 (1.34x-1.42x at arity 2) across the three.

## Reproducibility

- Every published cell is the **median of 7 passes**; `matrix.sh` interleaves
  the whole matrix per pass so drift lands on all columns alike, and clears
  stale run directories first (the results are committed, so stale runs are
  the norm, not an accident).
- Across passes, a single variant's `net` moves by a median of **14% (p90
  22%, worst 30%)** on this machine — an un-isolatable WSL2 guest. (The matrix captions quote a slightly larger
  spread — that one is measured on the *ratios*, so it also absorbs the
  yardstick's own motion.) Quote medians, not passes.
- **The built-in control**: the three direct registries' `om vptr` rows must
  agree — the vptr policy is not on that call path, and they compile to the
  same two instructions. The tables print one `vptr` row, so `report.py`'s
  `control()` is where the other two are read; it warns when they diverge
  beyond the noise floor. They currently agree to 2-6% per column, against a
  14% pass-to-pass spread. `indirect` is excluded by design — its extra load is
  the point.
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

**The start** is `asm volatile("lfence; rdtsc; lfence" ::: "memory")`, and
each of the four parts earns its place. The leading `lfence` retires
everything the previous iteration left in flight, so the window does not open
on somebody else's work. `rdtsc` reads the counter. The trailing `lfence`
stops the dispatch from issuing before that read has, which is the whole point
of bracketing it. And the `"memory"` clobber is the one that is easy to
forget: `lfence` orders the *processor*, not the *compiler*, and without the
clobber the optimizer hoists the loop-invariant loads — the hash factors, the
table base, the method slot — out of the timed region entirely, leaving a
window that measures a call whose operands were already in registers.

**The stop** is `rdtscp`, and it waits until every prior instruction — the
whole dispatch chain, and in the use world the receiver load kept above it by
its memory clobber — has executed, so the stamp is the moment control
*arrived* in the right overrider. Everything after it is outside the window by
construction: assembling the stamp, writing the id, the entire return path,
and the elapsed computation, which is a data dependency on the returned value.
There is no closing bracket, so nothing is compiler-scheduled at the stop
edge.

- The start bracket lives inside a `noinline` `timed_call` whose parameters
  are the call arguments: the prologue is untimed while the arguments still
  arrive through the ABI and cannot be constant-folded or devirtualized.
- The start timestamp is captured as the raw `edx:eax` pair; the compiler
  schedules the two or three ALU ops that assemble it as it pleases — inside
  the window for some variants, partly or wholly after the stop for others
  (`probe` carries none in-window). The asymmetry is bounded by a couple of
  register-ALU ops, sub-cycle on this core.
- **Every variant replays the identical receiver sequence** (the RNG is
  reseeded per variant): `disp` is a difference of two measurements, and cold
  both are dominated by which objects were drawn — unpaired draws leave that
  term uncancelled.
- Each rep performs an untimed warm-up call, then scrubs, then measures once.
  The warm-up is what makes the scrub meaningful: it puts the lines *in* the
  caches so that flushing them evicts something.
- `lfence` is dispatch-serializing on Zen, so no `CPUID` — which traps to the
  hypervisor here and would cost more than the thing measured.
- Every compiler gets `-fcf-protection=none`; their defaults differ (gcc
  `=full`) in ways that put an `endbr64` on every indirect-call target of one
  build and not the other ([HISTORY.md](HISTORY.md)).

### Why cold only

Only `clflush` is published. `warm` is still in the binary, and it resolves
differences of a cycle or two that cold buries under a hundred — but warm a
virtual call is mostly a *branch misprediction*, not a load. The receivers are
drawn in shuffled order from a hundred classes, so the indirect call has a
hundred unpredictable targets and misses most of the time: warm, a plain call
measures around 4 cycles and a virtual call around 9, so more than half of the
yardstick is the mispredict. How well the predictor does with a given indirect
branch depends on where that branch happens to sit, so recompiling moves it
for reasons that have nothing to do with dispatch — and every ratio built on
that denominator moves with it. Cold, misses dominate and misses do not care
where the code landed, which is why the cold ratios reproduce across
compilers and the warm ones did not. A side experiment with a single leaf
class (perfectly predicted) put the predicted virtual call at a few cycles and
the mispredicted one severalfold higher; the data is archived in
[HISTORY.md](HISTORY.md).

### Cache states

| mode      | what it does                                                                                                                                                                                                                                                                                       |
| --------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `warm`    | nothing, after the warm-up call; diagnostic only, not in the published dataset — see "Why cold only"                                                                                                                                                                                               |
| `clflush` | `clflushopt` over the receiver(s), the C++ v-table from its head (vptr−16, so the `type_info` slot at vptr−8 is always evicted), the method's `fn` object, the registry's dispatch-table arena, the vptr storage, and — for indirect registries — the per-class `static_vptr` cells; then `mfence` |
| `sweep`   | one store per line over a 64 MiB buffer (2x L3); diagnostic only, not in the published dataset                                                                                                                                                                                                     |

### Statistics

The reported statistic is the **mean of each variant's samples, trimmed of the
top 5%** (where preemptions land), with the standard error of the difference
in the console's `+/-` column. The mean, not the median: an lfence-bracketed
counter read is quantized, costing 25 or 50 cycles bimodally on this machine,
so medians land on a tick while the trimmed mean resolves between them. Cold,
what dominates instead is which lines the draw happened to need, which is why
several passes are taken and the median across *passes* is what gets
published. TSC frequency is calibrated against `CLOCK_MONOTONIC` at startup;
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
- **`net` and `disp` differ by the plain-call cost by construction**: `net`
  subtracts the empty `probe`, `disp` subtracts a real call. Neither is wrong;
  they answer the two questions in "What the ratios divide".
- Cycles are TSC *reference* cycles at ~2.50 GHz, not core cycles; the core
  clock is invisible under WSL2/Hyper-V. `run.sh` requests `SCHED_FIFO` and
  falls back to normal priority without it.
- The harness reaches into `boost::openmethod::detail` for the dispatch-table
  arena — deliberate: `clflush` must evict exactly what the dispatch reads.

## Generated code

Timed regions, gcc 13 `-O2`, current binaries. Each region ends at the `call`
into a body whose first instruction is `rdtscp`; the `mov`/`shl`/`or` around
the dispatch is the start stamp's bookkeeping, which the compiler schedules
freely (here inside the window; in other variants partly after the call) — a
sub-cycle asymmetry, per "Timing".

These three are annotated by hand, and only the gcc column: they are the
teaching version. "Every timed region, side by side" below prints
the same windows for every dispatch and every compiler, straight from the
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

## Every timed region, side by side

The listings above are hand-picked. `asmtab.py` takes the same window from
every binary and every dispatch: the instructions `timed_call<V>` executes
between the start bracket and the `call` into the body. Each variant is its
own `noinline` instantiation, so the window is a contiguous range inside a
symbol the script finds by name — nothing here is transcribed by hand, and
`python3 asmtab.py` regenerates this section from whatever is in `bin/`.

Operands that read a global are rewritten to the role the value plays —
`[rip+slot]`, `[rip+mult]` — inferred from what the value is *used for* rather
than from its symbol offset. The map registries are left out; their probe
inlines a container walk rather than the arithmetic the others do.

**What is shown is the dispatch, sliced out of the window.** Everything below
is the code that ran between the brackets, minus the instructions the timing
rig leaves there — identified by a backward slice from the call, so nothing
that reaches the call or its arguments can be dropped. What the slice removes
is the start stamp: `rdtsc` leaves the counter split across `edx:eax`, and
each compiler stitches the halves into something the epilogue can subtract,
gcc shifting and or-ing, clang negating and subtracting. It is scheduled
freely and lands anywhere in the window (per "Timing"), and left in it
outnumbers the dispatch — a virtual call is two instructions under five of
harness. Because a slice is a heuristic and compilers move things, `probes.sh`
compiles the same dispatches standalone and `asmtab.py` fails the run if any
instruction of the standalone dispatch is missing from the slice.

### What the windows contain

Dispatch instructions, then the **dependent loads** — how many loads have to
complete one after another before the call has its target, counting the call's
own read of the v-table entry. (The listings earlier in this document count
that one separately, as "one dependent load, one indirect call"; the same `vf`
window is 1 + 1 there and 2 here.) The second number is the one that predicts
cost.

One row cannot be read like the others. The double-dispatch idiom's second
dispatch happens *inside* the first body — `Derived<N>::dd` calls
`other.dd_with(*this)` — so only its first call is in the window. Its two
dependent loads are half of what the `vf+vf` yardstick is timing; the rest is
compiled into the body.

| dispatch               | arity | clang 22 | gcc 15 | gcc 13 |
| ---------------------- | ----- | -------- | ------ | ------ |
| `vf (yardstick)`       | 1     | 2 / 2    | 2 / 2  | 2 / 2  |
| `vptr`                 | 1     | 2 / 2    | 2 / 2  | 3 / 2  |
| `om ref / vptr_vector` | 1     | 9 / 4    | 9 / 4  | 9 / 4  |
| `vptr / indirect`      | 1     | 3 / 2    | 3 / 2  | 4 / 3  |
| `om ref / indirect`    | 1     | 10 / 5   | 10 / 5 | 10 / 5 |
| `om ref / inplace`     | 1     | 3 / 2    | 3 / 2  | 3 / 2  |
| `om ref / inplace_ind` | 1     | 4 / 3    | 4 / 3  | 4 / 3  |
| `vf+vf (yardstick)`    | 2     | 2 / 2    | 2 / 2  | 2 / 2  |
| `vptr`                 | 2     | 8 / 3    | 8 / 3  | 8 / 3  |
| `om ref / vptr_vector` | 2     | 18 / 5   | 18 / 5 | 18 / 5 |
| `vptr / indirect`      | 2     | 10 / 4   | 10 / 4 | 10 / 4 |
| `om ref / indirect`    | 2     | 20 / 6   | 20 / 6 | 20 / 6 |
| `om ref / inplace`     | 2     | 8 / 3    | 8 / 3  | 8 / 3  |
| `om ref / inplace_ind` | 2     | 10 / 4   | 10 / 4 | 10 / 4 |

### The listings

`vf (yardstick)`, arity 1:

```asm
clang 22                   gcc 15                     gcc 13
-------------------------  -------------------------  -------------------------
mov rax, QWORD PTR [rdi]   mov rax, QWORD PTR [rdi]   mov rax, QWORD PTR [rdi]
call QWORD PTR [rax+0x10]  call QWORD PTR [rax+0x10]  call QWORD PTR [rax+0x10]
```

`vptr`, arity 1:

```asm
clang 22                       gcc 15                         gcc 13
-----------------------------  -----------------------------  -----------------------------
mov rax, QWORD PTR [rip+slot]  mov rax, QWORD PTR [rip+slot]  mov rcx, QWORD PTR [rsp]
call QWORD PTR [rdi+rax*8]     call QWORD PTR [rdi+rax*8]     mov rax, QWORD PTR [rip+slot]
                                                              call QWORD PTR [rcx+rax*8]
```

`om ref / vptr_vector`, arity 1:

```asm
clang 22                         gcc 15                           gcc 13
-------------------------------  -------------------------------  -------------------------------
mov rsi, QWORD PTR [rdi]         mov rax, QWORD PTR [rdi]         mov rax, QWORD PTR [rdi]
mov rcx, QWORD PTR [rip+mult]    mov rdx, QWORD PTR [rax-0x8]     mov rdx, QWORD PTR [rax-0x8]
movzx eax, BYTE PTR [rip+shift]  mov rax, QWORD PTR [rip+shift]   mov rax, QWORD PTR [rip+shift]
imul rcx, QWORD PTR [rsi-0x8]    imul rdx, QWORD PTR [rip+mult]   imul rdx, QWORD PTR [rip+mult]
mov rsi, QWORD PTR [rip+vptrs]   shrx rdx, rdx, rax               shrx rdx, rdx, rax
shrx rax, rcx, rax               mov rax, QWORD PTR [rip+vptrs]   mov rax, QWORD PTR [rip+vptrs]
mov rcx, QWORD PTR [rip+slot]    mov rax, QWORD PTR [rax+rdx*8]   mov rax, QWORD PTR [rax+rdx*8]
mov rax, QWORD PTR [rsi+rax*8]   mov rdx, QWORD PTR [rip+slot]    mov rdx, QWORD PTR [rip+slot]
call QWORD PTR [rax+rcx*8]       call QWORD PTR [rax+rdx*8]       call QWORD PTR [rax+rdx*8]
```

`vptr / indirect`, arity 1:

```asm
clang 22                       gcc 15                         gcc 13
-----------------------------  -----------------------------  -----------------------------
mov rcx, QWORD PTR [rip+slot]  mov rax, QWORD PTR [rdi]       mov rcx, QWORD PTR [rsp]
mov rax, QWORD PTR [rdi]       mov rdx, QWORD PTR [rip+slot]  mov rdx, QWORD PTR [rip+slot]
call QWORD PTR [rax+rcx*8]     call QWORD PTR [rax+rdx*8]     mov rax, QWORD PTR [rcx]
                                                              call QWORD PTR [rax+rdx*8]
```

`om ref / indirect`, arity 1:

```asm
clang 22                         gcc 15                           gcc 13
-------------------------------  -------------------------------  -------------------------------
mov rsi, QWORD PTR [rdi]         mov rax, QWORD PTR [rdi]         mov rax, QWORD PTR [rdi]
mov rcx, QWORD PTR [rip+mult]    mov rdx, QWORD PTR [rax-0x8]     mov rdx, QWORD PTR [rax-0x8]
movzx eax, BYTE PTR [rip+shift]  mov rax, QWORD PTR [rip+shift]   mov rax, QWORD PTR [rip+shift]
imul rcx, QWORD PTR [rsi-0x8]    imul rdx, QWORD PTR [rip+mult]   imul rdx, QWORD PTR [rip+mult]
mov rsi, QWORD PTR [rip+vptrs]   shrx rdx, rdx, rax               shrx rdx, rdx, rax
shrx rax, rcx, rax               mov rax, QWORD PTR [rip+vptrs]   mov rax, QWORD PTR [rip+vptrs]
mov rcx, QWORD PTR [rip+slot]    mov rax, QWORD PTR [rax+rdx*8]   mov rax, QWORD PTR [rax+rdx*8]
mov rax, QWORD PTR [rsi+rax*8]   mov rdx, QWORD PTR [rip+slot]    mov rdx, QWORD PTR [rip+slot]
mov rax, QWORD PTR [rax]         mov rax, QWORD PTR [rax]         mov rax, QWORD PTR [rax]
call QWORD PTR [rax+rcx*8]       call QWORD PTR [rax+rdx*8]       call QWORD PTR [rax+rdx*8]
```

`om ref / inplace`, arity 1:

```asm
clang 22                       gcc 15                         gcc 13
-----------------------------  -----------------------------  -----------------------------
mov rcx, QWORD PTR [rip+slot]  mov rax, QWORD PTR [rdi+0x8]   mov rax, QWORD PTR [rdi+0x8]
mov rax, QWORD PTR [rdi+0x8]   mov rdx, QWORD PTR [rip+slot]  mov rdx, QWORD PTR [rip+slot]
call QWORD PTR [rax+rcx*8]     call QWORD PTR [rax+rdx*8]     call QWORD PTR [rax+rdx*8]
```

`om ref / inplace_ind`, arity 1:

```asm
clang 22                       gcc 15                         gcc 13
-----------------------------  -----------------------------  -----------------------------
mov rcx, QWORD PTR [rdi+0x8]   mov rax, QWORD PTR [rdi+0x8]   mov rax, QWORD PTR [rdi+0x8]
mov rax, QWORD PTR [rcx]       mov rdx, QWORD PTR [rip+slot]  mov rdx, QWORD PTR [rip+slot]
mov rcx, QWORD PTR [rip+slot]  mov rax, QWORD PTR [rax]       mov rax, QWORD PTR [rax]
call QWORD PTR [rax+rcx*8]     call QWORD PTR [rax+rdx*8]     call QWORD PTR [rax+rdx*8]
```

`vf+vf (yardstick)`, arity 2:

```asm
clang 22                   gcc 15                     gcc 13
-------------------------  -------------------------  -------------------------
mov rax, QWORD PTR [rdi]   mov rax, QWORD PTR [rdi]   mov rax, QWORD PTR [rdi]
call QWORD PTR [rax+0x20]  call QWORD PTR [rax+0x20]  call QWORD PTR [rax+0x20]
```

`vptr`, arity 2:

```asm
clang 22                          gcc 15                            gcc 13
--------------------------------  --------------------------------  --------------------------------
mov rdx, QWORD PTR [rsp+0x30]     mov rax, QWORD PTR [rsp+0x30]     mov rcx, QWORD PTR [rip+slot]
mov r8, QWORD PTR [rip+slot]      mov r8, QWORD PTR [rsp+0x20]      mov rax, QWORD PTR [rsp+0x30]
mov rdi, QWORD PTR [rsp+0x20]     mov rdi, QWORD PTR [rip+slot]     mov rdi, QWORD PTR [rsp+0x20]
mov rax, QWORD PTR [rip+slot]     mov rax, QWORD PTR [rax+rdi*8]    mov rax, QWORD PTR [rax+rcx*8]
mov r8, QWORD PTR [rdx+r8*8]      mov rdi, QWORD PTR [rip+slot]     mov rcx, QWORD PTR [rip+slot]
imul r8, QWORD PTR [rip+stride]   imul rax, QWORD PTR [rip+stride]  imul rax, QWORD PTR [rip+stride]
mov rax, QWORD PTR [rdi+rax*8]    mov r8, QWORD PTR [r8+rdi*8]      mov r8, QWORD PTR [rdi+rcx*8]
call QWORD PTR [rax+r8*8]         call QWORD PTR [r8+rax*8]         call QWORD PTR [r8+rax*8]
```

`om ref / vptr_vector`, arity 2:

```asm
clang 22                          gcc 15                            gcc 13
--------------------------------  --------------------------------  --------------------------------
mov rdx, QWORD PTR [rdi]          mov rax, QWORD PTR [rdi]          mov rax, QWORD PTR [rdi]
mov rcx, QWORD PTR [rip+mult]     mov rcx, QWORD PTR [rip+mult]     mov rcx, QWORD PTR [rip+mult]
mov r10, QWORD PTR [rsi]          mov r8, QWORD PTR [rip+shift]     mov r8, QWORD PTR [rip+shift]
mov r8, QWORD PTR [rip+vptrs]     mov rdx, QWORD PTR [rip+vptrs]    mov rdx, QWORD PTR [rip+vptrs]
mov r9, QWORD PTR [rip+slot]      mov r10, QWORD PTR [rax-0x8]      mov r10, QWORD PTR [rax-0x8]
mov rax, QWORD PTR [rdx-0x8]      imul r10, rcx                     imul r10, rcx
movzx edx, BYTE PTR [rip+shift]   shrx rax, r10, r8                 shrx rax, r10, r8
imul rax, rcx                     mov r9, QWORD PTR [rdx+rax*8]     mov r9, QWORD PTR [rdx+rax*8]
imul rcx, QWORD PTR [r10-0x8]     mov rax, QWORD PTR [rsi]          mov rax, QWORD PTR [rsi]
shrx rax, rax, rdx                imul rcx, QWORD PTR [rax-0x8]     imul rcx, QWORD PTR [rax-0x8]
mov rax, QWORD PTR [r8+rax*8]     shrx rcx, rcx, r8                 shrx rcx, rcx, r8
shrx rcx, rcx, rdx                mov rax, QWORD PTR [rdx+rcx*8]    mov rax, QWORD PTR [rdx+rcx*8]
mov rdx, QWORD PTR [rip+slot]     mov rdx, QWORD PTR [rip+slot]     mov rdx, QWORD PTR [rip+slot]
mov rcx, QWORD PTR [r8+rcx*8]     mov rax, QWORD PTR [rax+rdx*8]    mov rax, QWORD PTR [rax+rdx*8]
mov rax, QWORD PTR [rax+r9*8]     mov rdx, QWORD PTR [rip+slot]     mov rdx, QWORD PTR [rip+slot]
mov rcx, QWORD PTR [rcx+rdx*8]    imul rax, QWORD PTR [rip+stride]  imul rax, QWORD PTR [rip+stride]
imul rcx, QWORD PTR [rip+stride]  mov rdx, QWORD PTR [r9+rdx*8]     mov rdx, QWORD PTR [r9+rdx*8]
call QWORD PTR [rax+rcx*8]        call QWORD PTR [rdx+rax*8]        call QWORD PTR [rdx+rax*8]
```

`vptr / indirect`, arity 2:

```asm
clang 22                          gcc 15                            gcc 13
--------------------------------  --------------------------------  --------------------------------
mov rdi, QWORD PTR [rsp+0x20]     mov r8, QWORD PTR [rsp+0x20]      mov rax, QWORD PTR [rsp+0x30]
mov rdx, QWORD PTR [rsp+0x30]     mov rax, QWORD PTR [rsp+0x30]     mov rdi, QWORD PTR [rsp+0x20]
mov r8, QWORD PTR [rip+slot]      mov rax, QWORD PTR [rax]          mov rcx, QWORD PTR [rip+slot]
mov r9, QWORD PTR [rip+slot]      mov rdi, QWORD PTR [r8]           mov rax, QWORD PTR [rax]
mov rax, QWORD PTR [rdi]          mov r8, QWORD PTR [rip+slot]      mov rdx, QWORD PTR [rdi]
mov rax, QWORD PTR [rax+r8*8]     mov rax, QWORD PTR [rax+r8*8]     mov rax, QWORD PTR [rax+rcx*8]
mov r8, QWORD PTR [rdx]           mov r8, QWORD PTR [rip+slot]      mov rcx, QWORD PTR [rip+slot]
mov r8, QWORD PTR [r8+r9*8]       imul rax, QWORD PTR [rip+stride]  imul rax, QWORD PTR [rip+stride]
imul r8, QWORD PTR [rip+stride]   mov r8, QWORD PTR [rdi+r8*8]      mov r8, QWORD PTR [rdx+rcx*8]
call QWORD PTR [rax+r8*8]         call QWORD PTR [r8+rax*8]         call QWORD PTR [r8+rax*8]
```

`om ref / indirect`, arity 2:

```asm
clang 22                          gcc 15                            gcc 13
--------------------------------  --------------------------------  --------------------------------
mov rdx, QWORD PTR [rdi]          mov rax, QWORD PTR [rdi]          mov rax, QWORD PTR [rdi]
mov rcx, QWORD PTR [rip+mult]     mov rcx, QWORD PTR [rip+mult]     mov rcx, QWORD PTR [rip+mult]
mov r10, QWORD PTR [rsi]          mov r8, QWORD PTR [rip+shift]     mov r8, QWORD PTR [rip+shift]
mov r8, QWORD PTR [rip+vptrs]     mov rdx, QWORD PTR [rip+vptrs]    mov rdx, QWORD PTR [rip+vptrs]
mov r9, QWORD PTR [rip+slot]      mov r10, QWORD PTR [rax-0x8]      mov r10, QWORD PTR [rax-0x8]
mov rax, QWORD PTR [rdx-0x8]      imul r10, rcx                     imul r10, rcx
movzx edx, BYTE PTR [rip+shift]   shrx rax, r10, r8                 shrx rax, r10, r8
imul rax, rcx                     mov rax, QWORD PTR [rdx+rax*8]    mov rax, QWORD PTR [rdx+rax*8]
imul rcx, QWORD PTR [r10-0x8]     mov r9, QWORD PTR [rax]           mov r9, QWORD PTR [rax]
shrx rax, rax, rdx                mov rax, QWORD PTR [rsi]          mov rax, QWORD PTR [rsi]
mov rax, QWORD PTR [r8+rax*8]     imul rcx, QWORD PTR [rax-0x8]     imul rcx, QWORD PTR [rax-0x8]
shrx rcx, rcx, rdx                shrx rcx, rcx, r8                 shrx rcx, rcx, r8
mov rdx, QWORD PTR [rip+slot]     mov rax, QWORD PTR [rdx+rcx*8]    mov rax, QWORD PTR [rdx+rcx*8]
mov rcx, QWORD PTR [r8+rcx*8]     mov rdx, QWORD PTR [rip+slot]     mov rdx, QWORD PTR [rip+slot]
mov rax, QWORD PTR [rax]          mov rax, QWORD PTR [rax]          mov rax, QWORD PTR [rax]
mov rcx, QWORD PTR [rcx]          mov rax, QWORD PTR [rax+rdx*8]    mov rax, QWORD PTR [rax+rdx*8]
mov rax, QWORD PTR [rax+r9*8]     mov rdx, QWORD PTR [rip+slot]     mov rdx, QWORD PTR [rip+slot]
mov rcx, QWORD PTR [rcx+rdx*8]    imul rax, QWORD PTR [rip+stride]  imul rax, QWORD PTR [rip+stride]
imul rcx, QWORD PTR [rip+stride]  mov rdx, QWORD PTR [r9+rdx*8]     mov rdx, QWORD PTR [r9+rdx*8]
call QWORD PTR [rax+rcx*8]        call QWORD PTR [rdx+rax*8]        call QWORD PTR [rdx+rax*8]
```

`om ref / inplace`, arity 2:

```asm
clang 22                          gcc 15                            gcc 13
--------------------------------  --------------------------------  --------------------------------
mov rdx, QWORD PTR [rdi+0x8]      mov rcx, QWORD PTR [rip+slot]     mov rcx, QWORD PTR [rip+slot]
mov rcx, QWORD PTR [rip+slot]     mov rax, QWORD PTR [rsi+0x8]      mov rax, QWORD PTR [rsi+0x8]
mov rax, QWORD PTR [rdx+rcx*8]    mov rdx, QWORD PTR [rdi+0x8]      mov rdx, QWORD PTR [rdi+0x8]
mov rcx, QWORD PTR [rsi+0x8]      mov rax, QWORD PTR [rax+rcx*8]    mov rax, QWORD PTR [rax+rcx*8]
mov rdx, QWORD PTR [rip+slot]     mov rcx, QWORD PTR [rip+slot]     mov rcx, QWORD PTR [rip+slot]
mov rcx, QWORD PTR [rcx+rdx*8]    imul rax, QWORD PTR [rip+stride]  imul rax, QWORD PTR [rip+stride]
imul rcx, QWORD PTR [rip+stride]  mov rdx, QWORD PTR [rdx+rcx*8]    mov rdx, QWORD PTR [rdx+rcx*8]
call QWORD PTR [rax+rcx*8]        call QWORD PTR [rdx+rax*8]        call QWORD PTR [rdx+rax*8]
```

`om ref / inplace_ind`, arity 2:

```asm
clang 22                          gcc 15                            gcc 13
--------------------------------  --------------------------------  --------------------------------
mov rcx, QWORD PTR [rdi+0x8]      mov rax, QWORD PTR [rdi+0x8]      mov rax, QWORD PTR [rdi+0x8]
mov rdx, QWORD PTR [rip+slot]     mov rcx, QWORD PTR [rip+slot]     mov rcx, QWORD PTR [rip+slot]
mov rax, QWORD PTR [rcx]          mov rdx, QWORD PTR [rax]          mov rdx, QWORD PTR [rax]
mov rcx, QWORD PTR [rip+slot]     mov rax, QWORD PTR [rsi+0x8]      mov rax, QWORD PTR [rsi+0x8]
mov rax, QWORD PTR [rax+rcx*8]    mov rax, QWORD PTR [rax]          mov rax, QWORD PTR [rax]
mov rcx, QWORD PTR [rsi+0x8]      mov rax, QWORD PTR [rax+rcx*8]    mov rax, QWORD PTR [rax+rcx*8]
mov rcx, QWORD PTR [rcx]          mov rcx, QWORD PTR [rip+slot]     mov rcx, QWORD PTR [rip+slot]
mov rcx, QWORD PTR [rcx+rdx*8]    imul rax, QWORD PTR [rip+stride]  imul rax, QWORD PTR [rip+stride]
imul rcx, QWORD PTR [rip+stride]  mov rdx, QWORD PTR [rdx+rcx*8]    mov rdx, QWORD PTR [rdx+rcx*8]
call QWORD PTR [rax+rcx*8]        call QWORD PTR [rdx+rax*8]        call QWORD PTR [rdx+rax*8]
```

### What the windows show

- **The reference chain is the same depth on every build**: 4 dependent loads
  at arity 1 — receiver, `type_info`, vptr vector, open-method v-table — and 5
  through `indirect`. What differs between the columns is register allocation
  and where the stamp bookkeeping lands, not the chain. That is the structural
  reason the ratios agree across builds.
- **A `virtual_ptr` call is 2 dependent loads, exactly what a virtual function
  call is** (2) — but not the same 2. The virtual call must read the receiver
  before it can read its v-table; the `virtual_ptr` call already has the
  v-table and spends its chain on the slot and the v-table entry the call
  reads through it. Nothing has to find the receiver first, which is why the
  two come out level.
- **Where the compilers differ is what they do with the fat pointer**: clang
  22 takes 2, gcc 15 takes 2, gcc 13 takes 3 instructions to dispatch through
  it. That costs nothing at arity 1 where the reload runs beside the slot
  load, but it is what makes the next bullet uneven.
- **`indirect_vptr` adds exactly one dependent load — where there is no slack
  to hide it.** Through a reference it deepens the chain on every build (4 →
  5), and on the inplace hierarchy too (2 → 3). Through a `virtual_ptr` at
  arity 1 it deepens gcc 13, which reaches the fat pointer through memory, and
  disappears on clang 22 and gcc 15, which keeps it in a register and so has
  the extra dereference to spend. At arity 2 the slack is gone and it costs a
  load everywhere (3 → 4). That is the same unevenness the cycle tables show,
  where the policy costs a `virtual_ptr` a fraction of what it costs a
  reference.
- **At two virtual arguments the multi-method's two lookups are independent**:
  each virtual argument's v-table is fetched on its own chain, one `imul` by
  the table stride combines them, and a single indexed call ends it — depth 3
  through a `virtual_ptr` against the idiom's two chained calls.

## Build and run

Boost (1.91+) must be installed system-wide, and `./include` must be a
symlink into a Boost checkout — it is machine-specific and not committed:

```sh
ln -s /path/to/boost/libs/openmethod/include include
```

```sh
./build.sh                       # -> bin/benchmark-g++-64
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
compile-time: `CLASSES=200 ./build.sh` or `-DOMB_CLASSES` with CMake.

## Files

| file                  |                                                                            |
| --------------------- | -------------------------------------------------------------------------- |
| `src/timing.hpp`      | brackets, stamps, cache scrubbing, TSC calibration, statistics             |
| `src/hierarchy.hpp`   | both class hierarchies, yardsticks, the `touch` baselines                  |
| `src/registries.hpp`  | the six dispatch configurations and the const-body methods                 |
| `src/use_methods.hpp` | the use-body methods and overriders                                        |
| `src/main.cpp`        | variants, measurement loop, verification, reporting                        |
| `compilers.conf`      | the compilers to measure, in column order; the first is the reference      |
| `matrix.sh`           | builds, verifies and measures every compiler in that list, N passes        |
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
