#!/usr/bin/env python3
"""Render the timed regions of every dispatch, from the four built binaries.

    python3 asmtab.py [summary|listings|all] [bindir]

The measured window is what `timed_call<V>` executes between the start bracket
(`lfence; rdtsc; lfence`) and the `call` into the body, whose first instruction
is `rdtscp`. Every variant is its own `noinline` instantiation, so the window is
a contiguous instruction range inside a symbol this script can find by name.

Operands that read a global are rewritten to what they are -- `[rip+slot]`,
`[got+mult]` -- so the four builds can be read side by side. The roles are
inferred from *use*, not from symbol offsets, because the same three registry
fields sit at different offsets in the 32- and 64-bit builds:

    imul reg, [x]           -> x is the hash multiplier
    shrx a, b, reg          -> reg's source is the hash shift
    mov  r, [base + i*n]    -> base's source is the vptr vector
    call [base + i*n]       -> i's source is the method slot

The map registries are excluded: their dispatch inlines an unordered_map probe,
which is a container walk rather than the arithmetic the other registries do.

What the listings show is the dispatch *as it was measured*, sliced out of the
timed window. To keep that slice honest, ./probes.sh compiles the same
dispatches standalone (src/dispatch_probe.cpp) and this script checks that no
instruction of the standalone dispatch is missing from the slice -- a compiler
that rearranges the window then fails the run instead of quietly shortening a
listing. The check is skipped, with a note, when the probe binaries are absent.

Requires the binaries; build them with ./matrix.sh (or build.sh per config),
and the oracle with ./probes.sh.
"""

import collections
import glob
import io
import os
import re
import subprocess
import sys
import textwrap
from contextlib import redirect_stdout

# The README is table-aligned by the editor's markdown formatter on save;
# emitting the same shape keeps regeneration from churning whitespace.
from report import align_tables, compilers, label_of

BUILDS = [(label_of(c), f"benchmark-{c}") for c in compilers()]

# label, call form, arity, variant struct, tag template argument.
#
# The tag pins the registry: clang keeps these symbols mangled (its demangler
# gives up on the pointer-template-argument), but the mangled string still
# spells the struct and the tag, so substring matching works on both compilers.
VARIANTS = [
    ("vf (yardstick)", "", 1, "v_vf1", None),
    ("vf+vf (yardstick)", "", 2, "v_vf2", None),
    ("vptr", "", 1, "v_om_vp1", "group_vector"),
    ("vptr_vector", "ref", 1, "v_om_ref1", "group_vector"),
    ("vptr", "", 2, "v_om_vp2", "group_vector"),
    ("vptr_vector", "ref", 2, "v_om_ref2", "group_vector"),
    ("vptr / indirect", "", 1, "v_om_vp1", "group_indirect"),
    ("indirect", "ref", 1, "v_om_ref1", "group_indirect"),
    ("vptr / indirect", "", 2, "v_om_vp2", "group_indirect"),
    ("indirect", "ref", 2, "v_om_ref2", "group_indirect"),
    ("inplace", "ref", 1, "v_ip_ref1", "group_inplace"),
    ("inplace", "ref", 2, "v_ip_ref2", "group_inplace"),
    ("inplace_ind", "ref", 1, "v_ip_ref1", "group_inplace_ind"),
    ("inplace_ind", "ref", 2, "v_ip_ref2", "group_inplace_ind"),
]


# (probe function, registry spelled in its mangled symbol) per variant, for the
# oracle in src/dispatch_probe.cpp. Keyed by the same (struct, tag) pair the
# timed windows are found by.
PROBES = {
    ("v_vf1", None): ("probe_vf1", None),
    ("v_vf2", None): ("probe_vf2", None),
    ("v_om_vp1", "group_vector"): ("probe_om_vp1", "default_registry"),
    ("v_om_ref1", "group_vector"): ("probe_om_ref1", "default_registry"),
    ("v_om_vp2", "group_vector"): ("probe_om_vp2", "default_registry"),
    ("v_om_ref2", "group_vector"): ("probe_om_ref2", "default_registry"),
    ("v_om_vp1", "group_indirect"): ("probe_om_vp1", "indirect_registry"),
    ("v_om_ref1", "group_indirect"): ("probe_om_ref1", "indirect_registry"),
    ("v_om_vp2", "group_indirect"): ("probe_om_vp2", "indirect_registry"),
    ("v_om_ref2", "group_indirect"): ("probe_om_ref2", "indirect_registry"),
    ("v_ip_ref1", "group_inplace"): ("probe_ip_ref1", "inplace_registry"),
    ("v_ip_ref2", "group_inplace"): ("probe_ip_ref2", "inplace_registry"),
    ("v_ip_ref1", "group_inplace_ind"):
        ("probe_ip_ref1", "inplace_indirect_registry"),
    ("v_ip_ref2", "group_inplace_ind"):
        ("probe_ip_ref2", "inplace_indirect_registry"),
}


