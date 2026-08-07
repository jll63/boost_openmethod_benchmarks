# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

An RDTSC micro-benchmark that times **one** Boost.OpenMethod dispatch with the caches scrubbed,
against a virtual function call as the yardstick. Read `README.md` first — it is the deliverable, not
just documentation, and it records the methodology and the reasons behind most of the design.

## Build and run

```sh
./build.sh                       # -> bin/benchmark-g++-64
CLASSES=200 ./build.sh           # leaf count is compile-time (Derived<N> is a template)
DEBUG=1 CLASSES=4 ./build.sh     # -> bin/benchmark-g++-64-g, -O0 -g, for tracing

bin/benchmark-g++-64 --verify    # correctness gate, see below
./run.sh                         # pins a core, one build, full matrix of variants

./matrix.sh                      # every compiler in compilers.conf, RUNS=5 passes -> results/run<k>/
python3 report.py                # renders ALL measured README tables from results/run*/ + results-zen/
python3 report.py matrix         # or just one section: results|indirect|matrix
./probes.sh                      # the disassembly oracle -> bin/probe-*
python3 asmtab.py                # renders "Every timed region" by disassembling bin/

cmake -S . -B build && cmake --build build && (cd build && ctest)
```

`matrix.sh` honours `CPU`, `REPS`, `RUNS`, `OUTDIR`; `run.sh` honours `CPU`, `REPS`, `BIN`.
Benchmark flags: `--reps --objects --sweep-mb --cpu --seed --mode warm|clflush|sweep|all --csv
--verify`.

There is no test suite beyond `--verify`, which is also the only ctest target.

**25 leaf classes, and that is measured, not inherited.** A sweep over 10/25/50/100/200 moved no
cold ratio outside the pass noise -- cold, a dispatch misses one line per level of its chain
regardless of how many classes exist. The old 100 existed to keep the indirect call unpredictable
for the *warm* numbers, which are no longer published. It matters because compile time is
superlinear in the count and dominated by the optimizer, not the metaprogramming: `-ftime-report`
on g++ 15 puts template instantiation at 6% and `callgraph functions expansion` + `ipa passes` at
88%, and 100 classes took it over half an hour against 62 s at 25. Switching the hierarchy from
templates to Boost.PP would attack the 6%.

## Dependencies

`include/` is a **symlink into a local Boost checkout** — `ln -s <boost>/libs/openmethod/include
include` — so OpenMethod is compiled live from that working tree; everything else comes from the
Boost 1.91 installed in `/usr/local/include`. Header-only, nothing is linked. Edits to
`libs/openmethod` take effect on the next `./build.sh` with no reinstall. The symlink is
machine-specific and is not committed.

The compilers measured are listed in `compilers.conf`, one per line, in column order; the first
is the reference build the single-column sections come from. `matrix.sh`, `probes.sh`, `report.py`
and `asmtab.py` all read it, so adding a compiler is adding a line and re-running them. The
repository lives at
https://github.com/jll63/boost_openmethod_benchmarks (branch `master`); `bin/`, `build/`,
`.vscode/` and the `include` symlink are local-only and gitignored.

## Architecture

### Variants are types, not data

`src/main.cpp` models every measurement as a struct with a fixed shape:

```cpp
struct v_something {
    static constexpr const char *name, *group, *hier;   // identity in the output
    static constexpr int arity;                          // 0, 1 or 2
    using args = ...;                                    // what the call needs
    static auto prepare(env&, size_t i, size_t j) -> args;   // OUTSIDE the timed region
    static auto call(args, int x) -> int;                    // the only thing timed
    static void regions(args, std::vector<region>&);         // what clflush mode evicts
};
```

`run<V>()` is templated on it, so `call` inlines into the timed region and no `std::function`
indirection is measured. **Adding a dispatch means writing one of these and adding it to the
`OMB_RUN*` macro list in `main_impl`** — nothing else is table-driven.

Ordering convention: `om vptr` rows precede `om ref` rows everywhere (`virtual_ptr` is the
recommended API), in the `OMB_RUN` macro, in `report.py`, and in the README tables.

### Two compensations, and they are the point

- `net = mean - probe` removes the apparatus only (the empty window: bracket + stamp). It is the
  whole-call cost, and `x net` — a row's total call cost over the yardstick's — is the headline
  ratio. `direct` (a
  real call to a stamping body) is a quoted reference point, subtracted only in `disp` for rows
  that never touch the receiver. Do not make excess-over-direct the headline: that answers "what
  does the mechanism add", not "open method vs virtual function".
