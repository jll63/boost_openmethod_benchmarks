# How this benchmark got its numbers

The benchmark did not arrive at its current design; it was corrected into it,
several times, and the corrections are more instructive than the design. This
file is the chronicle. **Numbers quoted in older episodes come from the
measurement schemes of their time and are not comparable with the current
tables** — several redesigns changed what the timed window contains, on
purpose.

Commits named below are in this repository's history.

## Origins (`eec4e04`)

Boost.OpenMethod's documentation claims that dispatching through a reference
is "between 30% and 50% slower than calling the equivalent virtual function",
attributed to "micro- and RDTSC-based benchmarks" that did not exist in the
repository. This project set out to supply them: one dispatch, timed by
`rdtsc`, caches deliberately scrubbed, a virtual function as the yardstick,
and — from the start — a `--verify` gate calling every dispatch path before
any timing ran.

The first design measured a paired-bracket window: `lfence; rdtsc; lfence` …
work … `rdtscp; lfence`, with an empty-region baseline (`ovh`) subtracted.
Axes accreted quickly: four registries, two call forms, two arities, then
gcc/clang, 64/32-bit, and the `inplace_vptr` hierarchies. Two early findings
still stand: an lfence-bracketed `rdtsc` pair reads 25 or 50 cycles bimodally
on this machine, so only a trimmed mean resolves below one tick; and cold
absolutes swing ~25% between runs while same-run ratios hold, which is why
every published cell is a median over seven interleaved passes.

## The adversarial review (`44e1d0f`)

A 27-agent adversarial review of the initial commit — six finder lenses, one
skeptic per finding, a completeness critic — confirmed 16 defects. The three
that mattered:

- **The cold `disp` column subtracted a receiver miss from rows that never
  touch the receiver.** A `virtual_ptr` timed region provably never
  dereferences the object, yet `disp = mean − nvf` credited it with the
  baseline's ~270-cycle cold miss, understating its dispatch cost about 2x.
- **`clflush` mode left the `type_info` line warm whenever a v-table was
  64-byte aligned** — the flush region started at the v-table's address point
  and `flush_range` aligns down, so the slot at vptr−8 escaped. Link layout
  made the hole ~3x more frequent under clang than gcc: a compiler-asymmetric
  bias.
- **The verify gate was circular at arity 2**: the expected value was computed
  by one of the dispatch paths under test, so every registry being wrong the
  same way passed unseen. The fix derives oracles from the class contracts
  alone — and was demonstrated the same day against a live specimen: a working
  tree whose diagonal overrider pack had been unregistered failed the
  strengthened gate on the spot.

Also from the review: `matrix.sh` blending stale committed passes into fresh
medians, a pipeline swallowing the cold leg's exit status, crashes on
malformed CLI input, and a 32-bit `long` overflow that a 3-second SIGSTOP
during calibration turned into a 56 GHz "measured" TSC frequency.

## The receiver asymmetry, and the two worlds (`578f549`)

The review fix created its own unfairness: subtracting the receiver-touch from
the yardstick but not from `virtual_ptr` rows charged the latter full price
while excusing the virtual function part of its own mechanism — the load it is
"charged" for *is* the vptr fetch. The resolution reframed the benchmark: no
single statistic is fair to a mechanism that must load the receiver and one
that need not, so measure two worlds. In the **delivery world** bodies return
constants (the no-op-default pattern — call overhead is the entire cost); in
the **use world** every body reads a member of every receiver, every row owes
the receiver's line exactly once, and the subtraction is fair everywhere.

The use world answered a question arithmetic could not: the `virtual_ptr`
body's deferred receiver miss costs *nothing* — it completes under the table
misses, because the object address travels in the fat pointer and is known
before the indirect call resolves.

## The arrival window (`5b86a4c`)

The deepest redesign came from a simple suggestion: move the second counter
read into the measured body and return it. Every yardstick, baseline and
overrider now returns a `{stamp, id}` pair — the stamp taken by `rdtscp` on
arrival (it waits for every prior instruction, i.e. the whole dispatch chain),
the id carrying the verify oracle, computed after the stamp, outside the
window. The closing bracket ceased to exist, and with it the return path and
every stop-side scheduling artifact.

The redesign settled an embarrassment that had survived several revisions as
"an unexplained artifact": warm mode reported arity 2 as *cheaper* than arity
1 for reference dispatch — physically impossible, since the two-argument
dispatch strictly does more work. Under the arrival window the ordering is
correct. The inversion had lived in the return path all along.

### Two flags that had to be equalised

Both were found by reading disassembly, and both had been silently biasing the
compiler axis:

- **`-fcf-protection=none`.** Ubuntu's gcc defaults to `=full`, clang to none,
  so the gcc binary carried 7391 `endbr64` landing pads against clang's 5 —
  one at the top of every indirect-call target, including every overrider and
  every virtual function. The axis was comparing a distribution's hardening
  policy, not code generation. Both builds pass `-fcf-protection=none` and the
  callees are byte-identical:

  ```asm
  ; gcc, before          ; gcc and clang, now
  endbr64                lea eax, [rsi+0x1]
  lea eax, [rsi+0x1]     ret
  ret
  ```

- **How the timestamp is assembled** — below.

### Why the timestamp is assembled afterwards

`rdtsc` returns the counter in `edx:eax`. With the `__rdtsc()` intrinsic, the
64-bit assembly — `shl`, `mov`, `or` — was compiler-scheduled, and in the era
when the harness had a *closing* bracket too, each compiler placed it
differently: gcc inside the measured window, clang outside, and in one
baseline clang sank the `or` past the work, so the arithmetic did not cancel
in `disp` and the cross-compiler columns carried a scheduling artifact.

