"""Write .cube colour grading tables for the Grade row in the video menu.

A .cube is what every colour grading tool exports, so the point of the feature
is that you make a grade somewhere that shows you the picture while you turn
the knobs -- Resolve, Lightroom, Photoshop, an online LUT editor -- and drop
the file into the grades folder. This script is for the case where you have not
made one yet: it writes a few, so the row has something in it and you can see
what a grade does before deciding to author your own.

    python3 make_grade.py <the grades folder>

The grades folder sits next to settings.ini:

    Windows   %LOCALAPPDATA%/MGAcid/grades
    Linux     ~/.local/share/MGAcid/grades

Size
----

17 entries a side, not 33. The tables here are smooth functions of the input
colour -- curves, a saturation scale, a tint that varies with brightness -- and
trilinear interpolation between 17 points reproduces a smooth function to well
under a display step. 33 would quadruple the file for no visible difference. A
grade with a hard edge in it, such as a key or a posterisation, does need 33,
which is why the reader accepts anything up to 64.

Honesty about what these are
----------------------------

These are arithmetic, not colour science: a curve, a saturation scale and a
tint, chosen by eye against screenshots. A grade made by a colourist looking at
the actual game is a better grade, and this script exists to make that
comparison possible rather than to stand in for it.
"""

import os
import sys

SIZE = 17

# Rec.709 luma. The same weights post.frag uses, so a grade written here and
# the built-in grade disagree about brightness in no way beyond what they
# actually do differently.
LUMA = (0.2126, 0.7152, 0.0722)


def clamp(value, low=0.0, high=1.0):
    return low if value < low else high if value > high else value


def luma(rgb):
    return sum(w * c for w, c in zip(LUMA, rgb))


def saturate(rgb, amount):
    """amount 1 leaves it alone, 0 is grey, above 1 is more colourful."""
    grey = luma(rgb)
    return tuple(grey + (c - grey) * amount for c in rgb)


def contrast(rgb, amount, pivot=0.435):
    """An S-curve about `pivot`, which is mid grey in display values.

    A straight multiply about the pivot clips: it takes highlights past 1 and
    shadows below 0 and flattens both into a block. This rolls off instead --
    the slope is `amount` at the pivot and falls to nothing at each end -- so
    the extremes keep their separation, which is the whole reason for wanting
    more contrast in the first place.
    """
    out = []
    for c in rgb:
        if c >= pivot:
            span = 1.0 - pivot
            t = (c - pivot) / span if span > 0 else 0.0
            # t -> t^(1/amount) steepens near 0 and flattens near 1.
            out.append(pivot + span * (t ** (1.0 / amount)))
        else:
            t = (pivot - c) / pivot if pivot > 0 else 0.0
            out.append(pivot - pivot * (t ** (1.0 / amount)))
    return tuple(out)


def split_tone(rgb, shadow, highlight):
    """Push shadows towards one colour and highlights towards another.

    This is the part of a grade that a 1D table cannot do and that makes a
    picture read as graded rather than as adjusted: the tint depends on how
    bright the pixel is, so the same red gets a different push in a dark corner
    than under a lamp.
    """
    weight = luma(rgb)
    tint = tuple(s + (h - s) * weight for s, h in zip(shadow, highlight))
    # Around a neutral 1.0, so a tint of (1, 1, 1) changes nothing.
    return tuple(c * t for c, t in zip(rgb, tint))


def lift_black(rgb, amount):
    """Raise the floor, the way film never reaches true black."""
    return tuple(amount + c * (1.0 - amount) for c in rgb)


GRADES = {
    "metal-gear-cold": {
        "title": "Metal Gear, cold",
        "note": "Cool shadows, slightly desaturated, a firm S-curve. The look the "
                "series settled into: a dark room with hard light in it.",
        "apply": lambda c: saturate(
            split_tone(contrast(c, 1.30), shadow=(0.92, 0.98, 1.10), highlight=(1.04, 1.01, 0.96)), 0.88),
    },
    "warm-film": {
        "title": "Warm film",
        "note": "Lifted blacks, warm highlights, gentle contrast. Softer and less "
                "clinical; closer to how the art looks on a PSP's own screen.",
        "apply": lambda c: saturate(
            split_tone(lift_black(contrast(c, 1.15), 0.035),
                       shadow=(1.03, 0.99, 0.96), highlight=(1.05, 1.01, 0.94)), 1.06),
    },
    "neutral-contrast": {
        "title": "Neutral contrast",
        "note": "Contrast and saturation only, no hue moved at all. What the "
                "built-in grade is trying to be, with the S-curve it does not have. "
                "Useful as the control: if a grade below looks better than this, "
                "the difference is its colour and not just its contrast.",
        "apply": lambda c: saturate(contrast(c, 1.22), 1.10),
    },
}


def write_cube(path, title, note, apply):
    last = float(SIZE - 1)
    with open(path, "w") as handle:
        handle.write('TITLE "%s"\n' % title)
        for line in note.split(". "):
            if line.strip():
                handle.write("# %s\n" % line.strip().rstrip("."))
        handle.write("# Written by make_grade.py. Edit it in a grading tool, or edit\n"
                     "# the numbers: they are plain text, red varying fastest.\n")
        handle.write("LUT_3D_SIZE %d\n" % SIZE)
        handle.write("DOMAIN_MIN 0.0 0.0 0.0\n")
        handle.write("DOMAIN_MAX 1.0 1.0 1.0\n")
        # Red fastest, then green, then blue: the order the format specifies.
        for b in range(SIZE):
            for g in range(SIZE):
                for r in range(SIZE):
                    colour = apply((r / last, g / last, b / last))
                    handle.write("%.6f %.6f %.6f\n" % tuple(clamp(c) for c in colour))


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[0])
        print("usage: make_grade.py <the grades folder>")
        return 2
    destination = argv[1]
    os.makedirs(destination, exist_ok=True)
    for name, grade in sorted(GRADES.items()):
        path = os.path.join(destination, name + ".cube")
        write_cube(path, grade["title"], grade["note"], grade["apply"])
        print("%-24s %6.1f KB   %s" % (name + ".cube", os.path.getsize(path) / 1024.0, grade["title"]))
    print("\n%d grades in %s" % (len(GRADES), destination))
    print("They appear in the Grade row in Video. Colour sets how far towards one the picture is taken,")
    print("so a grade does nothing while Colour is Off.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