- `disp = mean - touch` additionally removes *reaching the receiver* — `touch` is a non-virtual member
  call that loads the object but dispatches on nothing. Rows whose timed region never touches the
  receiver (`om vptr` with const bodies: the v-table pointer is already in the arguments) set
  `touches_receiver = false` and subtract `direct` instead (see the bullet above) — subtracting
  `touch` from them fabricates a credit for a miss they never paid.

### Two body worlds

Every variant carries `body`: `const` bodies return compile-time constants (the delivery world —
the receiver is touched only where the mechanism requires it; this is the no-op-default pattern,
where call overhead is the entire cost, so it is a real workload, not a control; fair cross-form
statistic is `net`);
`use` bodies read a member of every receiver (`src/use_methods.hpp`, `vfu`/`ddu` yardsticks — the
world where `disp` is fair for every row, virtual_ptr included, because everyone owes the receiver
line exactly once). The worlds exist because a virtual function *must* load the receiver (the vptr
lives there) and a `virtual_ptr` call need not — no single statistic is fair to both. Baselines are
body-neutral (`body = "-"`). Yardstick matching in `report()` and `report.py` keys on body; use
rows ratio against the use yardstick. See README, "Two fair comparisons". Cold, that is ~45% of what a virtual call
  appears to cost, and it is common to every variant, so `net` alone compresses every ratio toward
  1.

`x net` (ratio to the same-arity `vf` yardstick) is the headline statistic, and the only ratio the
tables now print: the cold tables were decluttered to `dispatch | arity | x net (Intel) | x net
(Zen)`, and `x disp` was dropped from them (it lives on only as a diagnostic, quoted in `disp`
cycles by the indirect-cost table). Cold absolutes swing ~25% run to run; same-run ratios do not.

### Two class hierarchies, and per-hierarchy baselines

`src/hierarchy.hpp` has `Base`/`Derived<N>` and, separately, `IBase<R>`/`IDerived<R,N>` for
`inplace_vptr`. The second hierarchy is unavoidable: `inplace_vptr_base` declares
`friend auto boost_openmethod_registry(Class*) -> Registry`, so **a class binds to exactly one
registry**, and direct and indirect inplace need one instantiation each.

Because inplace objects are a different size (24 vs 16 bytes), each hierarchy carries **its own
`touch` baseline and `vf` yardstick**. Baseline and yardstick lookups in `report()` are keyed by
`(mode, arity, hier)` — if you add a hierarchy, it needs its own baselines or `disp` is meaningless.

### Registries and methods

`src/registries.hpp` defines six dispatch configurations (four registries plus two inplace) and the
methods. **Four are published.** The two `vptr_map` registries are still built, measured and
verified -- `report.py`'s `control()` reads their `virtual_ptr` rows to check that the vptr policy
stays off that call path -- but `REGISTRIES` in `report.py` leaves them out of the tables:
`clflush` cannot reach a hash map's runtime-allocated bucket arrays, so their cold rows keep
interior state resident and read better than a truly cold map would. A flattering number under a
caveat confused more than it informed. Overriders are registered in bulk through the core API, not the `BOOST_OPENMETHOD_*`
macros, because the leaves are a class template — `method<...>::override` takes a *pack* of
functions, so one static registrar carries all N:

```cpp
template<class R, std::size_t... I>
auto poke_ref_registrar(std::index_sequence<I...>)
    -> typename poke_ref<R>::template override<poke_ref_impl<R, I>...>;

BOOST_OPENMETHOD_REGISTER(decltype(poke_ref_registrar<R>(all_indices{})));
```

### Timing harness

`src/timing.hpp`. The `rdtsc` brackets live *inside* a `noinline` function whose parameters are the
call arguments, so the call and prologue are untimed while the arguments still arrive through the
ABI and cannot be constant-folded or devirtualized. Three cache modes: `warm`, `clflush` (targeted
`clflushopt` over the regions each variant declares) and `sweep` (64 MiB of ordinary stores, ~2.7x L3).
**Only `clflush` is measured and published.** Warm resolves a cycle or two, but warm a virtual call
is mostly an indirect-branch mispredict whose cost moves with binary layout, so warm ratios are
build-local -- they changed sign between clang 18 and clang 22 without the dispatch changing at
all. See README, "Why cold only". The 32-bit axis is likewise gone: `build.sh` still honours
`BITS`, nothing else does.

## Tracing the code

`-O2` is unsteppable: the dispatch inlines into `timed_call` and the locals are gone. Build
`DEBUG=1 CLASSES=4 ./build.sh` instead (`-O0 -g`, `-g`-suffixed binary, small hierarchy). Its
timings are meaningless — that is not what it is for. `.vscode/launch.json` (local editor config, not committed) has *trace: verify (debug build)* and
*trace: one dispatch (debug build)* wired to a *build debug* task; a fresh clone has only the
`DEBUG=1` build.sh path.

