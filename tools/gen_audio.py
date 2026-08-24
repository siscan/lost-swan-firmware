#!/usr/bin/env python3
"""Synthesized placeholder cues (spec 9), so the audio pipeline works before
Nico's Swan recordings exist.

These are NOT the show's sounds and are not trying to be. They are three
distinguishable, obviously-synthetic tones whose only job is to prove that the
right cue fires at the right moment, at the right volume, through the right
speaker. Every one of them is meant to be replaced: upload a WAV from
Settings -> Audio and it lands in the same place with the same name, no
reflash.

    python tools/gen_audio.py [--out audio]

16-bit mono PCM at 22050 Hz (spec 9).
"""
import argparse
import math
import pathlib
import struct

RATE = 22050


def wav(path, samples):
    data = b"".join(struct.pack("<h", max(-32768, min(32767, int(s)))) for s in samples)
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)
    return len(data) + 44


def env(i, n, attack=0.01, release=0.08):
    """A short attack and release, because a square-edged tone clicks."""
    a = int(RATE * attack)
    r = int(RATE * release)
    if i < a:
        return i / a
    if i > n - r:
        return max(0.0, (n - i) / r)
    return 1.0


def tone(freq, seconds, amp=0.35, harmonic=0.0):
    n = int(RATE * seconds)
    for i in range(n):
        t = i / RATE
        v = math.sin(2 * math.pi * freq * t)
        if harmonic:
            v += harmonic * math.sin(2 * math.pi * freq * 2 * t)
        yield 32767 * amp * env(i, n) * v / (1 + harmonic)


def silence(seconds):
    for _ in range(int(RATE * seconds)):
        yield 0


def chain(*gens):
    for g in gens:
        yield from g


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="audio")
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    # Deliberately distinguishable by ear alone, so a bench test can tell which
    # cue fired without looking at the console.
    files = {
        # 4:00 - two calm mid tones.
        "warn_4min.wav": chain(tone(660, 0.18), silence(0.10), tone(660, 0.18)),
        # 1:00 - three, higher and faster. More urgent without being an alarm.
        "warn_1min.wav": chain(tone(880, 0.12), silence(0.07), tone(880, 0.12),
                               silence(0.07), tone(880, 0.12)),
        # Zero - the alarm. Loops, bounded by countdown.failure_loop_s, so this
        # is one cycle: a two-tone warble, harsh on purpose.
        "system_failure.wav": chain(tone(440, 0.35, amp=0.5, harmonic=0.6),
                                    tone(587, 0.35, amp=0.5, harmonic=0.6)),
        # UI. Short enough not to be in the way; these never interrupt a
        # warning (see cue_policy).
        "ui_execute.wav": tone(1320, 0.05, amp=0.22),
        "ui_reject.wav": chain(tone(220, 0.09, amp=0.28, harmonic=0.8)),
    }

    total = 0
    for name, gen in files.items():
        n = wav(out / name, gen)
        total += n
        print("  %-22s %6d B" % (name, n))
    print("wrote %d placeholder cues, %d bytes total, into %s/" % (len(files), total, out))
    print("These are placeholders. Replace them from Settings -> Audio; no reflash needed.")


if __name__ == "__main__":
    main()
