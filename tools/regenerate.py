#!/usr/bin/env python3
"""Regenerate the recompiled C for Burnout from the user's own disc image.

Nothing this produces is redistributable: src/recomp/gen/ and build/ are
derived from the game and are gitignored. Every user runs this against their
own copy.

Python rather than shell so the Windows launcher can run it too (through
tools/build_from_iso.py, with the embeddable Python it ships). regenerate.sh
is a thin wrapper around this file for development: it checks the pinned
inputs with tools/verify-inputs.sh, then runs this with the toolkit's venv.

Two things differ deliberately from the Breakdown control's pipeline, and
both are intentional -- do not "simplify" them back:

  1. Every intermediate goes to build/analysis/ INSIDE this project. The
     toolkit's stages default to writing under the toolkit (tools/disasm/
     output, tools/func_id/output, tools/abi_analysis/output), which would
     mutate the shared toolchain worktree and let one title's analysis be read
     by another's build. Each stage is given explicit input and output
     directories instead.

  2. Seeds come from tracked files under config/seeds/, not from a list in
     this file. --seed-functions is repeatable, so the hand-maintained list
     and the machine-generated ICALL feedback are passed as separate files and
     never merged. See config/seeds/README.md.

Usage:  python tools/regenerate.py [--xbe PATH] [--toolkit DIR] [--no-fixes]
        then rebuild in build-win/
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(PROJECT, "config", "source-manifest.json")

# --------------------------------------------------------------------------
# Seeds: entry points reachable ONLY as data. They live in
# config/seeds/seed-functions.json (hand-maintained) and
# config/seeds/icall-feedback.json (measured); this is their history.
#
# 2026-09-20: 17 addresses seeded, all found the same way and all verified as
# genuine function starts before being added.
#
# HOW THEY WERE FOUND: the first lift left 31 addresses in
# src/recomp/gen/recomp_stubs_unresolved.c -- called by translated code but not
# detected as functions, so each became a stub that returns immediately. Each
# of the 31 was checked against the retail bytes; 17 begin with a standard
# 55 8b ec prologue and sit in a gap the linear sweep stepped over (the bytes
# before 0x0002AD2C, for instance, are a data table, which is where the sweep
# stopped). The other 14 really are mid-function entry points or data and are
# deliberately NOT seeded.
#
# WHAT BREAKS WITHOUT 0x0002AD2C, the one that was chased down: it implements
# command 0 of the module-system object handler sub_0002A9E0 (jump table at
# 0x0002AC68, index 0). Stubbed, it returns without doing anything, so
# sub_0001BF0B returns 0, so sub_0001BE44 returns 0, so sub_0007E242 aborts at
# 0x0007E327 and never reaches the module-system start at 0x0007E379. No module
# init ever runs, and the title later walks an uninitialised list forever.
# The failure surfaced as a boot hang four call levels away from this address.
# See HeroLab tasks d91b05b9, 4c781665, 189291d1.
#
# Precedent from the Breakdown control, for the other shapes to expect:
#   - a worker-thread start routine handed to PsCreateSystemThreadEx;
#   - the CRT's InitializeCriticalSectionAndSpinCount fallback, whose
#     unresolved indirect call reads as a failed lock init and tears down the
#     CRT's whole lock table;
#   - vtable-dispatched DSOUND methods with zero xrefs of any kind.
# Burnout links the same XDK 4361 DSOUND and has that section marked
# executable, so expect the third shape in particular.
#
# 2026-10-07: three more, each verified in Ghidra (/mnt/dev/Burnout-Ghidra) and
# against the retail bytes. They took boot from 120 file opens to 322, and to
# the first rendered city and cars, with 0 ICALL failures (HeroLab ab9e7247).
#   0x00063446  CRT _msize (SEH prologue: push 0x10; push 0x17E8B0; call
#               __SEH_prolog). Its only caller is the atexit-table growth in
#               sub_0005F847. Stubbed, it returned whatever was in eax, so the
#               table was realloc'd to size 0, which frees it and returns NULL.
#               The fallback realloc then got the freed block back, and the next
#               atexit registration freed it again: a double free. The corrupt
#               free list then handed out the same 0x7C block forever, a list
#               node linked to itself, and loading spun after
#               d:\trafcars\models\coma\coma.dff.
#   0x0006EE7E,  callbacks referenced only as DATA (stored at 0x0006F59B in
#   0x0007036C   sub_0006F51A and at 0x00070D17 in sub_00070C9A), each right
#               after the previous function's ret, with an aligned-stack
#               prologue. These were the only two [ICALL] failures once
#               0x00063446 was seeded.
# Still stubbed, and judged NOT to be function starts (mid-function entries or
# garbage decodes): 0x00063526, 0x00063529, 0x000639C3, 0x000639DE,
# 0x00078BEB, 0x00085942, 0x00085953, 0x024DA047. Still stubbed but they DO look
# like real starts, not yet seeded because no failure has been traced to them:
# 0x00064307, 0x00068372, 0x000B1F73, 0x00147AF8, 0x001501BA.
#
# Seeding an address the sweep stepped over needs xboxrecomp commit c70542c or
# newer; before it, such a seed silently did nothing.
# --------------------------------------------------------------------------
SEED_FILES = [os.path.join(PROJECT, "config", "seeds", "seed-functions.json"),
              os.path.join(PROJECT, "config", "seeds", "icall-feedback.json")]


# Runs `python -m <module>` with the toolkit on sys.path, written out because
# `-m` alone resolves modules against the working directory, and the embedded
# Python the Windows launcher ships ignores that (its ._pth file fixes
# sys.path, as Python's embeddable distribution is meant to).
_RUN_MODULE = ("import runpy, sys; sys.path.insert(0, sys.argv[1]); "
               "sys.argv = [sys.argv[2]] + sys.argv[3:]; "
               "runpy.run_module(sys.argv[0], run_name='__main__', alter_sys=True)")


def module_cmd(toolkit, module, *args):
    return [sys.executable, "-c", _RUN_MODULE, toolkit, module, *args]


def step(title, cmd, toolkit):
    print(f"==> {title}", flush=True)
    subprocess.run(cmd, cwd=toolkit, check=True)


def manifest():
    with open(MANIFEST) as f:
        return json.load(f)


def check_xbe(xbe, want):
    """Refuse an executable that is not the one this project was built
    against. A lift from a different XBE (another region, a revision) matches
    none of the seeds, spin fixes or evidence here, and would fail in ways
    that look like bugs."""
    h = hashlib.sha256()
    with open(xbe, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != want["sha256"]:
        sys.exit(f"{xbe} is not the {want['title']} ({want['region_media']}) retail executable "
                 f"this project supports (title ID {want['title_id']}).\n"
                 f"  sha256 {h.hexdigest()}\n  want   {want['sha256']}")


def warn_if_toolkit_moved(toolkit, want):
    """Warn (never fail) if the toolkit is not the pinned revision.

    The lift uses the toolkit's working tree, so uncommitted changes count too.
    Silent when there is no git metadata, as in the plain toolkit copy a
    player's machine builds from."""
    if not os.path.exists(os.path.join(toolkit, ".git")):
        return
    git = ["git", "-c", "safe.directory=*", "-C", toolkit]
    try:
        got = subprocess.run(git + ["rev-parse", "HEAD"], check=True,
                             capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(git + ["status", "--porcelain", "--untracked-files=no"],
                               check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return
    if got != want:
        print(f"WARNING: the toolkit is not the revision Burnout is pinned to\n"
              f"  pinned  {want} (config/source-manifest.json)\n  current {got}", flush=True)
    if dirty:
        print("WARNING: the toolkit has uncommitted changes, and this lift uses them:\n"
              + dirty, flush=True)


def main():
    m = manifest()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--xbe", default=os.path.join(PROJECT, "game_files", "default.xbe"))
    ap.add_argument("--toolkit", default=os.environ.get("XBOXRECOMP",
                                                        m["toolchain"]["xboxrecomp"]["path"]))
    ap.add_argument("--no-fixes", action="store_true",
                    help="skip apply_spin_fixes.py (the caller runs it as its own step)")
    args = ap.parse_args()
    xbe = os.path.abspath(args.xbe)
    toolkit = os.path.abspath(args.toolkit)
    if not os.path.isfile(xbe):
        sys.exit(f"missing {xbe} -- supply your own disc image")
    if not os.path.isdir(os.path.join(toolkit, "tools")):
        sys.exit(f"missing the xboxrecomp toolkit at {toolkit}")
    check_xbe(xbe, m["executable"])
    warn_if_toolkit_moved(toolkit, m["toolchain"]["xboxrecomp"]["commit"])

    analysis = os.path.join(PROJECT, "build", "analysis")
    xbe_analysis = os.path.join(analysis, "xbe-analysis.json")
    disasm = os.path.join(analysis, "disasm")
    func_id = os.path.join(analysis, "func_id")
    abi = os.path.join(analysis, "abi")
    recomp = os.path.join(analysis, "recomp")
    gen = os.path.join(PROJECT, "src", "recomp", "gen")
    manual = os.path.join(PROJECT, "src", "recomp_manual.c")
    for d in (disasm, func_id, abi, recomp):
        os.makedirs(d, exist_ok=True)

    seeds = []
    for f in SEED_FILES:
        if os.path.isfile(f):
            seeds += ["--seed-functions", f]
    # --exclude-manual keeps a regeneration from overwriting hand-written
    # overrides in recomp_manual.c with the generated version of the same
    # function. Pointing it at a missing path would be worse than omitting it.
    manual_args = ["--exclude-manual", manual] if os.path.isfile(manual) else []

    def mod(module, *rest):
        return module_cmd(toolkit, module, *rest)

    # The Breakdown control has no parser step because its analysis JSON was
    # produced once by hand and left beside the XBE; that is not reproducible.
    # tools.disasm looks for "<xbe stem>_analysis.json" next to the executable
    # unless told otherwise, so the path is passed explicitly and the JSON is
    # regenerated every run.
    step("parsing the XBE", mod("tools.xbe_parser", xbe, "--json", xbe_analysis), toolkit)
    # NOT --text-only. Burnout statically links seven XDK 4361 libraries into
    # their own executable sections; --text-only would drop D3D, D3DX, XGRPH,
    # DSOUND, XPP and DOLBY together. The default sweep already covers all of
    # them, so no --extra-sections is needed (unlike Breakdown, whose DSOUND is
    # not marked executable). Authoritative list: config/section-policy.json.
    step("disassembling", mod("tools.disasm", xbe, "-o", disasm, "--analysis-json", xbe_analysis,
                              "--force", "-v", *seeds), toolkit)
    step("identifying library functions",
         mod("tools.func_id", xbe, "-o", func_id,
             "--functions", os.path.join(disasm, "functions.json"),
             "--strings", os.path.join(disasm, "strings.json"),
             "--xrefs", os.path.join(disasm, "xrefs.json"), "-v"), toolkit)
    step("analysing calling conventions",
         mod("tools.abi_analysis", xbe, "--disasm-dir", disasm, "--func-id-dir", func_id,
             "--output-dir", abi, "-v"), toolkit)
    # Stale chunk files from a previous split count or layout produce a build
    # with mismatched dispatch entries, so the gen dir is cleared every time.
    if os.path.exists(gen):
        shutil.rmtree(gen)
    # -o is separate from --gen-dir and is easy to miss: --gen-dir places the
    # generated C, while -o places summary.json. Its default is inside the
    # toolkit, so omitting it overwrites whatever title ran last -- observed on
    # the first Burnout lift, which clobbered Breakdown's summary.
    step("lifting to C", mod("tools.recomp", xbe, "--all", "--split", "1000", "--gen-dir", gen,
                             "-o", recomp, "--disasm-dir", disasm, "--func-id-dir", func_id,
                             "--abi-dir", abi, *manual_args), toolkit)

    # The lift just wiped every spin fix. Re-applied here, not as a separate
    # step to remember: on Breakdown, forgetting it looked exactly like a fresh
    # regression, twice. The launcher's build runs it as its own stage.
    if not args.no_fixes:
        print("==> applying spin fixes", flush=True)
        subprocess.run([sys.executable, os.path.join(PROJECT, "tools", "apply_spin_fixes.py")],
                       cwd=PROJECT, check=True)
    print(f"==> done; now rebuild in {os.path.join(PROJECT, 'build-win')}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
