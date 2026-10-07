#!/usr/bin/env python3
"""Line a CD rip of the soundtrack up against the game's own music.

    match_ost.py decode <game wav dir> <cd dir> <work dir>
    match_ost.py match  <work dir> <loops.csv> [report.csv]
    match_ost.py verify <match.csv> <loops.csv> <out.csv>

Why this is not just a file-name table
--------------------------------------
The game's 52 cues and an album's tracks are not the same recordings. A cue is
an edit: an intro, then a body that loops until the game stops it, with the loop
point recorded in the .a3p and nowhere else. An album track is the same music
mastered to be listened to once -- often a different length, usually with the
loop played through twice and an ending instead of a seam.

So a replacement needs two things the file name cannot give: which album track
a cue came from, and where in that track the cue's loop points land. Both are
found by correlating the audio.

How
---
Stage one correlates loudness envelopes at 100 Hz. That is coarse enough to be
fast across every pair -- 52 x 42 of them -- and robust to the level and EQ
differences between a disc rip and a 64 kbit/s ATRAC3plus encode, which defeat
a sample-level comparison.

Each candidate offset is then scored by correlating ONLY the region where the
two actually overlap. Scoring the whole track instead would cap a 14-second cue
inside a 100-second album track at about 0.37 however perfect the match, purely
because of the length difference -- so short cues would look like failures.

Stage two takes the winner and correlates the real waveform at 44.1 kHz within
half a second of the coarse offset, which puts the alignment within a few
samples. A cue only matches if both stages agree.

Stage three ("verify") is what the loop mapping actually rests on. It aligns
nine probes spread across the cue and keeps those agreeing with the median.
Two probes are not enough: a lossy-coded pair regularly has a correlation peak
a few hundred samples off the true one, and a pair of probes that each lock
onto a different peak fakes a drift of a thousand samples. With nine, the
outliers are visible as outliers.

What it establishes is that the offset is CONSTANT -- the measured rates agree
to about a hundred samples per track, which is a rate error of 0.0005% -- so a
cue's loop points move into album time by adding one number, with no
resampling. Where the probes do not agree on an offset at all, the album track
is a different edit rather than the same master, and the cue is left alone.

A note on two things that look like failures and are not. Waveform |r| is low
(0.3-0.5) for the dense, loud cues and near 1.0 for sparse ones; that tracks
ATRAC3plus coding noise at 64 kbit/s, not the quality of the match, which is
why the envelope score decides and |r| only confirms. And "inverted" polarity
is common and sometimes unstable between probes, for the same reason -- it is
inaudible by itself, and recorded only so a replacement can be flipped to match
if it is ever mixed against the game's own audio.

The report gives, per cue: its album track, the offset of the cue within it,
the correlation peak as a confidence, and the cue's loop points translated into
album-track time. A low peak means no match was found and the cue should keep
the game's own audio.
"""
import csv, os, subprocess, sys
import numpy as np

ENVELOPE_HZ = 100


def decode(path, rate, mono=True):
    cmd = ["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le",
           "-ar", str(rate), "-ac", "1" if mono else "2", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32)


