#!/usr/bin/env python3
"""Metal Gear Ac!d links its stage modules itself, at run time.

A stage PRX (PSP_GAME/USRDIR/stage/*/*.prx) refers to the main executable's
code and data through offsets with no relocation entries, plus a symbol table
the game resolves by hash after loading the module. Only then does the stage's
code hold real addresses. A recompiled corpus must be generated from the code
in that state, so the reliable source is the module's memory as the game left
it: run the game with MGA_DUMP_MODULES=<dir> and use the dumps here.

Usage:
The rest of that link is a relocation table the stage carries in its own
archive: PSP_GAME/USRDIR/stage/<name>/_zar holds <name>.rlc, ordinary PSP
relocations whose second base is not the module but the executable. The game
applies them with two bases -- 0x08804000 for its code, 0x0893E750 for the
data its stages' symbols live in -- and that is the step that turns the
offsets left in the PRX into addresses. Doing it here reproduces the game's
own link exactly: checked against dumps collected by playing, every word of
.text matches.

Usage:
  stage_link.py fromdump <stage.prx> <memory.bin> <out.prx> [<_zar> <stage>]
                                              an ELF whose contents are the loaded,
                                              linked module (for psp_recomp)
  stage_link.py patch <stage.prx> <out.prx>   only the `jal`s into the executable
                                              (partial: data references stay unlinked)
  stage_link.py check <stage.prx> <memory.bin> <load_base>
                                              compare the partial patch with a dump
  stage_link.py hash <file>                   FNV-1a hash the host's module loader uses
"""

import struct
import sys
import zlib

MAIN_BASE = 0x08804000
MAIN_TEXT_END = 0x00130000  # offsets past the executable's code are not calls into it


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


# The two bases the game hands its relocation walker: its code, and the data
# region the stages' symbols live in. Both are fixed -- the module manager
# allocates in the same order every boot -- and both were read straight out of
# the executable's own call to that walker at 0x08804E90.
RELOC_BASES = (0x08804000, 0x0893E750)


def unzar(blob):
    """A stage archive: an uncompressed size, then a raw zlib stream."""
    size, = struct.unpack_from("<I", blob, 0)
    raw = zlib.decompress(blob[4:])
    if len(raw) != size:
        raise ValueError("archive says %d bytes, got %d" % (size, len(raw)))
    return raw


def archive_entries(raw):
    """name -> bytes, for the contents of an unpacked archive.

    Each entry is a null terminated name, padded to four bytes, a u32 size,
    and then the data at the next sixteen byte boundary. That alignment is the
    part worth writing down: the header between a name and its data is eight
    bytes after one name and sixteen after another, so reading it as a fixed
    size header walks off the rails a few entries in.
    """
    count, = struct.unpack_from("<I", raw, 0)
    pos = 4
    for _ in range(count):
        end = raw.find(b"\0", pos)
        if end < 0:
            return
        name = raw[pos:end].decode("ascii", "replace")
        after = (end + 1 + 3) & ~3
        size, = struct.unpack_from("<I", raw, after)
        data = (after + 4 + 15) & ~15
        if data + size > len(raw):
            return
        yield name, raw[data:data + size]
        pos = data + size
        while pos < len(raw) and raw[pos] == 0:
            pos += 1


def stage_relocations(archive_path, stage):
    """<stage>.rlc out of that stage's archive, or None."""
    for name, data in archive_entries(unzar(open(archive_path, "rb").read())):
        if name == stage + ".rlc":
            return data
    return None


def _signed(half):
    return struct.unpack("<h", struct.pack("<H", half & 0xFFFF))[0]


def already_linked(image, rlc, segments):
    """Whether these relocations have been applied to this image already.

    A dump taken from a stage the game had finished with is linked; one taken
    the moment the module started is not. Applying the table twice moves every
    address a second time, so the difference has to be decided rather than
    assumed. A call into the executable is the tell: before the link it holds a
    bare offset, far too small to be an address.
    """
    for i in range(0, len(rlc) - 7, 8):
        offset, info = struct.unpack_from("<II", rlc, i)
        if (info & 0xFF) != 4 or ((info >> 8) & 0xFF) not in segments:
            continue
        at = segments[(info >> 8) & 0xFF] + offset
        if at + 4 > len(image):
            continue
        word, = struct.unpack_from("<I", image, at)
        return ((word & 0x03FFFFFF) << 2) >= MAIN_BASE
    return False


