#!/usr/bin/env python3
"""Generates the tiny WAV files of audio_demo (8-bit mono PCM, 11025 Hz; stdlib only).

Run from anywhere:  python3 examples/basics/12_audio/make_sounds.py
The output is deterministic, so re-running it leaves the committed files unchanged.
"""
import math
import os
import struct

RATE = 11025
HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")


def write_wav(rel, samples):
    """samples: floats in [-1, 1]."""
    data = bytes(max(0, min(255, int(round(128 + s * 120)))) for s in samples)
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE, 1, 8)
    header += b"data" + struct.pack("<I", len(data))
    path = os.path.join(ASSETS, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header + data)


def tone(freqs, seconds, decay=0.0, volume=0.6, attack=0.005):
    n = int(seconds * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        env = min(1.0, t / attack) if attack > 0 else 1.0
        if decay > 0:
            env *= math.exp(-t / decay)
        tail = min(1.0, (n - i) / (0.01 * RATE))  # 10 ms fade-out: no click
        s = sum(math.sin(2 * math.pi * f * t) for f in freqs) / len(freqs)
        out.append(s * env * tail * volume)
    return out


def arpeggio(notes, seconds, step):
    """A loop that wraps cleanly: notes of `step` seconds each, each with its own short envelope."""
    n = int(seconds * RATE)
    per = int(step * RATE)
    out = []
    for i in range(n):
        k = (i // per) % len(notes)
        t = (i % per) / RATE
        env = min(1.0, t / 0.01) * math.exp(-t / (step * 0.6))
        f = notes[k]
        s = 0.7 * math.sin(2 * math.pi * f * t) + 0.3 * math.sin(2 * math.pi * f * 0.5 * t)
        out.append(s * env * 0.5)
    return out


def wind(seconds):
    """Deterministic low-passed noise with a slow swell; starts and ends quiet so it loops."""
    n = int(seconds * RATE)
    state = 12345
    y = 0.0
    out = []
    for i in range(n):
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        x = state / 0x7FFFFFFF * 2 - 1
        y += 0.05 * (x - y)
        swell = 0.5 - 0.5 * math.cos(2 * math.pi * i / n)
        out.append(y * (0.4 + 0.6 * swell) * 1.6)
    return out


def main():
    write_wav("sounds/blip_1.wav", tone([660], 0.08, decay=0.03))
    write_wav("sounds/blip_2.wav", tone([880], 0.08, decay=0.03))
    write_wav("sounds/blip_3.wav", tone([990], 0.08, decay=0.03))
    write_wav("sounds/coin.wav", tone([988], 0.06, decay=0.05) + tone([1319], 0.12, decay=0.06))
    write_wav("sounds/hum.wav", tone([110, 220], 0.5, volume=0.5, attack=0.0))
    c, e, g, a, b = 262, 330, 392, 440, 494
    write_wav("music/calm.wav", arpeggio([c, e, g, e * 2, g, e], 2.0, 2.0 / 6))
    write_wav("music/tense.wav", arpeggio([a / 2, c, e, a, e, c, b / 2, e], 2.0, 2.0 / 8))
    write_wav("music/sting.wav", tone([c * 2, e * 2, g * 2], 0.6, decay=0.25, volume=0.8))
    write_wav("ambience/wind.wav", wind(2.0))


if __name__ == "__main__":
    main()
