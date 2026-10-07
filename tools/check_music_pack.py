#!/usr/bin/env python3
"""Check that a built music pack really lines up with the cues it replaces.

    check_music_pack.py <a3p dir> <music pack dir> [cue ...]

build_music_pack.py bakes an offset into every file so the game can keep using
its own loop points. Three independent numbers go into that offset -- the
correlation result, the encoder and decoder delay, and the loop length used to
fill a short tail -- and a mistake in any of them is inaudible in the file and
obvious in the game. This checks the result rather than the inputs: decode the
cue from its .a3p, find where it sits inside the replacement, and report the
error in samples. Zero means the replacement is exactly where the runtime will
look for it.

On probe width, which is not a detail
-------------------------------------
The probe has to be narrow and taken from inside the music. An earlier version
of this check used an eight-second probe and reported five of the game's short
cues as 25 ms out. They were not: a five-second one-shot is shorter than the
probe, so the window spanned the silent lead-in and the padded tail as well as
the music, and the correlation settled on a false peak. Re-measured with a
one-second probe from the middle, every one of them came back exact. A
measurement that disagrees with the thing it measures is usually the
measurement.
"""
import os, subprocess, sys
import numpy as np

RATE = 44100
ATRAC3, ATRAC3PLUS = 0x0270, 0xFFFE
DELAY = {ATRAC3: 69, ATRAC3PLUS: 368}


def decode(path):
    out = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le",
                          "-ar", str(RATE), "-ac", "1", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype=np.float32).astype(np.float64)


def skip_of(a3p):
    """The delay the decoded stream carries before its first playable sample."""
    import struct
    raw = open(a3p, "rb").read(0x1000)
    offset, tag, fact_offset = 12, ATRAC3PLUS, 0
    while offset + 8 <= len(raw):
        cid = raw[offset:offset + 4]
        size = struct.unpack_from("<I", raw, offset + 4)[0]
        body = offset + 8
        if cid == b"data":
            break
        if cid == b"fmt " and size >= 16:
            tag = struct.unpack_from("<H", raw, body)[0]
        elif cid == b"fact" and size >= 8:
            fact_offset = struct.unpack_from("<I", raw, body + 4)[0]
        offset = body + size + (size & 1)
    return fact_offset + DELAY.get(tag, DELAY[ATRAC3PLUS])


def residual(cue_audio, replacement, skip, probe_seconds=1.0, span=8000):
    """Median error, in samples, over probes taken from inside the music."""
    width = int(probe_seconds * RATE)
    if len(cue_audio) < width + 2000:
        width = max(RATE // 4, len(cue_audio) // 3)
    found = []
    for fraction in (0.3, 0.5, 0.7):
        at = int(fraction * (len(cue_audio) - width))
        probe = cue_audio[at:at + width]
        if np.sqrt((probe ** 2).mean()) < 1e-4:
            continue  # silence carries no alignment
        probe = probe - probe.mean()
        low = max(0, at + skip - span)
        high = min(len(replacement), at + skip + width + span)
        window = replacement[low:high]
        if len(window) < width + 2:
            continue
        size = 1
        while size < len(window) + width:
            size *= 2
        correlation = np.fft.irfft(np.fft.rfft(window, size) *
                                   np.fft.rfft(probe[::-1], size), size)[:len(window)]
        hit = low + int(np.argmax(np.abs(correlation))) - width + 1
        segment = replacement[hit:hit + width]
        if len(segment) < width:
            continue
        segment = segment - segment.mean()
        scale = np.linalg.norm(probe) * np.linalg.norm(segment)
        found.append((hit - (at + skip), abs(float(probe @ segment / scale)) if scale else 0.0))
    if not found:
        return None, 0.0
    return int(np.median([f[0] for f in found])), float(np.median([f[1] for f in found]))


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip()); return 2
    a3p_dir, pack_dir = argv[1], argv[2]
    wanted = set(argv[3:])
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "build", os.path.join(os.path.dirname(os.path.abspath(__file__)), "build_music_pack.py"))
    build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)

    checked = exact = off = 0
    for name in sorted(os.listdir(a3p_dir)):
        if not name.lower().endswith(".a3p"):
            continue
        cue = os.path.splitext(name)[0]
        if wanted and cue not in wanted:
            continue
        a3p = os.path.join(a3p_dir, name)
        info = build.describe(a3p)
        replacement = os.path.join(pack_dir, f"{info['key']:016x}.wav")
        if not os.path.exists(replacement):
            continue
        error, strength = residual(decode(a3p), decode(replacement), skip_of(a3p))
        checked += 1
        if error is None:
            print(f"  {cue:<22} could not be measured"); continue
        verdict = "exact" if abs(error) <= 2 else ("close" if abs(error) <= 50 else "OFF")
        if verdict == "exact": exact += 1
        elif verdict == "OFF": off += 1
        print(f"  {cue:<22} {error:+6d} samples ({error / RATE * 1000:+6.1f} ms)  "
              f"|r| {strength:.3f}  {verdict}")
    print(f"\n{checked} replacements checked: {exact} exact, {off} misaligned")
    return 1 if off else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
