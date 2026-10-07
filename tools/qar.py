"""Read Metal Gear Ac!d's .qar texture archives.

A .qar has no header. It is a run of texture chunks, each padded to a 128 byte
boundary, followed by an index: a u32 count, then count eight byte records of
(hash, chunk size), then count null terminated names. Nothing points forward to
the index, so the only way in is to walk the chunks and notice when one stops
looking like a chunk. That turns out to be easy, because every chunk carries
enough to compute its own length, which is also what makes the walk self
checking: if the walk is wrong it desynchronises within an entry or two, and the
count it lands on will not match the number of chunks it found.

A chunk is the same container the archive's first entry uses, thirty two bytes:

    u32 zero
    u32 flags
    u32 count          textures in this chunk
    u32 table_a        always 0x20
    u32 count          again
    u32 table_b        always 0x20 + count * 16
    u32 zero, zero

Table A is count sixteen byte texture descriptors, table B is count forty eight
byte records the game uses at draw time (an id, the dimensions again, a uv
scale) that nothing here needs. A descriptor is:

    u16 width          (log2(stride) << 12) | width
    u16 height         (log2(padded height) << 12) | height
    u16 format         a GE texture format: 4 is CLUT4, 5 is CLUT8
    u16 flags          2 means the pixels are deflated
    u32 data           from the start of the chunk
    u32 end            one past the pixels; the palette starts here

The low twelve bits of each dimension are the real size and the top nibble is
log2 of the size the game rounds up to, so a 96x40 texture reports a stride of
128. Rows are packed at the real width, not the stride. The palette that follows
the pixels is 4 << 2 bytes for CLUT4 and 256 << 2 for CLUT8, RGBA8888 either
way. A deflated texture stores a u32 length and then a zlib stream, and inflates
to exactly the size the dimensions and format imply.

Written against the forty four stage archives on the disc, which is all of
them: USRDIR/stage/<stage>/_zar, each holding one cache.qar.

There is no resident.qar on the disc. An earlier version of this note named one,
because a runtime dump of the resident texture set had that name and was what
the format was first cracked against. Its contents are not extra -- the same
textures are in stage/init and stage/com, and since a texture is keyed by its
content rather than its name, the pack comes out the same either way. Snake's
face is a good example: the dump called it resident/sna_def_low_09 and the disc
calls it com/sna_low_blue, and both are the same 128x64 image with the same
key.
"""

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from stage_link import archive_entries, unzar

ALIGN = 128
FORMATS = {4: "CLUT4", 5: "CLUT8"}
BITS = {4: 4, 5: 8}
DEFLATED = 2


class Texture(object):
    def __init__(self, archive, chunk, index, width, height, stride, padded,
                 fmt, flags, data, end):
        self.archive = archive
        self.chunk = chunk
        self.index = index
        self.width = width
        self.height = height
        self.stride = stride
        self.padded = padded
        self.format = fmt
        self.flags = flags
        self.data = data
        self.end = end

    @property
    def deflated(self):
        return bool(self.flags & DEFLATED)

    @property
    def pixel_bytes(self):
        return self.width * self.height * BITS[self.format] // 8

    @property
    def palette_bytes(self):
        return (1 << BITS[self.format]) * 4

    def __str__(self):
        note = " deflated" if self.deflated else ""
        stride = "" if self.stride == self.width else " stride %d" % self.stride
        return "%dx%d %s%s%s" % (self.width, self.height,
                                 FORMATS.get(self.format, "%d" % self.format),
                                 stride, note)


class Chunk(object):
    def __init__(self, offset, blob, textures):
        self.offset = offset
        self.blob = blob
        self.textures = textures
        self.name = None

    @property
    def size(self):
        """The pixels and palette that reach furthest, relative to the chunk.

        The textures are laid out in order, so this is all but always the last
        one, but taking the furthest costs nothing and does not assume it.
        """
        return max(texture.end - self.offset + texture.palette_bytes
                   for texture in self.textures)


