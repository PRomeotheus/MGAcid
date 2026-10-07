#!/usr/bin/env python3
"""Take text and interface art out of a replacement texture pack.

    prune_pack.py <pack dir> <archive|dir>...            # what it would move
    prune_pack.py --apply <pack dir> <archive|dir>...     # move it
    prune_pack.py --names <pack dir> <archive|dir>...     # just list names

Why text does not want a replacement
------------------------------------
A pack entry overrides the renderer's own upscaling completely. That is the
point of it for a character skin, where detail really is missing. For text it
is the wrong way round: the engine's Sharp scaler leaves a glyph's edges exactly
where they were -- measured at zero colour fringing and even stroke widths on
both hard and anti-aliased glyphs -- while any upscaler that invents
intermediate values changes stroke weight, which is what makes enlarged text
look wrong. Removing the pack entry is therefore not a loss of sharpness. It
hands the texture back to the one path that does not reshape letters.

Why by name rather than by looking at the picture
------------------------------------------------
Guessing from pixels does not work well enough. Counting colours while ignoring
alpha, counting only visible colours, and measuring thin strokes were all tried
against the game's archives: each one moved a handful of textures out of several
hundred, because the game's text is drawn with gradients and outlines and has
as many colours as its artwork does.

The archives, though, carry the names the artists used. qar.py reports them
alongside the content key the pack is filed under, so a name is an exact
statement about what a texture is, where a pixel statistic is a guess. Anything
this moves can be moved back: it goes to a holding folder, it is not deleted.
"""
import argparse, importlib.util, os, pathlib, re, shutil, sys

HERE = pathlib.Path(__file__).resolve().parent

# Names Ac!d uses for lettering and interface furniture. Deliberately narrow:
# a texture wrongly kept in the pack looks as it does today, while one wrongly
# removed loses detail it could have had.
TEXT_NAMES = re.compile(
    r"font|text|msg|moji|word|letter|caption|subtit|dsc|help|tuto|"
    r"gui|menu|window|button|cursor|icon|logo|title|rule|frame",
    re.I)


def load_qar():
    spec = importlib.util.spec_from_file_location("qar", HERE / "qar.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pack", help="the folder of <key>.png replacements")
    parser.add_argument("archives", nargs="+", help=".qar/_zar archives, or folders of them")
    parser.add_argument("--apply", action="store_true", help="actually move the files")
    parser.add_argument("--names", action="store_true", help="list matching names and stop")
    parser.add_argument("--pattern", help="override the name pattern")
    parser.add_argument("--by-art", action="store_true",
                        help="select by what the texture is rather than by its name: "
                             "anything the engine now enlarges exactly (see below)")
    parser.add_argument("--into", default="_text_originals",
                        help="holding folder, made inside the pack folder")
    args = parser.parse_args(argv)

    pattern = re.compile(args.pattern, re.I) if args.pattern else TEXT_NAMES
    qar = load_qar()

    def drawn_by_hand(texture, data):
        """The same test common/texture_scale.cpp applies at run time.

        Colours among the texels that can be seen, alpha disregarded. When this
        is true the engine reproduces the texture exactly at a larger size, so a
        pack entry can only differ from the original -- never improve on it.
        """
        colours = set()
        for i in range(0, len(data), 4):
            if data[i + 3] <= 16:
                continue
            colours.add(data[i] | (data[i + 1] << 8) | (data[i + 2] << 16))
            if len(colours) > 12:
                return False
        return len(colours) > 0

    # key -> the names it is known by. One key can cover several appearances.
    names = {}
    by_art = set()
    for path, label, blob in qar.walk(args.archives):
        try:
            found = qar.chunks(blob)
        except ValueError as problem:
            print(f"{label}: {problem}", file=sys.stderr)
            continue
        for chunk in found:
            for texture in chunk.textures:
                name = qar.stem(chunk.name)
                try:
                    data = qar.rgba(texture)
                except Exception:
                    continue
                key = qar.content_key(texture.width, texture.height, data)
                names.setdefault(key, set()).add(name)
                if args.by_art and drawn_by_hand(texture, data):
                    by_art.add(key)

    if args.by_art:
        matched = {key: sorted(found) for key, found in names.items() if key in by_art}
        print(f"{len(names)} distinct textures in the archives; "
              f"{len(matched)} are drawn from few enough colours that the engine "
              f"reproduces them exactly")
    else:
        matched = {key: sorted(found) for key, found in names.items()
                   if any(pattern.search(name) for name in found)}
        print(f"{len(names)} distinct textures in the archives; "
              f"{len(matched)} match the interface-art names")
    if args.names:
        for key, found in sorted(matched.items(), key=lambda kv: kv[1]):
            print(f"  {key:016x}  {', '.join(found)}")
        return 0

    pack = pathlib.Path(args.pack)
    if not pack.is_dir():
        print(f"{pack}: not a folder", file=sys.stderr)
        return 1
    holding = pack / args.into

    present = []
    for key, found in matched.items():
        candidate = pack / f"{key:016x}.png"
        if candidate.is_file():
            present.append((candidate, found))
    print(f"{len(present)} of them have a replacement in the pack")
    for candidate, found in sorted(present, key=lambda row: row[1]):
        print(f"  {candidate.name}  <- {', '.join(found)}")
    if not args.apply:
        print("\nnothing moved. Pass --apply to move these into "
              f"{holding.name}/ (reversible: move them back to undo)")
        return 0

    holding.mkdir(exist_ok=True)
    moved = 0
    for candidate, _ in present:
        target = holding / candidate.name
        if target.exists():
            print(f"  {candidate.name}: already in {holding.name}, left alone")
            continue
        shutil.move(str(candidate), str(target))
        moved += 1
    print(f"\nmoved {moved} into {holding}")
    print("The game now draws those from its own textures, scaled by the "
          "engine's Sharp filter. Move them back to undo.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
