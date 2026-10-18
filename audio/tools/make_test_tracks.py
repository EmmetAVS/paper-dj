#!/usr/bin/env python3
"""Generates two simple loop tracks (no numpy needed) so the engine can be
tested before you have real music on the Pi.

  python3 tools/make_test_tracks.py      -> tracks/testA_120bpm.wav, tracks/testB_124bpm.wav
"""
import math, os, random, struct, wave

RATE = 48000

def render(bpm, bars, bass_notes, hat_every, seed, path):
    random.seed(seed)
    beat = 60.0 / bpm
    n = int(bars * 4 * beat * RATE)
    left = [0.0] * n
    right = [0.0] * n

    def add(start, samples, pan=0.5):
        for i, s in enumerate(samples):
            j = start + i
            if j >= n: break
            left[j] += s * (1 - pan) * 2 * 0.5
            right[j] += s * pan * 2 * 0.5

    kick = [math.sin(2 * math.pi * (50 + 120 * math.exp(-t / 2000)) * t / RATE) * math.exp(-t / 9000)
            for t in range(int(0.35 * RATE))]
    hat = [(random.random() * 2 - 1) * math.exp(-t / 900) * 0.35 for t in range(int(0.08 * RATE))]
    clap = [(random.random() * 2 - 1) * math.exp(-t / 3000) * 0.4 for t in range(int(0.2 * RATE))]

    steps = bars * 16
    for k in range(steps):
        t = int(k * beat / 4 * RATE)
        if k % 4 == 0: add(t, kick)
        if k % hat_every == hat_every // 2: add(t, hat, pan=0.7)  # offbeat hats
        if k % 8 == 4: add(t, clap, pan=0.4)
        if k % 2 == 0:  # bass on 8th notes
            freq = bass_notes[(k // 16) % len(bass_notes)]
            dur = int(beat / 2 * RATE * 0.9)
            add(t, [0.3 * math.tanh(3 * math.sin(2 * math.pi * freq * i / RATE)) * math.exp(-i / 6000)
                    for i in range(dur)])

    peak = max(max(abs(x) for x in left), max(abs(x) for x in right)) or 1
    g = 0.8 / peak
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<hh", int(l * g * 32767), int(r * g * 32767))
                               for l, r in zip(left, right)))
    print(f"wrote {path} ({n / RATE:.1f} s)")

if __name__ == "__main__":
    os.makedirs("tracks", exist_ok=True)
    render(120, 16, [55.0, 55.0, 65.41, 49.0], 4, 1, "tracks/testA_120bpm.wav")   # A, A, C, G
    render(124, 16, [73.42, 61.74, 82.41, 73.42], 2, 2, "tracks/testB_124bpm.wav")  # D, B, E, D


def write_wav(path, frames):
    peak = max(max(abs(l), abs(r)) for l, r in frames) or 1
    g = 0.8 / peak
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<hh", int(l * g * 32767), int(r * g * 32767)) for l, r in frames))
    print(f"wrote {path} ({len(frames) / RATE:.1f} s)")


def air_horn(path):
    """Three-blast air horn: stacked detuned saws with a little vibrato."""
    out = []
    for blast, dur in ((0, 0.18), (0.22, 0.18), (0.44, 0.7)):
        start = int(blast * RATE)
        while len(out) < start: out.append((0.0, 0.0))
        for i in range(int(dur * RATE)):
            t = i / RATE
            env = min(1, t / 0.01) * min(1, (dur - t) / 0.05)
            vib = 1 + 0.004 * math.sin(2 * math.pi * 6 * t)
            s = sum(((f * vib * t) % 1.0) * 2 - 1 for f in (466.0, 470.0, 699.0, 704.0)) / 4
            out.append((s * env, s * env))
    write_wav(path, out)


def laser_zap(path):
    """Downward 'pew' sweep with a noisy tail."""
    random.seed(3)
    out, phase, dur = [], 0.0, 0.6
    for i in range(int(dur * RATE)):
        t = i / RATE
        f = 2400 * math.exp(-t * 7) + 80
        phase += f / RATE
        env = math.exp(-t * 4)
        s = (math.sin(2 * math.pi * phase) + 0.15 * (random.random() * 2 - 1)) * env
        out.append((s, s * 0.9))
    write_wav(path, out)


if __name__ == "__main__":
    air_horn("tracks/sample1_airhorn.wav")
    laser_zap("tracks/sample2_zap.wav")