def _dimension(value):
    """A packed dimension: log2 of the rounded up size, then the real size."""
    return value & 0xFFF, 1 << (value >> 12)


def chunk_at(blob, offset):
    """The chunk at an offset, or None if there is not one there."""
    if offset + 32 > len(blob):
        return None
    zero, _flags, count, table_a, again, table_b, tail_a, tail_b = \
        struct.unpack_from("<8I", blob, offset)
    if zero or tail_a or tail_b or table_a != 0x20:
        return None
    if not count or count != again or table_b != 0x20 + count * 16:
        return None
    if offset + table_b + count * 48 > len(blob):
        return None
    textures = []
    for i in range(count):
        width, height, fmt, flags, data, end = \
            struct.unpack_from("<HHHHII", blob, offset + table_a + i * 16)
        if fmt not in BITS or data >= end:
            return None
        w, stride = _dimension(width)
        h, padded = _dimension(height)
        if not w or not h:
            return None
        textures.append(Texture(blob, offset, i, w, h, stride, padded,
                                fmt, flags, offset + data, offset + end))
    return Chunk(offset, blob, textures)


def chunks(blob):
    """Walk an archive. Raises if the index does not agree with the walk."""
    found = []
    offset = 0
    while True:
        chunk = chunk_at(blob, offset)
        if chunk is None:
            break
        found.append(chunk)
        offset = (offset + chunk.size + ALIGN - 1) & ~(ALIGN - 1)
    if offset + 4 > len(blob):
        raise ValueError("walked off the end of the archive")
    count, = struct.unpack_from("<I", blob, offset)
    if count != len(found):
        raise ValueError("index says %d chunks, the walk found %d"
                         % (count, len(found)))
    names = offset + 4 + count * 8
    pos = names
    for chunk in found:
        end = blob.find(b"\0", pos)
        if end < 0:
            raise ValueError("the name table stops early")
        chunk.name = blob[pos:end].decode("ascii", "replace")
        pos = end + 1
    return found


def pixels(texture):
    """The index bytes of a texture, inflating it if it is deflated."""
    if not texture.deflated:
        return texture.archive[texture.data:texture.end]
    length, = struct.unpack_from("<I", texture.archive, texture.data)
    start = texture.data + 4
    return zlib.decompress(texture.archive[start:start + length])


def unswizzle(data, pitch, height):
    """Undo the GE's texture swizzle: sixteen byte by eight row blocks.

    Every texture in the archives is swizzled, and all of them are a whole
    number of blocks, so there is no partial block case to get wrong. It is
    worth knowing what the failure looks like: a swizzled texture read as a
    linear one comes out as horizontal streaks with the right colours in it,
    which reads as a corrupt palette rather than a layout problem.
    """
    out = bytearray(len(data))
    blocks = pitch // 16
    source = 0
    for block_row in range(height // 8):
        for block in range(blocks):
            for row in range(8):
                at = (block_row * 8 + row) * pitch + block * 16
                out[at:at + 16] = data[source:source + 16]
                source += 16
    return bytes(out)


def palette(texture):
    """RGBA quads, one per index."""
    raw = texture.archive[texture.end:texture.end + texture.palette_bytes]
    return [tuple(raw[i:i + 4]) for i in range(0, len(raw), 4)]


def rgba(texture):
    """Decode to a width * height * 4 bytearray."""
    index = pixels(texture)
    want = texture.pixel_bytes
    if len(index) != want:
        raise ValueError("%s: %d pixel bytes, expected %d"
                         % (texture, len(index), want))
    pitch = texture.width * BITS[texture.format] // 8
    if pitch % 16 or texture.height % 8:
        raise ValueError("%s: %d by %d is not whole blocks"
                         % (texture, pitch, texture.height))
    index = unswizzle(index, pitch, texture.height)
    clut = palette(texture)
    out = bytearray(texture.width * texture.height * 4)
    at = 0
    if texture.format == 4:
        for byte in index:
            out[at:at + 4] = bytes(clut[byte & 0xF])
            out[at + 4:at + 8] = bytes(clut[byte >> 4])
            at += 8
    else:
        for byte in index:
            out[at:at + 4] = bytes(clut[byte])
            at += 4
    return out


def content_key(width, height, data):
    """The key host/gpu/texture_decode.cpp computes for the same texture.

    FNV-1a over the width, the height and then the RGBA bytes. It has to match
    content_key() there exactly, so keep the two together: this is what lets a
    replacement extracted here be found again while the game is running.
    """
    key = 0xCBF29CE484222325
    prime = 0x100000001B3
    mask = 0xFFFFFFFFFFFFFFFF
    for value in (width, height):
        for byte in struct.pack("<I", value):
            key = ((key ^ byte) * prime) & mask
    for byte in data:
        key = ((key ^ byte) * prime) & mask
    return key


def png(width, height, data):
    """A PNG of a width * height * 4 buffer, so this needs nothing installed."""
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)
        rows += data[y * stride:(y + 1) * stride]

    def block(tag, payload):
        head = struct.pack(">I", len(payload)) + tag
        return head + payload + struct.pack(">I", zlib.crc32(tag + payload))

    header = struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + block(b"IHDR", header)
            + block(b"IDAT", zlib.compress(bytes(rows), 9))
            + block(b"IEND", b""))


