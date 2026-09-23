#!/usr/bin/env python3
"""Metal Gear Ac!d links its stage modules itself, at run time.

A stage PRX (PSP_GAME/USRDIR/stage/*/*.prx) refers to the main executable's
code and data through offsets with no relocation entries, plus a symbol table
the game resolves by hash after loading the module. Only then does the stage's
code hold real addresses. A recompiled corpus must be generated from the code
in that state, so the reliable source is the module's memory as the game left
it: run the game with MGA_DUMP_MODULES=<dir> and use the dumps here.

Usage:
  stage_link.py fromdump <stage.prx> <memory.bin> <out.prx>
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

MAIN_BASE = 0x08804000
MAIN_TEXT_END = 0x00130000  # offsets past the executable's code are not calls into it


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


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


def from_dump(source, dump, destination):
    """An ELF holding the module exactly as the game linked it in memory.

    Section contents are replaced by the dump, and the relocation sections are
    dropped: the dump is already relocated, so applying them again would move
    every address a second time.
    """
    data = bytearray(open(source, "rb").read())
    memory = open(dump, "rb").read()
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
    if len(sys.argv) == 5 and sys.argv[1] == "fromdump":
        from_dump(sys.argv[2], sys.argv[3], sys.argv[4])
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
