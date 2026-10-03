#!/usr/bin/env python3
"""Read Metal Gear Ac!d's .a3p music files: what they are, and where they loop.

    a3p.py list   <a3p dir>
    a3p.py decode <a3p dir> <out dir> [--flac]

An .a3p is an ordinary RIFF/WAVE whose fmt tag is WAVE_FORMAT_EXTENSIBLE and
whose data is ATRAC3plus at 44.1 kHz stereo, about 64 kbit/s. FFmpeg decodes it
with no help, so "decode" is a convenience; the reason this script exists is
the loop points, which an audio editor will not show you.

Where the loop lives
--------------------
Two chunks matter and they do not agree about where zero is.

    fact   +0  samples        the playable length
           +4  fact_offset    the encoder delay, in samples
    smpl  +28  loop count
          +44  loop start     \\  counted from the start of the ENCODED stream,
          +48  loop end       /   so they include the encoder delay

The decoded stream begins earlier than sample zero of the music, by the encoder
delay plus the decoder's own (368 samples for ATRAC3plus, 69 for ATRAC3). So a
loop point as the game sees it is

    loop_start = smpl_start - fact_offset

which is what host/hle/hle_atrac.cpp computes, and what this prints under
"game". Whether your decoded WAV agrees depends on whether the decoder dropped
the delay for you: FFmpeg does, so "game" is the number to use against its
output. "raw" is printed beside it for when it does not.

The end is clamped the same way the runtime clamps it, to the last playable
sample rather than whatever the smpl chunk claims.
"""
import os, struct, subprocess, sys

ATRAC3, ATRAC3PLUS = 0x0270, 0xFFFE
DELAY = {ATRAC3: 69, ATRAC3PLUS: 368}
FRAME = {ATRAC3: 1024, ATRAC3PLUS: 2048}


def read(path):
    b = open(path, "rb").read()
    if b[0:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    info = {"path": path, "rate": 0, "channels": 0, "block_align": 0, "tag": 0,
            "fact_samples": None, "fact_offset": 0, "loop": None, "data_size": 0}
    off = 12
    while off + 8 <= len(b):
        cid, size = b[off:off + 4], struct.unpack_from("<I", b, off + 4)[0]
        body = off + 8
        if cid == b"data":
            info["data_size"] = size
            break
        if cid == b"fmt " and size >= 16:
            info["tag"], info["channels"] = struct.unpack_from("<HH", b, body)
            info["rate"] = struct.unpack_from("<I", b, body + 4)[0]
            info["block_align"] = struct.unpack_from("<H", b, body + 12)[0]
        elif cid == b"fact" and size >= 4:
            info["fact_samples"] = struct.unpack_from("<I", b, body)[0]
            if size >= 8:
                info["fact_offset"] = struct.unpack_from("<I", b, body + 4)[0]
        elif cid == b"smpl" and size >= 36:
            if struct.unpack_from("<I", b, body + 28)[0] > 0 and size >= 60:
                info["loop"] = struct.unpack_from("<II", b, body + 44)
        off = body + size + (size & 1)

    frame = FRAME.get(info["tag"], 2048)
    skip = info["fact_offset"] + DELAY.get(info["tag"], 368)
    decoded = (info["data_size"] // info["block_align"]) * frame if info["block_align"] else 0
    playable = max(0, decoded - skip)
    total = min(info["fact_samples"], playable) if info["fact_samples"] else playable
    info["samples"] = total
    info["end_sample"] = total - 1 if total else 0
    if info["loop"]:
        s, e = info["loop"]
        if s >= info["fact_offset"] and e >= s:
            info["game_loop"] = (s - info["fact_offset"],
                                 min(e - info["fact_offset"], info["end_sample"]))
    return info


def clock(samples, rate):
    if not rate:
        return "?"
    t = samples / rate
    return f"{int(t // 60)}:{t % 60:06.3f}"


def main(argv):
    if len(argv) < 3 or argv[1] not in ("list", "decode"):
        print(__doc__.strip())
        return 2
    src = argv[2]
    files = sorted(f for f in os.listdir(src) if f.lower().endswith(".a3p"))
    if not files:
        print(f"no .a3p files in {src}", file=sys.stderr)
        return 1

    if argv[1] == "decode":
        if len(argv) < 4:
            print("decode needs an output directory", file=sys.stderr)
            return 2
        out, flac = argv[3], "--flac" in argv
        os.makedirs(out, exist_ok=True)

    rows, looped = [], 0
    for name in files:
        try:
            i = read(os.path.join(src, name))
        except Exception as problem:
            print(f"  {name}: {problem}", file=sys.stderr)
            continue
        stem = name[:-4]
        rate = i["rate"]
        if "game_loop" in i:
            looped += 1
            s, e = i["game_loop"]
            loop = f"{s}-{e}  ({clock(s, rate)} - {clock(e, rate)})"
            raw = f"raw {i['loop'][0]}-{i['loop'][1]}, delay {i['fact_offset']}"
        else:
            loop, raw = "none", ""
        rows.append((stem, i, loop, raw))
        print(f"{stem:<22} {clock(i['samples'], rate):>10}  {i['channels']}ch {rate}Hz  loop {loop}  {raw}")

        if argv[1] == "decode":
            ext = "flac" if flac else "wav"
            target = os.path.join(out, stem + "." + ext)
            cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                   "-i", os.path.join(src, name)]
            cmd += ["-c:a", "flac"] if flac else ["-c:a", "pcm_s16le"]
            subprocess.run(cmd + [target], check=True)

    print(f"\n{len(rows)} tracks, {looped} with a loop")
    if argv[1] == "decode":
        report = os.path.join(out, "loops.csv")
        with open(report, "w") as f:
            f.write("track,samples,rate,channels,loop_start,loop_end,loop_start_time,loop_end_time\n")
            for stem, i, _, _ in rows:
                s, e = i.get("game_loop", ("", ""))
                f.write(f"{stem},{i['samples']},{i['rate']},{i['channels']},{s},{e},"
                        f"{clock(s, i['rate']) if s != '' else ''},"
                        f"{clock(e, i['rate']) if e != '' else ''}\n")
        print(f"audio and loops.csv written to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