def archives(path):
    """(label, bytes) for a .qar, or for every .qar inside a stage archive.

    Every stage archive holds a cache.qar, so a stage's textures are labelled
    with the stage rather than the entry, or forty four stages would all want
    to write into the same directory.
    """
    with open(path, "rb") as handle:
        blob = handle.read()
    if os.path.basename(path).lower().endswith(".qar"):
        yield stem(path), blob
        return
    stage = os.path.basename(os.path.dirname(os.path.abspath(path)))
    found = [(name, data) for name, data in archive_entries(unzar(blob))
             if name.lower().endswith(".qar")]
    for name, data in found:
        yield stage if len(found) == 1 else "%s_%s" % (stage, stem(name)), data


def walk(paths):
    """Every archive under the given files and directories."""
    for path in paths:
        if os.path.isdir(path):
            for root, _dirs, files in sorted(os.walk(path)):
                for name in sorted(files):
                    full = os.path.join(root, name)
                    if name.lower().endswith(".qar") or name == "_zar":
                        for label, blob in archives(full):
                            yield full, label, blob
        else:
            for label, blob in archives(path):
                yield path, label, blob


def stem(name):
    base = os.path.basename(name)
    for suffix in (".txp", ".qar"):
        if base.lower().endswith(suffix):
            return base[:-4]
    return base


def command_list(paths):
    for path, label, blob in walk(paths):
        try:
            found = chunks(blob)
        except ValueError as problem:
            print("%s: %s" % (label, problem))
            continue
        total = sum(len(chunk.textures) for chunk in found)
        print("%s (%s): %d chunks, %d textures" % (label, path, len(found), total))
        for chunk in found:
            for texture in chunk.textures:
                print("    %-28s %s" % (stem(chunk.name), texture))