def apply_relocations(image, rlc, segments, bases=RELOC_BASES):
    """The game's own second relocation pass, over a module image in memory."""
    entries = [struct.unpack_from("<II", rlc, i) for i in range(0, len(rlc) - 7, 8)]
    applied = 0
    for index, (offset, info) in enumerate(entries):
        kind, offset_base, addr_base = info & 0xFF, (info >> 8) & 0xFF, (info >> 16) & 0xFF
        if offset_base not in segments:
            continue
        at = segments[offset_base] + offset
        if at + 4 > len(image):
            continue
        addend = bases[addr_base] if addr_base < len(bases) else 0
        word, = struct.unpack_from("<I", image, at)
        if kind == 2:                                     # R_MIPS_32
            word = (word + addend) & 0xFFFFFFFF
        elif kind == 4:                                   # R_MIPS_26
            target = ((word & 0x03FFFFFF) << 2) + addend
            word = (word & 0xFC000000) | ((target >> 2) & 0x03FFFFFF)
        elif kind == 5:                                   # R_MIPS_HI16
            # The high half cannot be worked out without the low half it pairs
            # with: that half is signed, so it may borrow from this one.
            low = next((e for e in entries[index + 1:] if (e[1] & 0xFF) == 6), None)
            if low is None:
                continue
            low_at = segments.get((low[1] >> 8) & 0xFF, 0) + low[0]
            if low_at + 4 > len(image):
                continue
            low_word, = struct.unpack_from("<I", image, low_at)
            value = ((word & 0xFFFF) << 16) + _signed(low_word) + addend
            word = (word & 0xFFFF0000) | (((value >> 16) + (1 if value & 0x8000 else 0)) & 0xFFFF)
        elif kind == 6:                                   # R_MIPS_LO16
            word = (word & 0xFFFF0000) | ((_signed(word) + addend) & 0xFFFF)
        else:
            continue
        struct.pack_into("<I", image, at, word)
        applied += 1
    return applied


def sections(data):
    shoff, = struct.unpack_from("<I", data, 0x20)
    shnum, shstrndx = struct.unpack_from("<HH", data, 0x30)
    headers = [struct.unpack_from("<10I", data, shoff + 40 * i) for i in range(shnum)]
    names = headers[shstrndx]

    def name(offset):
        start = names[4] + offset
        return data[start:data.index(b"\0", start)].decode()

    return {name(h[0]): h for h in headers}


def main_calls(data):
    """(file offset, target offset) of every unrelocated jal into the executable."""
    secs = sections(data)
    text = secs[".text"]
    relocated = set()
    rel = secs.get(".rel.text")
    if rel is not None:
        for i in range(0, rel[5], 8):
            relocated.add(struct.unpack_from("<I", data, rel[4] + i)[0])
    calls = []
    for offset in range(0, text[5], 4):
        if text[3] + offset in relocated:
            continue
        word, = struct.unpack_from("<I", data, text[4] + offset)
        target = (word & 0x03FFFFFF) << 2
        if word >> 26 == 3 and target < MAIN_TEXT_END:
            calls.append((text[4] + offset, text[3] + offset, target))
    return calls


def patched_word(word):
    target = ((word & 0x03FFFFFF) << 2) + MAIN_BASE
    return (word & 0xFC000000) | ((target >> 2) & 0x03FFFFFF)


def patch(source, destination):
    data = bytearray(open(source, "rb").read())
    calls = main_calls(bytes(data))
    for file_offset, _, _ in calls:
        word, = struct.unpack_from("<I", data, file_offset)
        struct.pack_into("<I", data, file_offset, patched_word(word))
    open(destination, "wb").write(bytes(data))
    print(f"{source}: {len(calls)} calls into the executable patched")