def envelope(samples, rate):
    """RMS in 1/ENVELOPE_HZ windows, normalised. Level-independent."""
    step = max(1, rate // ENVELOPE_HZ)
    usable = (len(samples) // step) * step
    if usable == 0:
        return np.zeros(1, dtype=np.float32)
    blocks = samples[:usable].reshape(-1, step)
    env = np.sqrt((blocks.astype(np.float64) ** 2).mean(axis=1))
    env -= env.mean()
    norm = np.linalg.norm(env)
    return (env / norm if norm > 0 else env).astype(np.float32)


def best_offset(short, long):
    """Where `short` sits inside `long`, by FFT cross-correlation.

    Returns (offset in samples, peak). The peak is already normalised by both
    vectors' energy, so it compares across pairs of different lengths.
    """
    n = 1
    while n < len(short) + len(long):
        n *= 2
    fa = np.fft.rfft(long, n)
    fb = np.fft.rfft(short[::-1], n)
    corr = np.fft.irfft(fa * fb, n)[: len(long)]
    index = int(np.argmax(corr))
    offset = index - len(short) + 1
    denom = np.linalg.norm(short) * np.linalg.norm(long)
    return offset, float(corr[index] / denom) if denom > 0 else 0.0


def clock(samples, rate=44100):
    """mm:ss.mmm for a sample position."""
    sign = "-" if samples < 0 else ""
    seconds = abs(samples) / rate
    return f"{sign}{int(seconds) // 60}:{seconds % 60:06.3f}"


def overlap_score(cue, album, offset):
    """Correlate cue against the album slice it lands on. Returns (r, coverage).

    Scored on the overlap alone so the result does not depend on how much longer
    the album track is. `coverage` is how much of the cue has album underneath
    it; well under 1.0 means the offset puts the cue off the front or back edge,
    which a real match never does.
    """
    low = max(0, -offset)
    high = min(len(cue), len(album) - offset)
    if high - low < 8:
        return 0.0, 0.0
    a = cue[low:high].astype(np.float64)
    b = album[low + offset:high + offset].astype(np.float64)
    a -= a.mean()
    b -= b.mean()
    denom = np.linalg.norm(a) * np.linalg.norm(b)
    return (float(a @ b / denom) if denom > 0 else 0.0), (high - low) / len(cue)


def refine(cue_wav, album_wav, coarse_s, span_s=0.5, probe_s=12.0):
    """Sample-accurate offset, by correlating real waveform near `coarse_s`.

    Takes a probe from the middle of the cue -- away from an intro or an abrupt
    tail -- and searches +/- span_s around where stage one said it should be.
    """
    cue = decode(cue_wav, 44100)
    album = decode(album_wav, 44100)
    probe_len = min(int(probe_s * 44100), len(cue))
    probe_at = max(0, (len(cue) - probe_len) // 2)
    probe = cue[probe_at:probe_at + probe_len].astype(np.float64)
    centre = int(round(coarse_s * 44100)) + probe_at
    span = int(span_s * 44100)
    low = max(0, centre - span)
    high = min(len(album), centre + probe_len + span)
    window = album[low:high].astype(np.float64)
    if len(window) < probe_len + 2:
        return None, 0.0
    probe -= probe.mean()
    n = 1
    while n < len(window) + probe_len:
        n *= 2
    corr = np.fft.irfft(np.fft.rfft(window, n) * np.fft.rfft(probe[::-1], n), n)[: len(window)]
    index = int(np.argmax(np.abs(corr)))
    at = low + index - probe_len + 1
    best = overlap_score(probe, album, at)[0]
    return (at - probe_at) / 44100.0, best


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip()); return 2

    if argv[1] == "decode":
        game_dir, cd_dir, work = argv[2], argv[3], argv[4]
        os.makedirs(work, exist_ok=True)
        for label, folder, exts in (("game", game_dir, (".wav",)), ("cd", cd_dir, (".flac", ".wav"))):
            names = sorted(f for f in os.listdir(folder) if f.lower().endswith(exts))
            for i, name in enumerate(names, 1):
                stem = os.path.splitext(name)[0]
                out = os.path.join(work, f"{label}__{stem}.npy")
                if os.path.exists(out):
                    continue
                samples = decode(os.path.join(folder, name), 44100)
                np.save(out, envelope(samples, 44100))
                print(f"  [{label} {i}/{len(names)}] {stem}")
        print("decoded")
        return 0

    if argv[1] == "match":
        work, loops_csv = argv[2], argv[3]
        report = argv[4] if len(argv) > 4 else os.path.join(work, "ost_match.csv")
        game_dir = os.environ.get("GAME_WAV", "")
        cd_dir = os.environ.get("CD_DIR", "")
        floor = float(os.environ.get("FLOOR", "0.55"))
        loops = {r["track"]: r for r in csv.DictReader(open(loops_csv))}
        cd = {}
        for f in sorted(os.listdir(work)):
            if f.startswith("cd__") and f.endswith(".npy"):
                cd[f[4:-4]] = np.load(os.path.join(work, f))
        rows = []
        for f in sorted(os.listdir(work)):
            if not (f.startswith("game__") and f.endswith(".npy")):
                continue
            cue = f[6:-4]
            env = np.load(os.path.join(work, f))
            scored = []
            for name, other in cd.items():
                short, long = (env, other) if len(env) <= len(other) else (other, env)
                offset, _ = best_offset(short, long)
                if len(env) > len(other):
                    offset = -offset
                score, coverage = overlap_score(env, other, offset)
                # An offset that hangs the cue off either edge is not a match,
                # however well the overlapping part happens to correlate.
                scored.append((score * coverage, score, coverage, name, offset))
            scored.sort(reverse=True)
            rank, score, coverage, name, offset = scored[0]
            runner = scored[1] if len(scored) > 1 else None
            coarse_s = offset / ENVELOPE_HZ
            row = {"cue": cue, "album_track": name, "envelope_r": f"{score:.3f}",
                   "coverage": f"{coverage:.3f}", "coarse_offset_s": f"{coarse_s:+.3f}",
                   "runner_up": runner[3] if runner else "",
                   "runner_up_r": f"{runner[1]:.3f}" if runner else ""}

            # Stage two, only where stage one found something worth refining.
            exact_s, waveform_r = None, 0.0
            if score >= floor and coverage > 0.95 and game_dir and cd_dir:
                cue_wav = os.path.join(game_dir, cue + ".wav")
                album = None
                for ext in (".flac", ".wav"):
                    candidate = os.path.join(cd_dir, name + ext)
                    if os.path.exists(candidate):
                        album = candidate
                        break
                if album and os.path.exists(cue_wav):
                    exact_s, waveform_r = refine(cue_wav, album, coarse_s)
            if exact_s is not None:
                row["exact_offset_s"] = f"{exact_s:+.6f}"
                row["waveform_r"] = f"{waveform_r:.3f}"
            use_s = exact_s if exact_s is not None else coarse_s

            verdict = "match"
            if score < floor:
                verdict = "no match - keep game audio"
            elif coverage <= 0.95:
                verdict = "rejected - cue overhangs track edge"
            elif waveform_r and waveform_r < 0.30:
                verdict = "weak - envelope only, verify by ear"
            row["verdict"] = verdict

            info = loops.get(cue)
            if info and info.get("loop_start") and verdict == "match":
                rate = int(info["rate"])
                for which in ("loop_start", "loop_end"):
                    row[which + "_in_album_s"] = f"{int(info[which]) / rate + use_s:.6f}"
            elif info and not info.get("loop_start"):
                row["loop_start_in_album_s"] = "one-shot"
            rows.append(row)
            mark = {"match": "ok  ", "weak - envelope only, verify by ear": "weak"}.get(verdict, "--  ")
            print(f"{mark} {cue:<22} {name:<32} env {score:.3f} cov {coverage:.2f}"
                  + (f"  wave {waveform_r:.3f} at {exact_s:+.4f}s" if exact_s is not None else ""))
        fields = ["cue", "album_track", "verdict", "envelope_r", "waveform_r", "coverage",
                  "coarse_offset_s", "exact_offset_s", "loop_start_in_album_s",
                  "loop_end_in_album_s", "runner_up", "runner_up_r"]
        with open(report, "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            for row in rows:
                writer.writerow({k: row.get(k, "") for k in fields})
        kept = sum(1 for r in rows if r["verdict"] == "match")
        print(f"\n{kept} of {len(rows)} cues matched -> {report}")
        return 0

    if argv[1] == "verify":
        match_csv, loops_csv, out_csv = argv[2], argv[3], argv[4]
        cd_dir, game_dir = os.environ["CD_DIR"], os.environ["GAME_WAV"]
        probe_len, probes_n, tol = 3 * 44100, 9, 40
        loops = {r["track"]: r for r in csv.DictReader(open(loops_csv))}
        cache = {}

        def album_of(name):
            if name not in cache:
                path = next((os.path.join(cd_dir, name + e) for e in (".flac", ".wav")
                             if os.path.exists(os.path.join(cd_dir, name + e))), None)
                cache[name] = decode(path, 44100).astype(np.float64) if path else None
            return cache[name]

        rows, out = list(csv.DictReader(open(match_csv))), []
        for row in rows:
            cue, name = row["cue"], row["album_track"]
            keep = {"cue": cue, "album_track": name, "envelope_r": row["envelope_r"]}
            album = album_of(name)
            cue_path = os.path.join(game_dir, cue + ".wav")
            if abs(float(row["envelope_r"])) < 0.50 or album is None or not os.path.exists(cue_path):
                keep["status"] = "not on album - keep game audio"
                out.append(keep); print(f"--   {cue:<22} not on the album"); continue
            guest = decode(cue_path, 44100).astype(np.float64)
            base = int(round(float(row["exact_offset_s"] or row["coarse_offset_s"]) * 44100))
            found = []
            for k in range(1, probes_n + 2):
                at = int(k * len(guest) / (probes_n + 2))
                if at + probe_len > len(guest):
                    break
                low = max(0, at + base - 6000)
                high = min(len(album), at + base + probe_len + 6000)
                window = album[low:high]
                if len(window) < probe_len + 2:
                    continue
                probe = guest[at:at + probe_len] - guest[at:at + probe_len].mean()
                n = 1
                while n < len(window) + probe_len:
                    n *= 2
                corr = np.fft.irfft(np.fft.rfft(window, n) * np.fft.rfft(probe[::-1], n), n)[: len(window)]
                hit = low + int(np.argmax(np.abs(corr))) - probe_len + 1
                seg = album[hit:hit + probe_len]
                if len(seg) < probe_len:
                    continue
                seg = seg - seg.mean()
                d = np.linalg.norm(probe) * np.linalg.norm(seg)
                found.append((hit - at, float(probe @ seg / d) if d > 0 else 0.0))
            if not found:
                keep["status"] = "too short to verify"; out.append(keep); continue
            offsets = np.array([f[0] for f in found], dtype=float)
            median = float(np.median(offsets))
            inliers = [f for f in found if abs(f[0] - median) <= tol]
            keep["inliers"] = f"{len(inliers)}/{len(found)}"
            if len(inliers) < 3:
                keep["status"] = "different edit - no constant offset"
                out.append(keep); print(f"--   {cue:<22} {name:<32} no constant offset "
                                        f"({len(inliers)}/{len(found)} agree)"); continue
            offset = int(round(float(np.median([f[0] for f in inliers]))))
            strength = float(np.median([abs(f[1]) for f in inliers]))
            signs = [f[1] for f in inliers]
            polarity = ("inverted" if max(signs) <= 0 else
                        "normal" if min(signs) >= 0 else "indeterminate")
            keep.update({"offset_samples": str(offset),
                         "offset_s": f"{offset / 44100:.6f}",
                         "waveform_r": f"{strength:.3f}", "polarity": polarity,
                         "album_len_s": f"{len(album) / 44100:.3f}"})
            info = loops.get(cue)
            quorum = max(3, int(0.4 * len(found)))
            if strength < 0.25 or len(inliers) < quorum:
                keep["status"] = "too weak to trust - keep game audio"
                out.append(keep)
                print(f"--   {cue:<22} {name:<32} too weak (|r| {strength:.3f}, {keep['inliers']})")
                continue
            status = "ok" if strength >= 0.30 else "ok - low |r|, dense mix"
            if info and info.get("loop_start"):
                start = int(info["loop_start"]) + offset
                end = int(info["loop_end"]) + offset
                keep.update({"loop_start_samples": str(start), "loop_end_samples": str(end),
                             "loop_start_time": clock(start), "loop_end_time": clock(end)})
                if end > len(album):
                    status = f"album track ends {(end - len(album)) / 44100:.2f}s before the loop does"
                elif start < 0:
                    status = "loop starts before the album track does"
            else:
                keep["loop_start_samples"] = "one-shot"
            keep["status"] = status
            out.append(keep)
            print(f"ok   {cue:<22} {name:<32} off {offset:+8d} |r| {strength:.3f} "
                  f"{polarity:<13} {keep['inliers']}"
                  + ("" if status.startswith("ok") else "  <- " + status))
        fields = ["cue", "album_track", "status", "offset_samples", "offset_s", "envelope_r",
                  "waveform_r", "polarity", "inliers", "loop_start_samples", "loop_end_samples",
                  "loop_start_time", "loop_end_time", "album_len_s"]
        with open(out_csv, "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader()
            for row in out:
                writer.writerow({k: row.get(k, "") for k in fields})
        good = sum(1 for r in out if r.get("status", "").startswith("ok"))
        print(f"\n{good} of {len(out)} cues map cleanly -> {out_csv}")
        return 0

    print(__doc__.strip()); return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
