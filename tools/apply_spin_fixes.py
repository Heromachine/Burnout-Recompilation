#!/usr/bin/env python3
"""Reapply Burnout's spin-wait fixes to freshly lifted src/recomp/gen/*.c.

Each fix makes a wait resolve that our emulation can never satisfy, because
the hardware that would clear the condition is not modelled. They are
temporary, title-specific stand-ins (the same mechanism as Breakdown's
apply_spin_fixes.py) until the engine models the real behaviour.

A lift wipes src/recomp/gen, so tools/regenerate.py runs this as its last
step (the launcher's build runs it as its own stage). Run it by hand only after
lifting some other way:

    python3 -I tools/apply_spin_fixes.py

--- Why fixes are anchored by ADDRESS ---
Matching excerpts of the generated C would make this file a derivative of the
game's code, which .gitignore keeps out of git. A guest VA is a fact about the
binary, and the inserted statement is ours. Generated labels are
`loc_<VA>: ;` on a line of their own, so the anchor is exact.

--- Why every chunk, and every occurrence ---
Chunk split points move whenever the lift gains functions, and an alias entry
(a second way into a function, lifted as its own body) repeats the code after
it, labels included. The wait is the same wait in every copy.
"""
import glob
import sys

GEN = "src/recomp/gen"

# (label VA, generated statements to skip after the label, statement, why)
FIXES = [
    # DSOUND sub_0015AD38, the XDK effects-image download (d:\audio\bin\
    # dsstdfx.bin). It copies the image, writes 3 to the GP DSP command
    # mailbox at base+0x810 (0x83794810 in a measured run, contiguous RAM),
    # then spins until the DSP firmware clears it. Our APU never runs that
    # firmware handshake, so boot stopped here after 17 file opens. With the
    # ack: 120 opens, 0 ICALL/FATAL, audio at 48 kHz (HeroLab 5be8b8f4,
    # research/artifacts/phase2/stall-2026-10-07). Breakdown's 0x001D41A3 fix
    # forces the same +0x810 word in its copy of this library. The real fix is
    # for the APU model to acknowledge the mailbox.
    (0x0015AE9F, 0, "MEM32(ebx) = 0;",
     "no APU emulation ever clears the GP DSP mailbox (+0x810); simulate the DSP's acknowledge"),
    # DSOUND sub_0015AED4, the GP DSP command submit (Breakdown's sub_001D4175,
    # same shape): base = [[[ecx+8]+0x10]]+0x800, then it spins until
    # base+0x10 (the same +0x810 mailbox) is 0 before writing the next
    # command. With only the entry above, boot reached the first rendered city
    # and cars and then froze here: a still frame, no sound (HeroLab 78c3c1ba).
    # Breakdown's 0x001D41A3 is this exact fix.
    (0x0015AF02, 0, "MEM32(eax + 0x10) = 0;",
     "no APU emulation ever clears the GP DSP mailbox (+0x810) before the next command; simulate the DSP's acknowledge"),
]


def fix_line(va, stmt, why):
    """The exact inserted line. It carries the VA, so it identifies itself in
    a diff and doubles as the idempotency check."""
    return f"    {stmt} /* TEMP FIX 0x{va:08X}: {why} */"


def apply_all():
    applied = already = 0
    problems = []
    occurrences = {va: 0 for va, _skip, _stmt, _why in FIXES}

    paths = sorted(glob.glob(f"{GEN}/recomp_*.c"))
    if not paths:
        problems.append(f"no generated chunks under {GEN}")

    for path in paths:
        with open(path) as f:
            lines = f.read().split("\n")
        located = []
        for va, skip, stmt, why in FIXES:
            want = fix_line(va, stmt, why)
            label = f"loc_{va:08X}: ;"
            for hit in (i for i, l in enumerate(lines) if l.rstrip() == label):
                occurrences[va] += 1
                idx = hit + 1 + skip
                if idx < len(lines) and lines[idx] == want:
                    already += 1
                    continue
                if idx > len(lines):
                    problems.append(f"{path}: insertion point for 0x{va:08X} past end of file")
                    continue
                located.append((idx, want))
        # Bottom up, so an insertion cannot shift the ones still to come.
        for idx, want in sorted(located, reverse=True):
            lines.insert(idx, want)
            applied += 1
        if located:
            with open(path, "w") as f:
                f.write("\n".join(lines))

    for va, count in occurrences.items():
        if count == 0:
            problems.append(f"label loc_{va:08X} not found in any chunk")

    print(f"applied {applied} fixes, {already} already present, "
          f"{len(FIXES)} fixes at {sum(occurrences.values())} site(s), {len(problems)} problem(s)")
    for p in problems:
        print(f"  PROBLEM: {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(apply_all())
