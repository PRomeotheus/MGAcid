#!/usr/bin/env python3
"""Build a replacement-music pack from a mapping produced by match_ost.py.

    build_music_pack.py <a3p dir> <source audio dir> <ost_map.csv> <out dir>
                        [--dry-run]

Writes one <key>.wav per mapped cue, where <key> is the identity the runtime
computes for that cue in audio/music_pack.cpp. Nothing has to be renamed and
there is no manifest: the file name is the lookup.

Each file is written already lined up with the stream it replaces -- sample 0
of the file is sample 0 of the DECODED stream, delay included -- so the game
keeps using its own loop points and position arithmetic untouched. All the
alignment lives here.

    output[i] = source[i + offset - skip]

`offset` is what match_ost.py measured by correlation; `skip` is the encoder
and decoder delay the real stream carries before its first playable sample,
derived from the .a3p exactly as the runtime's header parser derives it.

Where the source recording runs out before the cue's loop end -- an album
arrangement that ends where the game's cue would still be playing -- the tail
is filled from one loop length earlier in the output:

    output[i] = output[i - loop_length]

which is musically what the loop itself asserts, so the seam stays seamless
instead of fading to silence.
"""
import csv, os, struct, subprocess, sys, wave

ATRAC3, ATRAC3PLUS = 0x0270, 0xFFFE
CODEC_ID = {ATRAC3: 0, ATRAC3PLUS: 1}          # mga::audio::AtracCodec
DELAY = {ATRAC3: 69, ATRAC3PLUS: 368}          # decoder delay, in samples
FRAME = {ATRAC3: 1024, ATRAC3PLUS: 2048}
KEY_BYTES = 1024                               # kMusicKeyBytes
RATE = 44100