def command_pack(paths, destination):
    """A dump the texture pack can read, without the game having run.

    The pack addresses a replacement by content_key, as sixteen lowercase hex
    digits and .png, which is exactly what this writes. So the whole route is

        qar.py pack <dump dir> <USRDIR>
        upscale_textures.py <dump dir> <data>/textures
        turn the Texture pack setting on

    and the playthrough the dump used to need does not happen. One file per
    distinct image rather than one per appearance: the same art in twelve
    stages keys the same and is written once, which is the difference between
    2,356 files and 9,933.
    """
    os.makedirs(destination, exist_ok=True)
    seen = {}
    failed = 0
    for path, label, blob in walk(paths):
        try:
            found = chunks(blob)
        except ValueError as problem:
            print("%s: %s" % (label, problem))
            continue
        for chunk in found:
            for texture in chunk.textures:
                try:
                    data = rgba(texture)
                except (ValueError, zlib.error) as problem:
                    print("  %s: %s" % (stem(chunk.name), problem))
                    failed += 1
                    continue
                key = content_key(texture.width, texture.height, data)
                name = "%016x" % key
                where = "%s/%s" % (stem(label), stem(chunk.name))
                if key in seen:
                    seen[key][2].append(where)
                    continue
                with open(os.path.join(destination, name + ".png"), "wb") as handle:
                    handle.write(png(texture.width, texture.height, data))
                seen[key] = (texture.width, texture.height, [where])
    # The pack's own index is name and size; the archive each image came from
    # is added because a folder of hashes says even less than a folder of
    # names, and knowing which stage a texture belongs to is what makes it
    # possible to paint the ones that matter first.
    with open(os.path.join(destination, "index.txt"), "w") as handle:
        for key in sorted(seen):
            width, height, where = seen[key]
            handle.write("%016x  %dx%d  %s%s\n" % (key, width, height, where[0],
                                                   "" if len(where) == 1 else "  (+%d more)" % (len(where) - 1)))
    appearances = sum(len(v[2]) for v in seen.values())
    print("wrote %d images from %d appearances -> %s%s"
          % (len(seen), appearances, destination, ", %d failed" % failed if failed else ""))
    return 1 if failed else 0


def command_extract(paths, destination, manifest=False):
    written = failed = 0
    rows = []
    for path, label, blob in walk(paths):
        try:
            found = chunks(blob)
        except ValueError as problem:
            print("%s: %s" % (label, problem))
            continue
        out = os.path.join(destination, stem(label))
        os.makedirs(out, exist_ok=True)
        for chunk in found:
            for texture in chunk.textures:
                name = stem(chunk.name)
                if len(chunk.textures) > 1:
                    name = "%s_%02d" % (name, texture.index)
                try:
                    data = rgba(texture)
                except (ValueError, zlib.error) as problem:
                    print("  %s: %s" % (name, problem))
                    failed += 1
                    continue
                with open(os.path.join(out, name + ".png"), "wb") as handle:
                    handle.write(png(texture.width, texture.height, data))
                written += 1
                if manifest:
                    rows.append((content_key(texture.width, texture.height, data),
                                 stem(label), name, texture.width, texture.height,
                                 FORMATS.get(texture.format, texture.format)))
        print("%s: %d chunks -> %s" % (label, len(found), out))
    if manifest:
        path = os.path.join(destination, "textures.tsv")
        seen = {}
        with open(path, "w") as handle:
            handle.write("key\tstage\tname\twidth\theight\tformat\n")
            for row in rows:
                handle.write("%016x\t%s\t%s\t%d\t%d\t%s\n" % row)
                seen.setdefault(row[0], []).append("%s/%s" % (row[1], row[2]))
        shared = sum(1 for where in seen.values() if len(where) > 1)
        print("manifest: %s (%d keys, %d shared by more than one texture)"
              % (path, len(seen), shared))
    print("wrote %d textures%s" % (written, ", %d failed" % failed if failed else ""))
    return 1 if failed else 0


def main(argv):
    argv = list(argv)
    manifest = "--manifest" in argv
    if manifest:
        argv.remove("--manifest")
    if len(argv) < 3 or argv[1] not in ("list", "extract", "pack"):
        print(__doc__.strip().splitlines()[0])
        print("usage: qar.py list <archive|directory>...")
        print("       qar.py extract [--manifest] <destination> <archive|directory>...")
        print("       qar.py pack <dump directory> <archive|directory>...")
        return 2
    if argv[1] == "list":
        command_list(argv[2:])
        return 0
    if argv[1] == "pack":
        return command_pack(argv[3:], argv[2])
    return command_extract(argv[3:], argv[2], manifest)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
