"""Read Metal Gear Ac!d's .dar archives: the models, animations and effects.

A stage's _zar holds a cache.qar of textures (see qar.py) and a cache.dar of
everything else. The .dar is the same named-entry container the _zar itself
uses internally, which is why it needs so little code here:

    u32 count
    count times:
        a null-terminated name, padded to four bytes
        u32 size
        the data, at the next sixteen byte boundary

Across the 42 stages that have one: 1,749 entries, and every archive's count
matches what the walk finds.

    .mdp   782   models          "MDP "
    .la2   315   animation       07 00 01 00
    .mtsq  144   motion          ca 54 01 00
    .mtst  144   motion
    .mtar  144   motion          "Mtar"
    .eft   116   effects         b9 6c 0b 00
    .lt2    49
    .mgm    43                   "MGM\\x02"
    .rpd     8
    .row     2
    .png     2   ordinary PNG

What is known about MDP, which is as far as this went
-----------------------------------------------------

Enough to walk the file and not enough to draw it. Worth writing down anyway,
because the next person to look starts here rather than at the first byte.

    +00  "MDP "
    +04  u32 kind                0 in most, but twelve values occur
    +08  u32 1
    +0c  u32 count               nodes
    +10  u32                     an id of some kind; differs per model
    +14  u32, +18 u32, +1c u32   three offset slots, not all of them used

The nodes begin at the LARGEST of those three offsets, and each is 80 bytes.
That rule holds for all 782 models in the archives, which is what makes it
worth stating; the header's earlier fields shift about between the twelve
values of `kind`, and reading a fixed offset works on two thirds of them and
quietly fails on the rest.

    +20 or +28  f32              a scale, 0.0100 to 0.0153 in the files seen
    vec4, vec4                   bounds, two corners, w = 1

A node:

    +00  u32 id                  a 24-bit value, never zero, different per
                                 node. It is NOT a magic: three models that
                                 happened to share one made it look like one,
                                 and eleven other values turn up as soon as
                                 more files are read. All 782 models have a
                                 zero high byte in every node, which is the
                                 check the walk uses instead.
    +0c  u32 offset              a descriptor: sizes and two more offsets
    +10  u32 offset              vertices
    +14  u32 offset              a table, contents unread
    +18  u32 count               vertices; count * 16 from +10 lands inside
                                 the file, which is what suggests 16 as the
                                 stride
    +20  vec4, +30 vec4          this node's own bounds

The vertices look like the GE's own packed format -- two u16 texture
coordinates, an 8888 colour (0xFFFFFFFF throughout the ones read), then three
s16 positions, which is fourteen bytes aligned to sixteen by the colour. That
is read off the bytes, not something drawn and checked, and the same caution
applies as to the node id: a pattern that holds in the files looked at first
is not the same as a pattern that holds.

No texture data and no texture name appears anywhere in an MDP. That is the
useful negative result: a model's art is in the stage's .qar, which qar.py
already extracts, so nothing here is needed for a texture pack.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from stage_link import archive_entries, unzar

NODE_STRIDE = 80


def entries(blob):
    """name -> bytes. The same layout the _zar uses inside itself."""
    count, = struct.unpack_from("<I", blob, 0)
    pos = 4
    for _ in range(count):
        end = blob.find(b"\0", pos)
        if end < 0:
            return
        name = blob[pos:end].decode("ascii", "replace")
        after = (end + 1 + 3) & ~3
        if after + 4 > len(blob):
            return
        size, = struct.unpack_from("<I", blob, after)
        data = (after + 4 + 15) & ~15
        if data + size > len(blob):
            return
        yield name, blob[data:data + size]
        pos = data + size
        while pos < len(blob) and blob[pos] == 0:
            pos += 1


def declared_count(blob):
    return struct.unpack_from("<I", blob, 0)[0]


class Model(object):
    """As much of an MDP as the notes above cover."""

    def __init__(self, blob):
        self.blob = blob
        self.nodes = []
        if len(blob) < 0x60 or blob[:4] != b"MDP ":
            return
        self.kind, _, count = struct.unpack_from("<3I", blob, 4)
        # The nodes start at whichever of the three slots reaches furthest; see
        # the notes above for why a fixed offset is wrong.
        start = max(struct.unpack_from("<3I", blob, 0x14))
        if start + NODE_STRIDE * count > len(blob):
            return
        for i in range(count):
            at = start + i * NODE_STRIDE
            fields = struct.unpack_from("<8I", blob, at)
            # A node's id is 24 bits and never zero. Not much of a check, but
            # it is the one that is true of every model rather than of the
            # first few.
            if fields[0] == 0 or (fields[0] >> 24) != 0:
                return
            self.nodes.append({"at": at, "id": fields[0], "descriptor": fields[3], "vertices": fields[4],
                               "table": fields[5], "count": fields[6],
                               "bounds": (struct.unpack_from("<4f", blob, at + 0x20),
                                          struct.unpack_from("<4f", blob, at + 0x30))})

    @property
    def valid(self):
        return bool(self.nodes)


def archives(path):
    """(label, bytes) for a .dar, or for every .dar inside a stage archive."""
    with open(path, "rb") as handle:
        blob = handle.read()
    if os.path.basename(path).lower().endswith(".dar"):
        yield os.path.splitext(os.path.basename(path))[0], blob
        return
    stage = os.path.basename(os.path.dirname(os.path.abspath(path)))
    for name, data in entries(unzar(blob)):
        if name.lower().endswith(".dar"):
            yield stage, data


def walk(paths):
    for path in paths:
        if os.path.isdir(path):
            for root, _dirs, files in sorted(os.walk(path)):
                for name in sorted(files):
                    if name.lower().endswith(".dar") or name == "_zar":
                        for label, blob in archives(os.path.join(root, name)):
                            yield label, blob
        else:
            for label, blob in archives(path):
                yield label, blob


def command_list(paths):
    kinds = {}
    total = archives_seen = disagreed = 0
    for label, blob in walk(paths):
        found = list(entries(blob))
        archives_seen += 1
        if len(found) != declared_count(blob):
            disagreed += 1
            print("%s: says %d entries, walked %d" % (label, declared_count(blob), len(found)))
        for name, data in found:
            total += 1
            kinds.setdefault(os.path.splitext(name)[1].lower(), []).append(len(data))
        print("%s: %d entries" % (label, len(found)))
    print("\n%d archives, %d entries%s" % (archives_seen, total,
                                           ", %d disagreed with their count" % disagreed if disagreed else ""))
    for kind, sizes in sorted(kinds.items(), key=lambda pair: -len(pair[1])):
        print("   %-7s %4d   %7.1f KB total" % (kind or "(none)", len(sizes), sum(sizes) / 1024.0))


def command_extract(paths, destination):
    written = 0
    for label, blob in walk(paths):
        out = os.path.join(destination, label)
        os.makedirs(out, exist_ok=True)
        for name, data in entries(blob):
            with open(os.path.join(out, os.path.basename(name)), "wb") as handle:
                handle.write(data)
            written += 1
        print("%s -> %s" % (label, out))
    print("wrote %d files" % written)


def command_models(paths):
    """What the MDP header says, for every model in the archives."""
    total = readable = 0
    nodes = vertices = 0
    for label, blob in walk(paths):
        for name, data in entries(blob):
            if not name.lower().endswith(".mdp"):
                continue
            total += 1
            model = Model(data)
            if not model.valid:
                print("   %-28s unreadable header" % name)
                continue
            readable += 1
            nodes += len(model.nodes)
            vertices += sum(node["count"] for node in model.nodes)
    print("models %d, headers read %d, nodes %d, vertices declared %d" % (total, readable, nodes, vertices))


def main(argv):
    if len(argv) < 3 or argv[1] not in ("list", "extract", "models"):
        print(__doc__.strip().splitlines()[0])
        print("usage: dar.py list <archive|directory>...")
        print("       dar.py extract <destination> <archive|directory>...")
        print("       dar.py models <archive|directory>...")
        return 2
    if argv[1] == "list":
        command_list(argv[2:])
    elif argv[1] == "models":
        command_models(argv[2:])
    else:
        command_extract(argv[3:], argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