`--verify` is the best entry point: it walks every dispatch path once with no timing loops around
it. The chain to a dispatch, with the stops worth breaking on:

| what | where |
|---|---|
| CLI, `initialize<R>()` per registry, then the run list | `main_impl`, `src/main.cpp` |
| every dispatch path once, no timing | `verify`, `src/main.cpp` |
| the measurement loop for one variant (scrub, pair the draws, sample) | `run<V>`, `src/main.cpp` |
| the timed region — rdtsc brackets around `V::call` | `timed_call<V>`, `src/main.cpp` |
| the one call being measured | `V::call`, e.g. `v_om_ref1::call` |
| the library's dispatch entry | `method::operator()`, `core.hpp` |
| generated trampoline to the overrider | `method::thunk::fn`, `core.hpp` |
| the overrider that finally runs | `poke_ref_impl` / `poke_vp_impl`, `src/registries.hpp` |

Line numbers move; break on the names, or on `src/registries.hpp` at the overrider body, which is
the fastest way to land mid-dispatch with the whole chain on the stack.

## Invariants that must not be broken

- **`--verify` is the correctness gate, and its oracles are independent of the paths under
  test**: `x + tag` for arity 1, the tag rule for arity 2, `x + b.tag` for the dd yardstick, and
  for the use world `x + a.tag + b.tag` on the diagonal against `x - a.tag - b.tag` in the
  catch-all — distinct values, so verify can tell which overrider ran.
  It used to compare the open methods against each other, which passed when every registry was
  wrong the same way (e.g. an unregistered overrider pack). A registry wired to the
  wrong method, or an overrider that silently failed to register, otherwise produces a plausible but
  meaningless table. It runs at startup of every measurement run.
- **The `virtual_ptr` control**: the three *direct* registries' `om vptr` rows must agree, because
  the vptr policy is not on that call path -- they compile to the same two instructions, differing
  only in which `method::fn` word holds the slot. All three are still measured, but the tables
  print one `vptr` row: three rows of the same call invited reading noise as a difference.
  `report.py`'s `control()` is where the other two are read, and it warns when they diverge beyond
  the noise floor. If they do, the measurement is wrong, not the library. `indirect` is excluded --
  it adds a load there by design.
- **Every variant must replay the identical receiver sequence**: `run<V>()` reseeds `e.rng` per
  variant. `disp` is a difference of two measurements, and cold both are dominated by which objects
  were drawn; unpaired draws leave that term uncancelled.
- **`_mm_lfence()` is not a compiler barrier.** The `asm volatile ... "memory"` in `tsc_before`/
  `tsc_after` are what stop the optimizer hoisting the loop-invariant global loads (hash factors,
  table base, method slot) out of the timed region.
- **Inplace classes register via their constructor being instantiated.** `ifactories<R>()` forces
  every leaf's constructor; a leaf that is never constructed is never registered and `initialize()`
  rejects the overrider naming it.
- **Read the mean, not the median.** An lfence-bracketed `rdtsc` pair is quantized to the TSC
  tick (~30 cycles on this machine), so medians land on a tick. The mean (trimmed of its top 5%,
  where preemptions land) resolves below one tick.

## Library facts that are easy to get wrong

Confirmed against the Boost.OpenMethod sources (`libs/openmethod` in a Boost checkout):

- The library says **`registry`**, not "policy". Customize with `default_registry::with<...>` /
  `::without<...>`. There is no `fork`/`rebind`/`replace` — those are yomm2 spellings.
- A registry is identified by its **policy list**, not by the struct that derives from it. Two
  structs with identical lists are one registry and share state.
- `vptr_map` takes an **mp11 quoted metafunction**, not a template-template parameter:
  `vptr_map<mp11::mp_quote<boost::unordered_flat_map>>`.
- `initialize<Registry>()` must be called for **each** registry, from a TU that includes
  `<boost/openmethod/initialize.hpp>`.
- `registry_regions()` reaches into `boost::openmethod::detail` for the dispatch-table arena
  (`registry_state_type::dispatch_data`). Deliberate — `clflush` mode must evict exactly what the
  dispatch reads.

## Measurement discipline

The machine is an Intel Alder Lake laptop (i7-1280P, 6 P-cores + 8 E-cores) on native Linux and is
**not idle** (load average ~1). Pin measurements to a P-core (the default `CPU=2`); an E-core would
characterize a different core, not a different dispatch. Consequences that have already bitten:

