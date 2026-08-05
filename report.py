#!/usr/bin/env python3
"""Render every README table from results/run*/.

    python3 report.py [results|indirect|matrix|all] [results-dir]

Three sections, matching the three places README.md carries tables:

  results   the per-build tables under "## Results" (gcc/64 only, const bodies)
  used      the use-world tables under "## Two fair comparisons" (gcc/64)
  indirect  the direct-vs-indirect pair under "## Cost of `indirect_vptr`"
  matrix    the compiler x bitness tables under "## Compiler and bitness"

All of them are medians over the matrix passes. A single cold pass moves by a
median of 11% between repeats, which is larger than the differences being
reported; the median over several passes is what makes the tables mean anything.

Ratios are always to the virtual-function yardstick of the same arity *and the
same hierarchy*, measured in the same build. That is what makes columns
comparable: gcc and clang generate different code for the yardstick itself, and
the inplace hierarchy's objects are a different size.

`om vptr` rows precede `om ref` rows throughout: virtual_ptr is the recommended
way to use the library.
"""

import csv
import glob
import os
import statistics
import sys

COLUMNS = [
    ("gcc/64", "g++-64.csv"),
    ("gcc/32", "g++-32.csv"),
    ("clang/64", "clang++-64.csv"),
    ("clang/32", "clang++-32.csv"),
]

# The build the single-column "## Results" tables are taken from.
BUILD = "g++-64.csv"

REGISTRIES = ("vptr_vector", "indirect", "vptr_map", "flat_map")
INPLACE = ("inplace", "inplace_ind")


def load(paths):
    """(mode, hier, body, group, dispatch, arity) -> row dicts, one per pass"""
    table = {}

    for path in paths:
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                key = (r["mode"], r["hier"], r["body"], r["group"],
                       r["dispatch"], int(r["arity"]))
                table.setdefault(key, []).append(r)

    return table


def med(data, key, field):
    if key not in data:
        sys.exit(f"results incomplete: no rows for {key} -- "
                 "interrupted matrix.sh pass? Re-run ./matrix.sh.")
    return statistics.median(float(r[field]) for r in data[key])


def cycles(v):
    # Warm values are single- to low-double-digit cycles; rounding them to
    # integers would throw away most of the signal.
    return f"{v:.1f}" if v < 100 else f"{v:.0f}"


def dispatch_rows(arity):
    """The yardstick, then every dispatch, virtual_ptr form before reference.

    inplace_vptr gets the reference form only: with the v-table pointer in the
    object, a virtual_ptr would carry a pointer the reference form already has.
    """
    yardstick = "vf" if arity == 1 else "vf+vf"
    rows = [(yardstick, "main", "yardstick", yardstick, arity)]
    rows += [
        (f"om {form} / {reg}", "main", reg, f"om {form}", arity)
        for reg in REGISTRIES
        for form in ("vptr", "ref")
    ]
    rows += [(f"om ref / {reg}", reg, reg, "om ref", arity) for reg in INPLACE]
    return rows


def emit_world(data, body, passes, warm_caption, cold_caption):
    """The results-shaped table pair for one body world."""
    print(warm_caption + "\n")
    print(f"Median of {passes} passes.\n")
    print("| dispatch | arity | net | x vf | disp |")
    print("|---|---|---|---|---|")

    for arity in (1, 2):
        for label, hier, group, disp, ar in dispatch_rows(arity):
            k = ("warm", hier, body, group, disp, ar)
            print(f"| `{label_for(label, group, ar)}` | {ar} | "
                  f"{cycles(med(data, k, 'net'))} | "
                  f"{med(data, k, 'x_net'):.2f}x | "
                  f"{cycles(med(data, k, 'disp'))} |")

    print("\n" + cold_caption + "\n")
    print(f"Median of {passes} passes.\n")
    print("| dispatch | arity | net | disp | x net | x disp |")
    print("|---|---|---|---|---|---|")

    for arity in (1, 2):
        for label, hier, group, disp, ar in dispatch_rows(arity):
            k = ("clflush", hier, body, group, disp, ar)
            print(f"| `{label_for(label, group, ar)}` | {ar} | "
                  f"{med(data, k, 'net'):.0f} | {med(data, k, 'disp'):.0f} | "
                  f"{med(data, k, 'x_net'):.2f}x | "
                  f"{med(data, k, 'x_disp'):.2f}x |")