def fnv1a(data, hash_=0xcbf29ce484222325):
    for byte in data:
        hash_ = ((hash_ ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return hash_


def describe(path):
    """The fields the runtime's parse_header derives, plus the key."""
    raw = open(path, "rb").read(0x1000)
    if raw[0:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"{path}: not RIFF/WAVE")
    out = {"file_size": struct.unpack_from("<I", raw, 4)[0] + 8}
    fact_samples = fact_offset = None
    loop = None
    offset = 12
    while offset + 8 <= len(raw):
        cid = raw[offset:offset + 4]
        size = struct.unpack_from("<I", raw, offset + 4)[0]
        body = offset + 8
        if cid == b"data":
            out["data_offset"], out["data_size"] = body, size
            break
        if cid == b"fmt " and size >= 16:
            tag, channels, _, _, align = struct.unpack_from("<HHIIH", raw, body)
            out.update(tag=tag, channels=channels, block_align=align)
        elif cid == b"fact" and size >= 4:
            fact_samples = struct.unpack_from("<I", raw, body)[0]
            fact_offset = struct.unpack_from("<I", raw, body + 4)[0] if size >= 8 else 0
        elif cid == b"smpl" and size >= 36:
            if struct.unpack_from("<I", raw, body + 28)[0] > 0 and size >= 60:
                loop = struct.unpack_from("<II", raw, body + 44)
        offset = body + size + (size & 1)

    tag = out["tag"]
    out["skip"] = (fact_offset or 0) + DELAY[tag]
    out["frame"] = FRAME[tag]
    decoded = (out["data_size"] // out["block_align"]) * out["frame"]
    playable = max(0, decoded - out["skip"])
    samples = min(fact_samples, playable) if fact_samples else playable
    out["end_sample"] = samples - 1
    out["loop_start"] = out["loop_end"] = -1
    if loop and loop[0] >= (fact_offset or 0) and loop[1] >= loop[0]:
        out["loop_start"] = loop[0] - (fact_offset or 0)
        out["loop_end"] = min(loop[1] - (fact_offset or 0), out["end_sample"])

    audio = raw[out["data_offset"]:out["data_offset"] + KEY_BYTES]
    if len(audio) < KEY_BYTES:
        raise ValueError(f"{path}: header window too small to key")
    head = b"".join(struct.pack("<I", v) for v in
                    (out["file_size"], out["data_size"], out["channels"],
                     out["block_align"], CODEC_ID[tag]))
    out["key"] = fnv1a(audio, fnv1a(head))
    return out


def decode(path):
    cmd = ["ffmpeg", "-v", "error", "-i", str(path), "-f", "s16le",
           "-ar", str(RATE), "-ac", "2", "-"]
    return bytearray(subprocess.run(cmd, capture_output=True, check=True).stdout)


def build(source, info, offset):
    """The decoded-stream timeline, as bytes. Returns (pcm, filled_frames)."""
    frame = info["frame"]
    total = ((info["end_sample"] + info["skip"]) // frame + 1) * frame
    shift = offset - info["skip"]
    out = bytearray(total * 4)

    # Copy whatever the source can cover.
    start = max(0, -shift)                     # first output frame with source
    src_at = max(0, shift)
    have = max(0, len(source) // 4 - src_at)
    run = min(total - start, have)
    if run > 0:
        out[start * 4:(start + run) * 4] = source[src_at * 4:(src_at + run) * 4]
    filled = start + max(0, run)

    # Fill any tail from one loop length earlier: the loop says those samples
    # are the same music, so this keeps the seam seamless.
    loop_len = (info["loop_end"] - info["loop_start"] + 1) if info["loop_start"] >= 0 else 0
    if filled < total and loop_len > 0:
        at = filled
        while at < total:
            back = at - loop_len
            if back < 0:
                break
            run = min(loop_len, total - at, at - max(0, back))
            if run <= 0:
                break
            out[at * 4:(at + run) * 4] = out[back * 4:(back + run) * 4]
            at += run
        filled = at
    return out, filled, total


def main(argv):
    if len(argv) < 5:
        print(__doc__.strip()); return 2
    a3p_dir, src_dir, map_csv, out_dir = argv[1:5]
    dry = "--dry-run" in argv
    if not dry:
        os.makedirs(out_dir, exist_ok=True)

    # Everything the mapping measured an offset for. A cue whose source runs
    # out before its loop end is included deliberately: the tail fill below is
    # what that case exists for. Only the three rejections are dropped.
    rejected = ("not on album", "too weak", "different edit", "missing", "too short")
    rows = [r for r in csv.DictReader(open(map_csv))
            if r.get("offset_samples") and not r["status"].startswith(rejected)]
    written = skipped = 0
    for row in rows:
        cue, album = row["cue"], row["album_track"]
        a3p = os.path.join(a3p_dir, cue + ".a3p")
        if not os.path.exists(a3p):
            print(f"  skip {cue}: no {cue}.a3p"); skipped += 1; continue
        source = next((os.path.join(src_dir, album + e) for e in (".flac", ".wav", ".m4a", ".mp3")
                       if os.path.exists(os.path.join(src_dir, album + e))), None)
        if source is None:
            print(f"  skip {cue}: no source audio for {album!r}"); skipped += 1; continue
        info = describe(a3p)
        pcm, filled, total = build(decode(source), info, int(row["offset_samples"]))
        name = f"{info['key']:016x}.wav"
        note = ""
        if filled < total:
            note = f"  ({(total - filled) / RATE:.2f}s of silence at the end)"
        elif int(row["offset_samples"]) - info["skip"] < 0:
            note = "  (leading silence: the recording starts after the cue does)"
        print(f"  {cue:<22} {name}  {total / RATE:6.1f}s  <- {album}{note}")
        if not dry:
            with wave.open(os.path.join(out_dir, name), "wb") as handle:
                handle.setnchannels(2); handle.setsampwidth(2); handle.setframerate(RATE)
                handle.writeframes(bytes(pcm))
        written += 1
    print(f"\n{written} written, {skipped} skipped -> {out_dir}" if not dry
          else f"\n{written} would be written, {skipped} skipped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