def check(source, dump, base):
    data = open(source, "rb").read()
    memory = open(dump, "rb").read()
    secs = sections(data)
    text = secs[".text"]
    calls = {vaddr for _, vaddr, _ in main_calls(data)}
    predicted_only = game_only = 0
    for vaddr in range(text[3], text[3] + text[5], 4):
        loaded, = struct.unpack_from("<I", memory, vaddr)
        original, = struct.unpack_from("<I", data, text[4] + vaddr - text[3])
        if vaddr in calls:
            if loaded != patched_word(original):
                predicted_only += 1
        elif loaded != original and original >> 26 == 3 and loaded >> 26 == 3:
            # A relocated call differs by the load base, which is expected.
            if ((loaded & 0x03FFFFFF) << 2) - ((original & 0x03FFFFFF) << 2) != (base & 0x0FFFFFFF):
                game_only += 1
    print(f"{source}: {len(calls)} predicted patches, {predicted_only} not matched in memory, "
          f"{game_only} other call changes")
    return predicted_only == 0 and game_only == 0


def from_dump(source, dump, destination, archive=None, stage=None):
    """An ELF holding the module exactly as the game linked it in memory.

    Section contents are replaced by the dump, and the relocation sections are
    dropped: the dump is already relocated, so applying them again would move
    every address a second time.
    """
    data = bytearray(open(source, "rb").read())
    memory = bytearray(open(dump, "rb").read())

    # The half of the link the game performs itself, which a dump taken the
    # moment the module started has not had yet.
    if archive is not None:
        rlc = stage_relocations(archive, stage)
        if rlc is None:
            print(f"{destination}: no {stage}.rlc in the archive; not linked", file=sys.stderr)
        else:
            secs = sections(bytes(data))
            segments = {0: secs[".text"][3]}
            if ".data" in secs:
                segments[1] = secs[".data"][3]
            if already_linked(memory, rlc, segments):
                print(f"{destination}: dump was taken after the game linked it")
            else:
                applied = apply_relocations(memory, rlc, segments)
                print(f"{destination}: {applied} relocations applied from {stage}.rlc")
    memory = bytes(memory)
    shoff, = struct.unpack_from("<I", data, 0x20)
    shnum, shstrndx = struct.unpack_from("<HH", data, 0x30)
    names = struct.unpack_from("<10I", data, shoff + 40 * shstrndx)

    def name(offset):
        start = names[4] + offset
        return data[start:data.index(b"\0", start)].decode()

    replaced = dropped = 0
    for i in range(shnum):
        header = struct.unpack_from("<10I", data, shoff + 40 * i)
        section = name(header[0])
        if section.startswith(".rel"):
            # SHT_NULL, with no contents: psp_recomp then applies no relocations.
            struct.pack_into("<I", data, shoff + 40 * i + 4, 0)
            struct.pack_into("<I", data, shoff + 40 * i + 20, 0)
            dropped += 1
            continue
        # PROGBITS inside the loaded image: take the bytes from memory.
        if header[1] != 1 or header[5] == 0 or header[3] + header[5] > len(memory):
            continue
        data[header[4]:header[4] + header[5]] = memory[header[3]:header[3] + header[5]]
        replaced += 1
    open(destination, "wb").write(bytes(data))
    print(f"{destination}: {replaced} sections taken from {dump}, {dropped} relocation sections dropped")


if __name__ == "__main__":
    if len(sys.argv) in (5, 7) and sys.argv[1] == "fromdump":
        from_dump(sys.argv[2], sys.argv[3], sys.argv[4],
                  *(sys.argv[5:7] if len(sys.argv) == 7 else ()))
        sys.exit(0)
    if len(sys.argv) == 4 and sys.argv[1] == "patch":
        patch(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 5 and sys.argv[1] == "check":
        sys.exit(0 if check(sys.argv[2], sys.argv[3], int(sys.argv[4], 0)) else 1)
    elif len(sys.argv) == 3 and sys.argv[1] == "hash":
        print(f"0x{fnv1a(open(sys.argv[2], 'rb').read()):08X}")
    else:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