def label_for(label, group, arity, suffix=""):
    if group != "yardstick":
        return label
    return label + (" (double dispatch)" if arity == 2 and not suffix
                    else suffix)


# ---------------------------------------------------------------------------


def section_results(data, passes):
    direct_m = med(data, ("warm", "main", "-", "baseline", "direct", 0), "mean")
    touch_m = med(data, ("warm", "main", "-", "baseline", "touch", 1), "mean")

    print("### Warm caches — the finest resolution\n")
    print(f"Warm mode resolves the mechanisms' few-cycle differences: reaching "
          f"the receiver\ncosts {touch_m - direct_m:.1f} cycles here, and rows "
          f"repeat within a build to a percent or two.\nBut the yardstick is "
          f"mostly indirect-branch misprediction, which depends on the\n"
          f"binary's layout — across the four builds its net ranges severalfold "
          f"— so warm\n*ratios* are build-local. For figures that transfer, "
          f"read the cold table below.\nThe `inplace` rows are ratioed against "
          f"the inplace hierarchy's own `vf`.\n")
    print(f"Median of {passes} passes.\n")
    direct = med(data, ("warm", "main", "-", "baseline", "direct", 0), "net")
    print(f"For scale: a direct call to a stamping body measures "
          f"{direct:.1f} cycles net here.\n")
    print("| dispatch | arity | net | x vf | disp |")
    print("|---|---|---|---|---|")

    for arity in (1, 2):
        for label, hier, group, disp, ar in dispatch_rows(arity):
            k = ("warm", hier, "const", group, disp, ar)
            print(f"| `{label_for(label, group, ar)}` | {ar} | "
                  f"{cycles(med(data, k, 'net'))} | "
                  f"{med(data, k, 'x_net'):.2f}x | "
                  f"{cycles(med(data, k, 'disp'))} |")

    touch_m = med(data, ("clflush", "main", "-", "baseline", "touch", 1), "net")
    vfn = med(data, ("clflush", "main", "const", "yardstick", "vf", 1), "net")

    print("\n### Caches cold (`clflush`) — the steadiest ratios\n")
    print(f"Flushed, the first touch of the receiver is a cache miss in its own "
          f"right: the\n`touch` baseline nets {touch_m:.0f} cycles, "
          f"against {vfn:.0f} for the whole `vf`\nyardstick. So "
          f"{touch_m / vfn * 100:.0f}% of a virtual call's `net` is reaching the "
          f"object rather than\ndispatching on it. `x net` — a row's total call "
          f"cost divided by the yardstick's —\nis the headline, and the most "
          f"reproducible figure this benchmark produces:\nmisses dominate, "
          f"and misses do not care about code layout. `disp` and\n`x disp` "
          f"are the mechanism-excess diagnostics.\n")
    print(f"Median of {passes} passes.\n")
    print("| dispatch | arity | net | disp | x net | x disp |")
    print("|---|---|---|---|---|---|")

    for arity in (1, 2):
        for label, hier, group, disp, ar in dispatch_rows(arity):
            k = ("clflush", hier, "const", group, disp, ar)
            print(f"| `{label_for(label, group, ar)}` | {ar} | "
                  f"{med(data, k, 'net'):.0f} | {med(data, k, 'disp'):.0f} | "
                  f"{med(data, k, 'x_net'):.2f}x | "
                  f"{med(data, k, 'x_disp'):.2f}x |")


def section_indirect(loaded, passes):
    # label, call form, (hier, group) direct, (hier, group) indirect
    SPEC = [
        ("`virtual_ptr`", "om vptr",
         ("main", "vptr_vector"), ("main", "indirect")),
        ("`virtual_` ref", "om ref",
         ("main", "vptr_vector"), ("main", "indirect")),
        ("`inplace` ref", "om ref",
         ("inplace", "inplace"), ("inplace_ind", "inplace_ind")),
    ]

    def table(mode):
        print("| call form | arity | " + " | ".join(n for n, _ in COLUMNS)
              + " |")
        print("|---|---|" + "---|" * len(COLUMNS))

        for label, call, dkey, ikey in SPEC:
            for ar in (1, 2):
                cells = []

                for name, _ in COLUMNS:
                    d = loaded[name]
                    dv = med(d, (mode, dkey[0], "const", dkey[1], call, ar), "disp")
                    iv = med(d, (mode, ikey[0], "const", ikey[1], call, ar), "disp")
                    delta = f"{iv - dv:+.1f}" if dv < 100 else f"{iv - dv:+.0f}"
                    cells.append(
                        f"{cycles(dv)} → {cycles(iv)} (**{delta}**)")

                print(f"| {label} | {ar} | " + " | ".join(cells) + " |")

    print(f"Median of {passes} passes; `disp` cycles, direct → indirect.\n")
    print("#### Warm — the extra load, uncontended\n")
    table("warm")
    print("\n#### Cold (`clflush`) — the extra load, as a cache miss\n")
    table("clflush")


