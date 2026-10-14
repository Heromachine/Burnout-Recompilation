# Burnout code-discovery seeds

`--seed-functions` is load-bearing, not optional. The linear sweep only finds
code reachable by falling through a section or by a statically visible
`call`/`jmp`. A function reachable **only as a data value** -- stored into a
function-pointer slot, built into a vtable at runtime, handed to the kernel as
a thread-start routine -- is invisible to it, and no amount of analysis inside
the disassembler will recover it.

On the Breakdown control, missing seeds did not produce a diagnostic. They
produced a build that did not boot at all, and separately a host stack overflow
that looked nothing like a seeding problem until it was root-caused. Assume the
same class of failure here.

## The two files, and why they stay separate

| File | Origin | Edited by |
|---|---|---|
| `seed-functions.json` | hand-maintained, one entry per address found the hard way | a human, with a written reason |
| `icall-feedback.json` | machine-generated from a run's measured indirect-branch targets | regenerated, never hand-edited |

A third file, `icall-db.json`, is the *cumulative database* behind the second.
`tools/recomp/icall_feedback.py` merges each run's `icall_targets.dump` into it
and emits `icall-feedback.json` from it. It is cumulative on purpose: a target
seen in an earlier run is never dropped because a later run did not reach it,
which is what makes repeated passes converge instead of oscillating.

```sh
# 1. run the title (it neither exits nor crashes, so ask it to dump)
XBOXRECOMP_ICALL_DUMP_AFTER=50 timeout 70 wine build-win/burnout_recomp.exe
# 2. merge into the database and re-emit the seed file
cd /mnt/dev/xboxrecomp-fork
.venv/bin/python -m tools.recomp.icall_feedback \
    --db /mnt/dev/Burnout-Launcher/config/seeds/icall-db.json \
    --functions /mnt/dev/Burnout-Launcher/build/analysis/disasm/functions.json \
    merge /mnt/dev/Burnout-Launcher/icall_targets.dump
# 3. ... then `seeds --align 0 --out .../icall-feedback.json`, and re-lift
```

`--align 0` is deliberate here. The default drops targets that are not
16-byte aligned, on the grounds that MSVC aligns function starts; three of
Burnout's four real gaps are NOT aligned (0x00124F6E, 0x00175344, 0x00175C29)
and the default would have silently discarded exactly the addresses worth
having. Each was checked against the retail bytes first: every one is preceded
by a `ret`/`ret n` or a data table and begins a plausible entry.

`--seed-functions` is repeatable -- pass it once per file -- so the two are
given to the disassembler together without being merged into each other. A
merged file loses the distinction between "we proved this by hand" and "a tool
guessed this from one run", which is exactly the distinction that matters when
a seed later turns out to be wrong.

## Format

```json
[
  {"start": "0x001A8460"}
]
```

## Rules

- **Every hand-added seed gets a comment in `regenerate.sh` saying what the
  address is, how it was found, and what breaks without it.** JSON has no
  comments; the script is where the reasoning lives, and a seed with no
  recorded reason is indistinguishable from a mistake.
- Seeds are cumulative and are not dropped because the current investigation
  ended. Each one was paid for once.
- Seeding can reveal the next seed: a newly reachable function makes its own
  indirect calls. Re-check the unresolved-ICALL log after every regeneration
  rather than assuming the list is complete.
- Do not seed into `DOLBY`. See `config/section-policy.json`: it is very likely
  DSP firmware rather than x86.
- A seed list whose only copy is in `/tmp`, a chat, or a HeroLab note is not a
  reproducible pipeline. It lives here.

## Current state

`seed-functions.json` is **empty**. Burnout has not been lifted yet, so no
data-only entry point has been discovered. The first lift is expected to need
seeds -- Burnout is a RenderWare/Criterion title with vtable-dispatched audio
in its `DSOUND` section, the same shape that required nine seeds on Breakdown.
An empty list is the honest starting state, not evidence that none are needed.
