#!/usr/bin/env python3
"""Aggregate the Switch sampling profiler's full dumps per function.

The runtime prints every sampled PC bin when started with
PSX_SWITCH_PROFILE=2 (runtime/src/switch_platform.c):

    psxrecomp: [profile-dump-begin] <total>
    P <image offset hex> <count>
    ...
    psxrecomp: [profile-dump-end]

This script maps the offsets to functions with `nm` on the matching .elf
and prints the hottest functions plus a breakdown by area.

    python3 tools/switch_profile_report.py switch_log.txt build-switch/<exe>.elf \
        [--last N] [--nm $DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm]

--last N uses only the last N dumps (default 6, i.e. the last minute at the
default 10 s interval) so boot and menus do not dilute in-game numbers.
"""
import argparse
import bisect
import collections
import os
import re
import subprocess
import sys

AREAS = [
    ("game code (recompiled)", re.compile(r"^func_[0-9A-Fa-f]{8}")),
    ("BIOS (recompiled)", re.compile(r"^OpenBIOS_|^SCPH\d+_")),
    ("cycle/timing accounting", re.compile(r"^psx_cyc_|^psx_advance_cycles|^psx_icache_|"
                                           r"_cycles_to_|^timer_|^timers_|^psx_check_interrupts")),
    ("interpreter (dirty RAM)", re.compile(r"dirty_ram|^exec_one|^interp_|^fetch_word")),
    ("memory access", re.compile(r"^psx_(read|write)_|^mem_")),
    ("GPU emulation", re.compile(r"^gpu_|^gp0_|^gp1_|^psx_ws_|^ws_")),
    ("GTE / PGXP", re.compile(r"^gte_|^psx_pgxp|^pgxp")),
    ("SPU / CD-ROM / DMA", re.compile(r"^spu_|^rev_|^cdrom|^present_cdrom|^dma_")),
    ("libc / kernel (memcmp, svc, mutex)", re.compile(r"^mem(cmp|cpy|set|move)$|^svc|^mutex|"
                                                      r"^_LoadExclusive|^_free_r|^_malloc_r")),
]


def load_symbols(nm, elf):
    out = subprocess.run([nm, "-n", "-C", "--defined-only", elf],
                         check=True, capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 3 or parts[1] not in "TtWw":
            continue
        addrs.append(int(parts[0], 16))
        names.append(parts[2])
    return addrs, names


def parse_dumps(path):
    """Returns a list of (pc Counter, (pc, lr) Counter) per dump."""
    dumps, cur, pairs = [], None, None
    with open(path, errors="replace") as fh:
        for line in fh:
            if "[profile-dump-begin]" in line:
                cur, pairs = collections.Counter(), collections.Counter()
            elif "[profile-dump-end]" in line:
                if cur is not None:
                    dumps.append((cur, pairs))
                cur = None
            elif cur is not None and line.startswith("P "):
                _, off, cnt = line.split()
                cur[int(off, 16)] += int(cnt)
            elif cur is not None and line.startswith("C "):
                _, pc, lr, cnt = line.split()
                pairs[(int(pc, 16), int(lr, 16))] += int(cnt)
    return dumps


def area_of(name):
    for label, rx in AREAS:
        if rx.search(name):
            return label
    return "other runtime"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("elf")
    ap.add_argument("--last", type=int, default=6)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--callers-of", type=int, default=12,
                    help="show callers for the N hottest functions")
    default_nm = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"),
                              "devkitA64/bin/aarch64-none-elf-nm")
    ap.add_argument("--nm", default=default_nm)
    args = ap.parse_args()

    dumps = parse_dumps(args.log)
    if not dumps:
        sys.exit("no [profile-dump-begin] blocks found (start with PSX_SWITCH_PROFILE=2)")
    used = dumps[-args.last:]
    addrs, names = load_symbols(args.nm, args.elf)

    def fn(off):
        i = bisect.bisect_right(addrs, off) - 1
        return names[i] if i >= 0 else "?"

    per_fn, per_area = collections.Counter(), collections.Counter()
    callers = collections.defaultdict(collections.Counter)
    total = 0
    for dump, pairs in used:
        for off, cnt in dump.items():
            name = fn(off)
            per_fn[name] += cnt
            per_area[area_of(name)] += cnt
            total += cnt
        for (pc, lr), cnt in pairs.items():
            callers[fn(pc)][fn(lr)] += cnt

    print(f"{total} samples from the last {len(used)} of {len(dumps)} dumps\n")
    print("By area:")
    for label, cnt in per_area.most_common():
        print(f"  {100.0 * cnt / total:5.1f}%  {label}")
    print(f"\nTop {args.top} functions:")
    for name, cnt in per_fn.most_common(args.top):
        print(f"  {100.0 * cnt / total:5.1f}%  {name}")
    if callers:
        print("\nCallers (from lr; exact for leaf functions such as memcmp/svc*):")
        for name, cnt in per_fn.most_common(args.callers_of):
            c = callers.get(name)
            if not c:
                continue
            print(f"  {100.0 * cnt / total:5.1f}%  {name}")
            sub = sum(c.values())
            for caller, k in c.most_common(5):
                print(f"           {100.0 * k / sub:5.1f}% <- {caller}")


if __name__ == "__main__":
    main()