def section_matrix(loaded, passes):
    def emit(mode, title):
        print(f"#### {title}\n")
        print("| dispatch | " + " | ".join(n for n, _ in COLUMNS) + " |")
        print("|---|" + "---|" * len(COLUMNS))

        spreads = []

        for arity in (1, 2):
            plural = "" if arity == 1 else "s"
            print(f"| **{arity} virtual argument{plural}** | "
                  + " | ".join([""] * len(COLUMNS)) + " |")

            for label, hier, group, disp, ar in dispatch_rows(arity):
                cells = []

                for name, _ in COLUMNS:
                    d = loaded[name]
                    k = (mode, hier, "const", group, disp, ar)
                    if k not in d:
                        sys.exit(f"results incomplete: no rows for {k} in "
                                 f"{name} -- interrupted matrix.sh pass? "
                                 "Re-run ./matrix.sh.")
                    ratios = [float(r["x_net"]) for r in d[k]]
                    ratio = statistics.median(ratios)
                    cells.append(f"{ratio:.2f}x ({cycles(med(d, k, 'net'))})")

                    if len(ratios) > 2 and ratio > 0:
                        spreads.append(
                            (max(ratios) - min(ratios)) / ratio * 100)

                shown = label_for(label, group, ar, " (yardstick)")
                print(f"| `{shown}` | " + " | ".join(cells) + " |")

        if spreads:
            spreads.sort()
            print(f"\nMedian of {passes} passes. Spread across passes: "
                  f"median {statistics.median(spreads):.0f}%, p90 "
                  f"{spreads[int(len(spreads) * 0.9)]:.0f}%.\n")
        else:
            # With 1 or 2 passes there is no spread; the old unconditional
            # median() crashed here (review finding).
            print(f"\nMedian of {passes} passes -- too few for spread "
                  f"statistics.\n")

    emit("clflush", "Caches cold (`clflush`)")
    emit("warm", "Warm caches")


# ---------------------------------------------------------------------------


SECTIONS = ("results", "used", "indirect", "matrix", "all")


def main():
    # Match the section name explicitly. Testing os.path.isdir() instead would
    # read `report.py results` as a results-directory argument, since ./results
    # exists, and silently emit every section.
    args = list(sys.argv[1:])
    section = args.pop(0) if args and args[0] in SECTIONS else "all"
    root = args.pop(0) if args else "results"

    if args:
        sys.exit(f"unexpected argument {args[0]!r}")

    run_dirs = sorted(glob.glob(os.path.join(root, "run*")))

    if not run_dirs:
        sys.exit(f"no {root}/run*/ directories -- run ./matrix.sh first")

    loaded = {}
    counts = []

    for name, fn in COLUMNS:
        paths = [os.path.join(d, fn) for d in run_dirs
                 if os.path.exists(os.path.join(d, fn))]
        if not paths:
            sys.exit(f"missing {fn} in every {root}/run*/")
        loaded[name] = load(paths)
        counts.append(len(paths))

    # min over columns: after an interrupted matrix.sh, counting run dirs on
    # disk overstated the data actually loaded (review finding).
    passes = min(counts)

    single = loaded[next(n for n, f in COLUMNS if f == BUILD)]

    if section in ("results", "all"):
        section_results(single, passes)
        print()

    if section in ("used", "all"):
        emit_world(
            single, "use", passes,
            "#### Warm, receiver used",
            "#### Cold (`clflush`), receiver used")
        print()

    if section in ("indirect", "all"):
        section_indirect(loaded, passes)
        print()

    if section in ("matrix", "all"):
        section_matrix(loaded, passes)


if __name__ == "__main__":
    main()
