#!/usr/bin/env python3
"""Find where one of the game's own settings lives in guest memory.

    find_setting.py <dump>=<value> <dump>=<value> ...        values known
    find_setting.py --changed <dump> <dump> [<dump> ...]     values unknown

The port writes a dump of the guest's memory when F7 is pressed, to
<data>/ram/ram_N.bin. Change a setting between dumps and the handful of bytes
that followed it are the ones that differ.

Known values is much the faster route. Ac!d's Options screen offers three
settings whose values you can read off the screen, so:

    1. Set Text Speed to its slowest, press F7
    2. Set it to its fastest, press F7
    3. Set it back to its slowest, press F7
    4. find_setting.py ram_1.bin=0 ram_2.bin=3 ram_3.bin=0

Going back to the first value in the third dump is what does the work: a
variable that merely changed could be anything the game touched, while one that
went there, away, and back again is almost certainly the one. Three dumps
usually leave a single address.

If the scale on screen does not say what the stored number is -- a slider with
no figures -- use --changed, which asks only that the value differed when the
setting differed and matched when it matched. It needs a dump or two more.

Addresses are printed as the guest sees them, which is what a patch needs.
"""
import sys
import numpy as np

BASE = 0x08000000


def load(path):
    return np.fromfile(path, dtype=np.uint8)


def view(data, width):
    if width == 1:
        return data
    usable = (len(data) // width) * width
    kind = {2: np.uint16, 4: np.uint32}[width]
    return data[:usable].view(kind)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = {a for a in argv[1:] if a.startswith("--")}
    if not args:
        print(__doc__.strip())
        return 2
    widths = [1, 2, 4]
    if "--byte" in flags: widths = [1]

    if "--changed" in flags:
        dumps = [load(p) for p in args]
        if len(dumps) < 2:
            print("--changed needs at least two dumps"); return 2
        for width in widths:
            views = [view(d, width) for d in dumps]
            size = min(len(v) for v in views)
            # Differed from the first dump at least once, and came back to it
            # at least once: a value that only ever drifted is not a setting.
            differed = np.zeros(size, dtype=bool)
            returned = np.zeros(size, dtype=bool)
            for v in views[1:]:
                same = views[0][:size] == v[:size]
                differed |= ~same
                returned |= same
            hits = np.nonzero(differed & returned)[0]
            print(f"  {width}-byte: {len(hits)} candidates")
            for i in hits[:20]:
                vals = [int(v[i]) for v in views]
                print(f"    0x{BASE + i * width:08x}  {vals}")
        return 0

    pairs = []
    for a in args:
        if "=" not in a:
            print(f"expected <dump>=<value>, got {a}"); return 2
        path, _, value = a.rpartition("=")
        pairs.append((load(path), int(value, 0)))
    if len(pairs) < 2:
        print("give at least two dumps"); return 2

    for width in widths:
        views = [(view(d, width), v) for d, v in pairs]
        size = min(len(v) for v, _ in views)
        keep = np.ones(size, dtype=bool)
        for data, wanted in views:
            keep &= data[:size] == wanted
        hits = np.nonzero(keep)[0]
        print(f"  {width}-byte: {len(hits)} address(es) hold "
              f"{[v for _, v in pairs]} in order")
        for i in hits[:20]:
            print(f"    0x{BASE + i * width:08x}")
        if len(hits) > 20:
            print(f"    ... and {len(hits) - 20} more; another dump will narrow it")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