def shape(text):
    """An instruction reduced to what a register allocator cannot change.

    Registers become `r`, so the same dispatch compiled twice compares equal
    even though it was allocated differently; the terminating `jmp` of a
    tail-called probe becomes `call`, which is what the same code does when a
    return value has to be kept. Role names (`slot`, `mult`, ...) survive,
    because those say which global is being read.
    """
    mnem = text.split()[0]
    ops = text[len(mnem):].strip()
    ops = re.sub(r"(?<![a-z0-9])(?:[re][a-z]{2}|r\d+[dwb]?|xmm\d+)", "r", ops)
    # `[ebp+0x0]` and `[edi]` are the same load; only the encoding differs.
    ops = ops.replace("+0x0]", "]")
    return ("call" if mnem == "jmp" else mnem) + " " + ops


def newest_source():
    """mtime of the newest file the binaries are built from."""
    return max(os.path.getmtime(f)
               for f in glob.glob("src/*") + ["build.sh"]
               if os.path.exists(f)
               # Built only by probes.sh, into its own binaries.
               and not f.endswith("dispatch_probe.cpp"))


def run(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{cmd[0]} failed: {r.stderr.strip()[:200]}")
    return r.stdout


def disassemble(path):
    """symbol -> [(addr, text)], Intel syntax, addresses as ints."""
    out = run("objdump", "-dC", "-M", "intel", "--no-show-raw-insn", path)
    funcs, cur = {}, None

    for line in out.split("\n"):
        m = re.match(r"^([0-9a-f]+) <(.+)>:$", line)
        if m:
            cur = []
            funcs[m.group(2)] = cur
            continue
        m = re.match(r"^\s+([0-9a-f]+):\t(.*)$", line)
        if m and cur is not None:
            # Drop objdump's trailing `# addr <symbol>` comment here, at the
            # only place that knows it is a comment: a demangled C++ symbol is
            # full of commas and angle brackets, and every operand parse
            # downstream would otherwise split on them.
            text = re.sub(r"\s+#.*$", "", m.group(2))
            text = re.sub(r"\s+", " ", text).strip()
            if text:
                cur.append((int(m.group(1), 16), text))

    return funcs


def symbols(path):
    """Sorted [(addr, size, name)] of defined data/function symbols."""
    syms = []

    for line in run("objdump", "-tC", path).split("\n"):
        m = re.match(r"^([0-9a-f]+)\s+\S+\s+\S+\s+\S+\s+([0-9a-f]+)\s+(.*)$",
                     line)
        if m:
            syms.append((int(m.group(1), 16), int(m.group(2), 16),
                         m.group(3).strip()))

    return sorted(syms)


def resolve(syms, addr):
    """The symbol containing addr, as (name, offset), or None."""
    lo, hi = 0, len(syms)

    while lo < hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid

    for base, size, name in reversed(syms[max(0, lo - 8):lo]):
        if base <= addr and (addr < base + size or size == 0):
            return name, addr - base

    return None


def find(funcs, struct, tag, build):
    """The one timed_call instantiation for this variant.

    `I` closes the struct name in clang's mangled symbols (Itanium template
    args), `<` in gcc's demangled ones; the tag must not run on into a longer
    name (group_inplace vs group_inplace_ind). The lookbehind is not `\\b`:
    in a mangled name the struct is preceded by its length (`8v_om_vp1`), and
    digit-to-letter is not a word boundary.
    """
    hits = [k for k in funcs
            if "timed_call" in k
            and re.search(rf"(?<![A-Za-z_]){struct}[<IE>]", k)
            and (tag is None or re.search(rf"{tag}(?![a-z_])", k))]

    if len(hits) != 1:
        sys.exit(f"{build}: {struct}/{tag} matched {len(hits)} symbols, "
                 "expected 1")

    return funcs[hits[0]]


def pic_base(body, upto):
    """{register: value} for the i386 PIC bases, wherever they are set up.

    Two idioms: gcc calls a `__x86.get_pc_thunk.<reg>` helper, clang calls the
    next instruction and pops the return address. Either way the register ends
    up holding (address of the instruction after the call) + the addend of the
    `add` that follows. Scanned across the whole window as well as the
    prologue: clang 22 sets the base up *inside* the timed region at -m32.
    """
    bases = {}

    for i, (addr, text) in enumerate(body[:upto]):
        m = re.match(r"call\s+([0-9a-f]+)(?: <(.*)>)?$", text)
        if not m or i + 1 >= len(body):
            continue

        target, name = int(m.group(1), 16), m.group(2) or ""
        nxt_addr, nxt_text = body[i + 1]

        thunk = re.search(r"__x86\.get_pc_thunk\.(\w+)$", name)

        if thunk:
            reg = "e" + thunk.group(1)
        elif target == nxt_addr and re.match(r"pop\s+(e[a-z]{2})$", nxt_text):
            reg = re.match(r"pop\s+(e[a-z]{2})$", nxt_text).group(1)
        else:
            continue

        value = nxt_addr

        for _, add in body[i + 1:upto]:
            m2 = re.match(rf"add\s+{reg},(0x[0-9a-f]+)$", add)
            if m2:
                value += int(m2.group(1), 16)
                break

        bases[reg] = value

    return bases


def indirect_call(text):
    """A call through memory or a register -- the dispatch, not a helper."""
    return bool(re.match(r"call\s+(?:\w+ PTR )?\[", text)
                or re.match(r"call\s+(?:[er][a-z]{2}|r\d+)$", text))


def bounds(body):
    """(first, last) indices of the timed window within a timed_call body.

    The window ends at the *indirect* call, not simply the first call: clang 22
    materializes the i386 PIC base with `call .+0; pop ebx` and puts it after
    the bracket, so stopping at the first call clipped the window to a single
    instruction (and left every global unresolved).
    """
    rd = next(i for i, (_, t) in enumerate(body) if re.match(r"rdtsc\b", t))
    start = rd + 2  # skip the lfence that closes the bracket
    end = next((i for i in range(start, len(body))
                if indirect_call(body[i][1])), None)
    return start, end


def window(body):
    """The instructions from after the start bracket through the call."""
    start, end = bounds(body)
    return body[start:end + 1]


def globals_read(win, bases, syms):
    """index -> (symbol, offset) for every instruction reading a global."""
    reads = {}

    for i, (addr, text) in enumerate(win):
        m = re.search(r"\[rip\+(0x[0-9a-f]+)\]", text)
        if m:
            # rip-relative displacements are from the *next* instruction.
            nxt = win[i + 1][0] if i + 1 < len(win) else addr
            target = nxt + int(m.group(1), 16)
        else:
            m = re.search(r"\[(e[a-z]{2})\+(0x[0-9a-f]+)\]", text)
            if not m or m.group(1) not in bases:
                continue
            target = bases[m.group(1)] + int(m.group(2), 16)

        got = resolve(syms, target)

        if got:
            reads[i] = got

    return reads


# eax and rax are the same value to a def-use pass; so are r8 and r8d.
def canon(reg):
    m = re.fullmatch(r"r(\d+)[dwb]?", reg)
    if m:
        return "r" + m.group(1)
    m = re.fullmatch(r"[re]?([a-d])x|([a-d])[hl]", reg)
    if m:
        return (m.group(1) or m.group(2)) + "x"
    m = re.fullmatch(r"[re]?(si|di|bp|sp)l?", reg)
    if m:
        return m.group(1)
    return reg


# Mnemonics whose first operand is read, not written.
READS_FIRST = {"push", "call", "cmp", "test", "jmp", "je", "jne", "ret"}


def family(sym):
    """Which object a global read belongs to.

    The family decides which names are even possible, so that the stride a
    two-dimensional method multiplies by (a field of the method's `fn`) is not
    confused with the hash multiplier (a field of the registry's state).
    """
    if "static_vptr" in sym:
        return "static_vptr"
    if "::fn" in sym:
        return "fn"
    if "registry_state" in sym or "::st" in sym:
        return "st"
    return "other"


def roles(win, reads, word=8):
    """index -> short operand name, inferred from how the value is used.

    A single forward pass: `live` maps a register to the instruction that last
    loaded it *from a global*, and any other write clears it. Building the map
    in a separate pass first would attribute a use to whichever load came last
    in the window rather than the one that reaches it -- with rax loaded from
    three different globals along the hash chain, that mislabels all three.

    Values that round-trip through the stack are tracked too: the i386 builds
    spill a slot and reload it into its use. Spill slots are keyed by their
    offset corrected for how far `esp` has moved since the window opened, so a
    `push` between the spill and the reload does not alias two slots together.
    """
    live = {}
    named = {}
    stack = {}
    delta = 0  # how far esp has been pushed down inside the window

    def name(idx, in_family, as_name):
        if idx is not None and family(reads[idx][0]) == in_family:
            named.setdefault(idx, as_name)

    for i, (_, text) in enumerate(win):
        mnem = text.split()[0]
        ops = text[len(mnem):].strip()

        # --- uses, resolved against the definitions that reach here ---

        # imul dst, src: whichever side is the global is the factor. Off the
        # registry state that is the hash multiplier; off the method's fn it
        # is the stride of a two-dimensional dispatch table.
        if mnem == "imul":
            idx = i if i in reads else live.get(canon(ops.split(",")[0]))
            name(idx, "st", "mult")
            name(idx, "fn", "stride")

        # shrx dst, src, count: the count's source is the hash shift.
        m = re.fullmatch(r"([a-z0-9]+),\s*[a-z0-9]+,\s*([a-z0-9]+)", ops)
        if mnem.startswith("shr") and m:
            name(live.get(canon(m.group(2))), "st", "shift")

        # [base + index*n]: the base is a table, the index a slot into it.
        m = re.search(
            r"\[([a-z0-9]+)\+([a-z0-9]+)\*[0-9](?:[+-]0x[0-9a-f]+)?\]", text)
        if m:
            name(live.get(canon(m.group(2))), "fn", "slot")
            name(live.get(canon(m.group(1))), "st", "vptrs")

        # --- spills and reloads, so a use reached through the stack still
        #     points at the load that produced the value ---
        spill = re.fullmatch(r"\w+PTR\[[er]sp\+(0x[0-9a-f]+)\],([a-z0-9]+)",
                             ops.replace(" ", ""))
        reload = re.fullmatch(r"([a-z0-9]+),\w+PTR\[[er]sp\+(0x[0-9a-f]+)\]",
                              ops.replace(" ", ""))

        if mnem == "mov" and spill:
            key = int(spill.group(1), 16) - delta
            src = canon(spill.group(2))
            stack[key] = live.get(src)

        # --- this instruction's own definition ---
        m = re.match(r"([a-z0-9]+)\s*(?:,|$)", ops)

        if m and mnem not in READS_FIRST and re.fullmatch(r"[a-z0-9]+",
                                                          m.group(1)):
            dst = canon(m.group(1))
            if i in reads and mnem in ("mov", "movzx", "movsx", "lea"):
                live[dst] = i
            elif mnem == "mov" and reload:
                came_from = stack.get(int(reload.group(2), 16) - delta)
                live[dst] = came_from
                if came_from is None:
                    live.pop(dst)
            else:
                live.pop(dst, None)

        # --- track esp, so spill keys stay comparable across a push ---
        m = re.fullmatch(r"esp,(0x[0-9a-f]+)", ops.replace(" ", ""))

        if mnem == "sub" and m:
            delta += int(m.group(1), 16)
        elif mnem == "add" and m:
            delta -= int(m.group(1), 16)
        elif mnem == "push":
            delta += word

    return named


def chain(win, word):
    """Dependent loads between the window opening and the call target.

    Instruction counts say little here -- the document's whole argument is
    about dependency chains -- so the summary counts the loads that must
    complete one after another before the indirect call can issue. A load's
    own depth is (deepest address operand) + 1; an independent load off a
    global is therefore 1 no matter how late it is scheduled. Read-modify-write
    forms keep their destination's incoming depth, three-operand forms
    (`shrx`, three-operand `imul`) do not.
    """
    REG = r"(?<![a-z0-9])((?:[re][a-z]{2})|r\d+[dwb]?)"
    MOVE = ("mov", "movzx", "movsx", "lea", "shrx", "shlx", "sarx")

    dep = {}
    stack = {}
    delta = 0

    def regs(text):
        return [canon(r) for r in re.findall(REG, text)]

    def deepest(rs):
        return max([dep.get(r, 0) for r in rs] or [0])

    for _, text in win:
        mnem = text.split()[0]
        ops = text[len(mnem):].strip()
        parts = [p.strip() for p in ops.split(",")] if ops else []

        mem = re.search(r"\[([^\]]+)\]", ops)
        stores = bool(re.match(r"\w+ PTR \[[^\]]+\],", ops))
        loaded = deepest(regs(mem.group(1))) + 1 if mem and not stores else None

        slot = re.search(r"\[[er]sp\+(0x[0-9a-f]+)\]", ops)
        key = int(slot.group(1), 16) - delta if slot else None

        if key is not None and not stores:
            # A slot this window spilled forwards the depth of what was put
            # there. A slot it never wrote is an incoming stack argument, and
            # reading it is an ordinary load -- scoring it 0 would make the
            # i386 builds' fat-pointer reads free while charging the identical
            # `[rsp+N]` read on the 64-bit builds (review finding).
            loaded = stack[key] if key in stack else loaded

        if mnem == "call" and indirect_call(text):
            # An indirect call through memory adds its own load; through a
            # register it just consumes what is already there.
            return loaded if mem else deepest(regs(ops))

        if mnem == "call":
            # A direct call is the i386 PIC-base idiom (`call .+0; pop`), not
            # the dispatch; the `pop` below is what carries its result.
            continue

        if mnem == "pop":
            # Reading the return address off the stack: a load, but one that
            # depends on nothing the dispatch computed.
            dep[canon(parts[0])] = 1
            continue

        if stores and key is not None:               # spill
            stack[key] = deepest(regs(parts[-1]))
        elif parts and re.fullmatch(REG.replace("(?<![a-z0-9])", ""),
                                    parts[0]) and mnem not in READS_FIRST:
            dst = canon(parts[0])
            srcs = deepest(regs(",".join(parts[1:]))) if len(parts) > 1 else 0
            keeps = mnem not in MOVE and len(parts) <= 2

            dep[dst] = max([srcs if not mem else 0,
                            loaded or 0,
                            dep.get(dst, 0) if keeps else 0])

        m = re.fullmatch(r"esp,(0x[0-9a-f]+)", ops.replace(" ", ""))

        if mnem == "sub" and m:
            delta += int(m.group(1), 16)
        elif mnem == "add" and m:
            delta -= int(m.group(1), 16)
        elif mnem == "push":
            delta += word

    return 0


def pic_setup(win):
    """Indices of the i386 PIC-base sequence, when it falls inside the window.

    `call .+0; pop reg; add reg, imm` really does execute between the brackets
    on clang 22 at -m32, so it stays in the listing even where the dispatch
    never reads a global and the slice would drop it as dead.
    """
    marks = set()

    for i, (_, text) in enumerate(win[:-1]):
        m = re.match(r"call\s+([0-9a-f]+)", text)

        if not m or int(m.group(1), 16) != win[i + 1][0]:
            continue

        pop = re.match(r"pop\s+(e[a-z]{2})$", win[i + 1][1])

        if not pop:
            continue

        marks |= {i, i + 1}

        for j in range(i + 2, len(win)):
            if re.match(rf"add\s+{pop.group(1)},0x[0-9a-f]+$", win[j][1]):
                marks.add(j)
                break

    return marks


def bookkeeping(win, word):
    """Indices of the instructions that are harness, not dispatch.

    A backward slice from the call: an instruction is kept when what it
    produces reaches the call's target or one of its arguments, and dropped
    when nothing in the window ever reads it again. That catches everything the
    timing rig leaves in the window and nothing else -- the start stamp being
    stitched together out of `edx:eax` (gcc shifts and ors, clang negates and
    subtracts), and the `cycles` out-parameter being parked in a callee-saved
    register, which clang 22 does after the bracket and clang 18 did before it.

    Stores are always kept: a store is either an argument the callee reads or a
    spill something reloads, and the slice cannot tell which.
    """
    REG = r"(?<![a-z0-9])((?:[re][a-z]{2})|r\d+[dwb]?|xmm\d+)"
    keep = {len(win) - 1} | pic_setup(win)
    needed = {canon(r) for r in re.findall(REG, win[-1][1])}

    for i in range(len(win) - 2, -1, -1):
        text = win[i][1]
        mnem = text.split()[0]
        ops = text[len(mnem):].strip()
        parts = [p.strip() for p in ops.split(",")] if ops else []

        stores = bool(re.match(r"\w+ PTR \[[^\]]+\],", ops))
        dst = None

        if parts and re.fullmatch(REG.replace("(?<![a-z0-9])", ""), parts[0]) \
                and mnem not in READS_FIRST:
            dst = canon(parts[0])

        wanted = stores or mnem in ("push", "call") or (dst in needed)

        if not wanted:
            continue

        keep.add(i)

        if dst is not None and dst in needed and mnem in ("mov", "movzx",
                                                          "movsx", "lea"):
            # A plain move redefines its destination; anything else (add, or,
            # imul) also reads it.
            needed.discard(dst)

        needed |= {canon(r) for r in re.findall(REG, ops if dst is None
                                                else ",".join(parts[1:]))}

        if dst is None:
            needed |= {canon(r) for r in re.findall(REG, ops)}

    return set(range(len(win))) - keep


def consistent(cells, build):
    """Name reads that usage could not reach, from the same field elsewhere.

    A field spilled to the stack in one window is loaded straight into its use
    in another, and the same (symbol, offset) always means the same thing
    within a build -- so one confident classification names all of them.
    """
    known = {}

    for win, reads, named in cells.values():
        for i, key in reads.items():
            if i in named:
                known.setdefault(key, named[i])

    unnamed = set()

    for win, reads, named in cells.values():
        for i, key in reads.items():
            if i in named:
                continue
            if key in known:
                named[i] = known[key]
            elif family(key[0]) == "static_vptr":
                named[i] = "static_vptr"
            else:
                named[i] = re.sub(r"[^A-Za-z_]", "",
                                  key[0].split("::")[-1])[:12] or "global"
                unnamed.add(f"{key[0].split('::')[-1]}+{key[1]:#x}")

    if unnamed:
        print(f"warning: {build}: unclassified global reads: "
              + ", ".join(sorted(unnamed)), file=sys.stderr)


def render(win, named, skip):
    """The window as text, globals rewritten to their roles.

    `skip` drops the start stamp's bookkeeping: it is not dispatch, the
    compilers interleave it differently, and it outnumbers the dispatch in the
    shortest windows.
    """
    out = []

    for i, (_, text) in enumerate(win):
        if i in skip:
            continue

        if i in named:
            text = re.sub(r"\[rip\+0x[0-9a-f]+\]", f"[rip+{named[i]}]", text)
            text = re.sub(r"\[(e[a-z]{2})\+0x[0-9a-f]+\]",
                          f"[got+{named[i]}]", text)
        # The i386 PIC base is materialized by calling the next instruction
        # and popping the return address; objdump renders that call's target
        # as the enclosing symbol, which is a mangled name a hundred columns
        # wide. Name the idiom instead.
        m = re.match(r"call\s+([0-9a-f]+)(?: <(.*)>)?$", text)

        if m and not indirect_call(text):
            nxt = win[i + 1][0] if i + 1 < len(win) else None
            thunk = int(m.group(1), 16) == nxt \
                or "__x86.get_pc_thunk" in (m.group(2) or "")
            text = "call pc_thunk" if thunk else f"call {m.group(2) or ''}"

        # objdump prints the resolved target after a '#'; the role name has
        # replaced it, and the raw address is noise in a four-way comparison.
        text = re.sub(r"\s*#.*$", "", text)
        text = text.replace(",", ", ")
        out.append(text)

    return out


def probe_window(funcs, syms, fn, registry, build, word):
    """The dispatch as compiled on its own, sliced and annotated the same way."""
    hits = [k for k in funcs
            # gcc names the TU's static-init function after its first symbol,
            # so _GLOBAL__sub_I__ZN3omb9probe_vf1... matches probe_vf1 too.
            if not k.startswith("_GLOBAL__sub_I")
            and re.search(rf"(?<![A-Za-z_]){fn}[<IE(]", k)
            and (registry is None
                 or re.search(rf"{registry}(?![a-z_])", k))]

    if len(hits) != 1:
        sys.exit(f"{build}: probe {fn}/{registry} matched {len(hits)} symbols, "
                 "expected 1 -- rebuild with ./probes.sh")

    body = funcs[hits[0]]
    # A tail call, through memory or through a register; a `jmp` to a plain
    # address is an ordinary branch and does not end the dispatch.
    end = next((i for i, (_, t) in enumerate(body)
                if indirect_call(t)
                or re.match(r"jmp\s+(?:\w+ PTR )?\[", t)
                or re.match(r"jmp\s+(?:[er][a-z]{2}|r\d+)$", t)), None)

    if end is None:
        sys.exit(f"{build}: probe {fn}/{registry} has no indirect tail call")

    win = body[:end + 1]

    # With no return value to keep, the compiler tail-calls: either
    # `jmp [mem]`, which shape() folds to `call [mem]`, or the entry loaded
    # into a register first and `jmp reg`. Fold the pair, so the probe is
    # compared in the form the measured code is forced into.
    m = re.match(r"jmp\s+([a-z0-9]+)$", win[-1][1])

    if m and len(win) > 1:
        prev = re.match(rf"mov\s+{m.group(1)},(\w+ PTR \[[^\]]+\])$",
                        win[-2][1])
        if prev:
            win = win[:-2] + [(win[-2][0], f"call {prev.group(1)}")]

    reads = globals_read(win, pic_base(body, end + 1), syms)
    named = roles(win, reads, word)
    return win, reads, named


def dispatch_ops(rendered):
    """The instructions that are the dispatch, as shapes.

    Reads of a global or of something the dispatch chased, plus the hash
    arithmetic. Stack traffic is excluded on both sides: in the timed window it
    is the harness spilling arguments, in the probe it is an ordinary frame,
    and neither is dispatch.
    """
    ops = []

    for text in rendered:
        mnem = text.split()[0]
        mem = re.search(r"\[([^\]]+)\]", text)
        stores = bool(re.match(r"\w+ PTR \[[^\]]+\],", text[len(mnem):]))

        if mem and not stores and not re.search(r"\[[er]sp", text):
            ops.append(shape(text))
        elif mnem in ("imul", "shrx", "shr"):
            ops.append(shape(text))

    return collections.Counter(ops)


def check_oracle(bindir, name, binary, cells, word):
    """Every instruction of the standalone dispatch must survive the slice.

    The slice keeps what reaches the call and drops the timing rig; the probe
    binary has no timing rig at all, so anything in the probe that is missing
    from the sliced window is the slice having eaten real dispatch.
    """
    path = f"{bindir}/{binary.replace("benchmark-", "probe-")}"

    if not os.path.exists(path):
        print(f"note: {path} absent -- skipping the oracle check "
              "(build it with ./probes.sh)", file=sys.stderr)
        return

    funcs = disassemble(path)
    syms = symbols(path)

    for (label, form, arity, struct, tag) in VARIANTS:
        pwin, preads, pnamed = probe_window(
            funcs, syms, *PROBES[(struct, tag)], name, word)
        pshapes = dispatch_ops(render(pwin, pnamed, set()))

        win, reads, named, skip = cells[(label, form, arity)]
        wshapes = dispatch_ops(render(win, named, skip))

        missing = pshapes - wshapes

        if missing:
            sys.exit(
                f"{name}: the slice of {row_label(label, form)} arity {arity} "
                f"is missing {sum(missing.values())} instruction(s) that the "
                f"standalone dispatch has: {', '.join(sorted(missing))}. "
                "Either bookkeeping() dropped real dispatch, or "
                "src/dispatch_probe.cpp has drifted from main.cpp.")


def extract(bindir, name, binary):
    bits = "32" if name.endswith("32") else "64"
    path = f"{bindir}/{binary}"
    funcs = disassemble(path)
    syms = symbols(path)
    cells = {}

    for label, form, arity, struct, tag in VARIANTS:
        body = find(funcs, struct, tag, name)

        if not any(re.match(r"rdtsc\b", t) for _, t in body):
            sys.exit(f"{name}: no rdtsc bracket in {struct}/{tag} -- is "
                     f"{path} an optimized build of the current sources?")

        start, end = bounds(body)

        if end is None:
            sys.exit(f"{name}: {struct}/{tag} has no indirect call after the "
                     "start bracket -- the dispatch was devirtualized?")

        win = body[start:end + 1]
        bases = pic_base(body, end + 1)
        reads = globals_read(win, bases, syms)
        cells[(label, form, arity)] = (win, reads,
                                      roles(win, reads, int(bits) // 8))

    consistent(cells, name)
    word = int(bits) // 8

    sliced = {k: (win, reads, named, bookkeeping(win, word))
              for k, (win, reads, named) in cells.items()}

    check_oracle(bindir, name, binary, sliced, word)

    return {k: (render(win, named, skip), chain(win, word), len(skip))
            for k, (win, reads, named, skip) in sliced.items()}


def row_label(label, form):
    return f"`{label}`" if not form else f"`om {form} / {label}`"


def prose(text, width=78):
    """Print a prose block, re-filled to the README's column.

    The figures below are interpolated, so the source cannot be wrapped by
    hand: a two-digit count where a one-digit one used to be would leave a
    ragged line in the document.
    """
    out = []

    for block in text.strip("\n").split("\n\n"):
        if block.startswith(("#", "|", "```")):
            out.append(block)
            continue

        # A run of bullets is one block: items are filled individually but
        # stay tight, the way the rest of the document writes lists.
        items = [textwrap.fill(
            " ".join(item.split()), width=width, initial_indent="",
            subsequent_indent="  " if item.startswith("- ") else "",
            break_long_words=False, break_on_hyphens=False)
            for item in re.split(r"\n(?=- )", block)]

        out.append("\n".join(items))

    print("\n\n".join(out) + "\n")


def insns(data, label, form, arity, build):
    return len(data[build][(label, form, arity)][0])


def depth(data, label, form, arity, build):
    return data[build][(label, form, arity)][1]


def uniform(data, label, form, arity, what="depth"):
    """The value every build agrees on, or a hard stop.

    The prose below states several claims as holding across all builds.
    Rather than print a claim that has quietly stopped being true -- a
    recompile with different flags is all it would take -- the generator
    refuses to emit it and says which row broke.
    """
    get = depth if what == "depth" else insns
    vals = {get(data, label, form, arity, n) for n, _ in BUILDS}

    if len(vals) != 1:
        sys.exit(f"asmtab: {row_label(label, form)} arity {arity} no "
                 f"longer has one {what} across the four builds ({sorted(vals)})"
                 " -- the prose in section_prose() claims it does; rewrite it.")

    return vals.pop()


def section_intro():
    prose("""## Every timed region, side by side

The listings above are hand-picked. `asmtab.py` takes the same window from every
binary and every dispatch: the instructions `timed_call<V>` executes between
the start bracket and the `call` into the body. Each variant is its own
`noinline` instantiation, so the window is a contiguous range inside a symbol
the script finds by name — nothing here is transcribed by hand, and
`python3 asmtab.py` regenerates this section from whatever is in `bin/`.

Operands that read a global are rewritten to the role the value plays —
`[rip+slot]`, `[rip+mult]` — inferred from what the value is *used for* rather
than from its symbol offset. The map registries are left out; their probe
inlines a container walk rather than the arithmetic the others do.

**What is shown is the dispatch, sliced out of the window.** Everything below
is the code that ran between the brackets, minus the instructions the timing
rig leaves there — identified by a backward slice from the call, so nothing
that reaches the call or its arguments can be dropped. What the slice removes
is the start stamp: `rdtsc` leaves the counter split across `edx:eax`, and each
compiler stitches the halves into something the epilogue can subtract, gcc
shifting and or-ing, clang negating and subtracting. It is scheduled freely and
lands anywhere in the window (per "Timing"), and left in it outnumbers the
dispatch — a virtual call is two instructions under five of harness. Because a
slice is a heuristic and compilers move things, `probes.sh` compiles the same
dispatches standalone and `asmtab.py` fails the run if any instruction of the
standalone dispatch is missing from the slice.

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
""")


def section_shows(data):
    ref1 = uniform(data, "vptr_vector", "ref", 1)
    ind1 = uniform(data, "indirect", "ref", 1)
    vf1 = uniform(data, "vf (yardstick)", "", 1)
    vp1 = uniform(data, "vptr", "", 1)
    ip1 = uniform(data, "inplace", "ref", 1)
    ipi1 = uniform(data, "inplace_ind", "ref", 1)
    vp2 = uniform(data, "vptr", "", 2)
    vpi2 = uniform(data, "vptr / indirect", "", 2)

    # Where the added indirection deepens the chain and where it hides.
    fatptr = ", ".join(
        f"{n} takes {insns(data, 'vptr', '', 1, n)}"
        for n, _ in BUILDS) + " instructions to dispatch through it"

    vpi = {n: depth(data, "vptr / indirect", "", 1, n) for n, _ in BUILDS}
    slack = [n for n, _ in BUILDS if vpi[n] == vp1]
    deeper = [n for n, _ in BUILDS if vpi[n] == vp1 + 1]

    if len(slack) + len(deeper) != len(BUILDS):
        sys.exit("asmtab: `om vptr / indirect` arity 1 depth is neither the base "
                 f"nor base+1 on some build ({vpi}) -- rewrite section_shows().")

    # The arity-1 story depends on which builds absorb the extra load; phrase it
    # for whatever the current build set does (a split, all-deeper, or all-hide).
    if deeper and slack:
        arity1 = (f"at arity 1 it deepens {' and '.join(deeper)}, which reaches "
                  f"the fat pointer through memory, and disappears on "
                  f"{' and '.join(slack)}, which keep it in a register")
    elif deeper:
        arity1 = (f"at arity 1 it deepens the chain on every build "
                  f"({vp1} → {vp1 + 1}), reaching the fat pointer through memory")
    else:
        arity1 = (f"at arity 1 it hides on every build — {vp1} loads deep either "
                  f"way — the fat pointer staying in a register, so the extra "
                  f"dereference runs beside the slot load")

    prose(f"""### What the windows show

- **The reference chain is the same depth on every build**: {ref1} dependent
  loads at arity 1 — receiver, `type_info`, vptr vector, open-method v-table —
  and {ind1} through `indirect`. What differs between the columns is
  register allocation and where the stamp bookkeeping lands, not the chain.
  That is the structural reason the ratios agree across builds.
- **A `virtual_ptr` call is {vp1} dependent loads, exactly what a virtual
  function call is** ({vf1}) — but not the same {vp1}. The virtual call must
  read the receiver before it can read its v-table; the `virtual_ptr` call
  already has the v-table and spends its chain on the slot and the v-table
  entry the call reads through it. Nothing has to find the receiver first,
  which is why the two come out level.
- **Where the compilers differ is what they do with the fat pointer**:
  {fatptr}. That costs nothing at arity 1 where the reload runs beside the
  slot load, but it is what makes the next bullet uneven.
- **`indirect_vptr` adds exactly one dependent load — where there is no slack
  to hide it.** Through a reference it deepens the chain on every build
  ({ref1} → {ind1}), and on the inplace hierarchy too ({ip1} → {ipi1}).
  Through a `virtual_ptr` {arity1}. At arity 2 the slack is gone and it costs a
  load everywhere ({vp2} → {vpi2}). That is the same unevenness the cycle tables show,
  where the policy costs a `virtual_ptr` a fraction of what it costs a
  reference.
- **At two virtual arguments the multi-method's two lookups are independent**:
  each virtual argument's v-table is fetched on its own chain, one `imul` by
  the table stride combines them, and a single indexed call ends it — depth
  {vp2} through a `virtual_ptr` against the idiom's two chained calls.
""")


def section_summary(data):
    print("| dispatch | arity | " + " | ".join(n for n, _ in BUILDS) + " |")
    print("|---|---|" + "---|" * len(BUILDS))

    for arity in (1, 2):
        for label, form, ar, _, _ in VARIANTS:
            if ar != arity:
                continue
            cells = [f"{len(t)} / {d}"
                     for n, _ in BUILDS
                     for t, d, _bk in [data[n][(label, form, ar)]]]
            print(f"| {row_label(label, form)} | {ar} | "
                  + " | ".join(cells) + " |")


def section_listings(data):
    for arity in (1, 2):
        for label, form, ar, _, _ in VARIANTS:
            if ar != arity:
                continue

            cols = [data[n][(label, form, ar)][0] for n, _ in BUILDS]
            width = max(len(t) for c in cols for t in c) + 2
            head = "".join(n.ljust(width) for n, _ in BUILDS).rstrip()

            print(f"`{row_label(label, form).strip('`')}`, arity {ar}:\n")
            print("```asm")
            print(head)
            print("".join("-" * (width - 2) + "  " for _ in BUILDS).rstrip())

            for i in range(max(len(c) for c in cols)):
                line = "".join((c[i] if i < len(c) else "").ljust(width)
                               for c in cols)
                print(line.rstrip())

            print("```\n")


def main():
    args = list(sys.argv[1:])
    section = args.pop(0) if args and args[0] in (
        "summary", "listings", "all") else "all"
    bindir = args.pop(0) if args else "bin"

    if args:
        sys.exit(f"unexpected argument {args[0]!r}")

    stale = [b for _, b in BUILDS
             if os.path.exists(f"{bindir}/{b}")
             and os.path.getmtime(f"{bindir}/{b}") < newest_source()]

    if stale:
        print(f"warning: {', '.join(stale)} older than src/ -- this section "
              "is generated from the binaries, so rebuild all four first",
              file=sys.stderr)

    data = {name: extract(bindir, name, binary) for name, binary in BUILDS}
    buf = io.StringIO()

    with redirect_stdout(buf):
        if section == "all":
            section_intro()

        if section in ("summary", "all"):
            section_summary(data)
            print()

        if section == "all":
            print("### The listings\n")

        if section in ("listings", "all"):
            section_listings(data)

        if section == "all":
            section_shows(data)

    sys.stdout.write(align_tables(buf.getvalue()))


if __name__ == "__main__":
    main()