Two designs removed the problem in sequence. First, both brackets captured raw
`lo`/`hi` in inline asm and assembled afterwards, making the bookkeeping
identical across every variant within a compiler. Then the closing bracket was
abolished altogether — the stop moved into the measured bodies, which is the
harness's own code, emitted identically everywhere. What survives of the
original issue is the start side: `tsc_start` captures the raw pair, and the
compiler schedules its assembly as it pleases — believed at the time to be
variant-identical and cancelling; the post-rewrite verification episode below
later showed it is neither, merely bounded and sub-cycle.

## From excess to whole call (`3789262`)

The arrival window's first baseline was a *direct call*, which quietly changed
the headline into `(om − direct) / (vf − direct)`: each mechanism's excess
over a plain call. Sharper-looking ratios — and the wrong question. The
benchmark exists to compare an open-method call with a virtual function call,
whole against whole; deciding what a plain call "should" cost is not part of
that comparison. The subtracted baseline became `probe` (the empty window:
apparatus only), `x net` became the headline, and `direct` was demoted to a
reference point quoted with the tables. The README's "What the ratios divide"
section exists so the two ratio families can never be conflated again.

## Warm was never the sharpest (`c9755d6`)

The warm tables carried the heading "the sharpest numbers" from the earliest
design, when it meant pass-to-pass repeatability. Challenged, the claim
collapsed on two counts. The caption's reasoning — "`net` and `disp` therefore
agree" — had been true under the direct-based net and silently false since the
probe correction (they differ by the plain-call cost by construction). And the
sharpness itself inverted under measurement: warm `om ref` ratios spread 42%
*across builds*, because a warm virtual call is mostly indirect-branch
misprediction and the predictor's behaviour depends on binary layout (the warm
yardstick ranged 6.4-16.8 cycles net over the four builds), while the cold
whole-call ratio held to 3.8%. The headings became "the finest resolution"
(warm — build-local ratios) and "the steadiest ratios" (cold).

## The chronicle splits out, the README gets audited (`1d290b0`, `d9fe8b9`)

The history moved into this file so the README could be results-first — and
promptly proved it needed more than trimming. A four-lens adversarial audit
(scheme leftovers, figure verification, structure, claims-vs-code) confirmed
**54 defects** in the README: figures from three different measurement
vintages quoted side by side as current; the entire "sweep is the mode to
quote" doctrine recommending a mode absent from the published dataset; "read
`x disp`" advice inverting the current headline; a `net = mean − direct`
definition surviving in the Results intro; a false claim that upstream 32-bit
CI was MSVC-only (gcc had carried `ADDRMD: '32,64'` all along — the claim came
from a commit message never checked against the file); a TSC figure 3.5% off
what the committed data implies; and stale reproducibility spreads from two
schemes ago.

Rather than patch, the README was rewritten from scratch: 730 lines from 993,
results-first, one telling of each concept, every inline figure re-derived
from the committed dataset, fresh disassembly. The audit also surfaced one
code gap: `verify()` had skipped the use-world `virtual_ptr` methods for the
two map registries; the gate now covers every path.

## The document gets the code's treatment (`60bf2e1`)

A fresh three-lens verification of the rewritten README caught the rewrite
minting its own errors — the worst being wishful: the opening claimed the
multi-method beats double dispatch "in every configuration measured" while
its own supporting bullet cited **1.14x cold** — a losing number — as
evidence. The honest split: warm through `virtual_ptr` the multi-method
halves the idiom's cost (0.52x); cold, the idiom's two lean chains win back
14%, because the two-dimensional dispatch data spans more cache lines than
two v-table chains; and the map registries' reference form loses even warm.

The same pass killed a fresh overclaim that the start-stamp bookkeeping is
"identical in every variant and cancels against `probe`" — the disassembly
shows gcc scheduling those ALU ops differently per variant, with none at all
inside `probe`'s window; it is now documented as a bounded sub-cycle
asymmetry, the floor on reading warm figures. And it restored a caveat the
rewrite had lost: `clflush` cannot evict the map registries' runtime-allocated
bucket arrays, so their cold rows read slightly better than a truly cold map
would. The moral joined the project's working rules: rewritten prose gets the
same adversarial gate as new code, because fresh text mints fresh errors.

## Odds and ends

- A cgroup-shielding experiment (à la Boost.Unordered's `cgexec` runs) showed
  deliberate CPU and memory co-tenancy moves the results *less* than idle
  run-to-run variance — the trimmed mean already discards preemptions — so the
  benchmark does not shield, and the README says why.
- The receiver-touch baseline was called `nvf` for most of this history —
  "non-virtual `vf`", coined when there was exactly one virtual function to be
  the non-virtual sibling of, and expanding to *non-virtual virtual function*
  ever after. It is now `touch` (`c6227ef`).
- The misprediction experiment (`CLASSES=1` vs 100) was measured under the
  previous, return-path-inclusive window; its point — a virtual call's cost is
  mostly indirect-branch misprediction, and yardstick differences between
  compilers are BTB behaviour, not dispatch quality — survives every scheme.
  The archived data (that scheme's `disp`, warm, not comparable with current
  tables):

  | `disp` cycles, warm | gcc | clang |
  |---|---|---|
  | `vf`, 1 class (predictable) | 2.4 | 6.3 |
  | `vf`, 100 classes (mispredicted) | 14.6 | 11.6 |
  | `om ref`, 1 class | 9.0 | 9.3 |
  | `om ref`, 100 classes | 18.7 | 18.2 |