- A single cold pass moves by a median of 11% between repeats (p90 40%). `matrix.sh` therefore loops
  the whole matrix and `report.py` takes the **median across passes**; looping the matrix rather
  than repeating each build in place also spreads drift over both columns alike.
- Warm has the finest resolution *within* a build (a few percent between passes) but its ratios
  are build-local: the warm yardstick is mostly indirect-branch misprediction, whose cost depends
  on binary layout (its net ranges severalfold across the four builds). The cold whole-call ratios
  are the reproducible characterization (~4% across builds for `om ref`). Sub-cycle warm gaps
  remain untrustworthy — scheduling phase; see HISTORY.md for the artifacts this produced.
- **Co-tenancy moves absolutes, not ratios** (measured on this box, 6 passes/condition). A spinner on
  the measurement core is absorbed by the trimmed mean (+0.4%, within the 1.6% idle spread); other-core
  spinners (~+5-7%) and 256 MiB memory streamers (~+3%) inflate absolute cold cycles via all-core-turbo
  clock reduction, but the yardstick moves with them so the published ratios hold (headline `x_disp`
  3.63 idle → 3.77 other-core). Pin to a lightly loaded P-core for stable absolutes; the ratios are
  robust regardless. See "Co-tenancy moves the absolutes, not the ratios" in README.md.
- The check that matters is the `virtual_ptr` control, not a tidy `uptime` — discard any run where
  the three direct registries' `om vptr` rows disagree.

## Editing the README and HISTORY.md

Benchmark evolution — superseded designs, resolved artifacts, review chronicles — belongs in
HISTORY.md, not the README. The README describes the current scheme and its results; when a
redesign obsoletes prose, move the story to HISTORY.md (with the old numbers, marked as
scheme-specific) and leave a link. The README's bottom "History" section points there.

The tables are a **cross-machine comparison**: `report.py` reads the Intel box's data from
`results/` and the AMD Zen 5 box's committed dataset from `results-zen/` (archived from git
history; kept out of the `/results-*/` ignore by a `!/results-zen/` exception, because report.py
hard-fails without it). The cold tables print `x net (Intel) | x net (Zen)`; "The compilers" is a
clang 22 / g++ 15 × Intel / Zen grid; the indirect-cost table stays Intel-only, clang 22 / g++ 15.
Column selection lives in `main()` (`matrix_cols`, `indirect_cols`); the single-column `x net`
sections come from Intel clang 22. `compilers.conf` lists only clang++-22 and g++-15 now — g++-13
was dropped altogether (removed from the manifest, and its CSVs deleted from both datasets).

Every table, and the paragraph of prose above each one, is generated by `report.py` — including the
captions that quote figures. Edit the generator, not the README, or the next regeneration reverts
you. `python3 report.py` emits all three sections; splice each into its section. After any
re-measurement regenerate **all** of them from the same `results/`, so the document never mixes
datasets. The published tables were
produced with `RUNS=7 REPS=6000 ./matrix.sh` (the `matrix.sh` default is 5 passes); `report.py`
prints the pass count under each table, so a regeneration says how it was made.

Prose quotes ~17 figures inline, deliberately limited to **warm gcc/64** values, which reproduce to
~1-2%, plus disassembly facts and the control. Volatile figures (cold cycle counts, per-build ratio
lists) are stated qualitatively on purpose. Re-verify the inline figures against `results/` after a
re-run; do not reintroduce volatile ones.

`./probes.sh` builds `src/dispatch_probe.cpp` -- every dispatch compiled standalone -- into
`bin/probe-*`. It is never measured and never published: it is the oracle `asmtab.py` checks its
backward slice against, so a compiler that rearranges the timed window fails the run instead of
silently shortening a listing. Rebuild it whenever the binaries are rebuilt; the check is skipped
with a note if it is missing. Its bodies mirror `V::call` in `main.cpp` and must be kept in step.

`asmtab.py` owns the whole "Every timed region" section — prose, table and listings — the same way
`report.py` owns the measured ones. It reads the four **binaries**, not `results/`, so regenerate it
after any change to `src/` or the build flags, having rebuilt all four. Its figures are computed,
and the claims that hold "on every build" are guarded: `uniform()` aborts rather than print a claim
that has stopped being true, naming the row that broke it. It also fails loudly on a global read it
cannot classify, instead of labelling it something plausible.

Both generators emit **prettier-aligned** markdown tables (`align_tables` in `report.py`, which
`asmtab.py` imports), because the editor reformats the README on save; matching that shape keeps
regeneration from churning several hundred whitespace-only lines.
