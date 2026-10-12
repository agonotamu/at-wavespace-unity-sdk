"""Génère les signaux de test du banc (WAV mono 48 kHz, 24 bits, stdlib + numpy).

    python tools/perf/scripts/gen_test_signals.py signals

- pink_10s.wav            : bruit rose court (tient en cache, lecture fichier minimale)
- pink_120s.wav           : bruit rose long (stresse la lecture disque/décodage, H3)
- decay_to_silence.wav    : impulsions qui décroissent puis silence numérique strict (dénormaux, H2)
- sweep_10s.wav           : sinus glissant 20 Hz–20 kHz (contrôle à l'oreille)
"""
import sys
import wave
from pathlib import Path

import numpy as np

FS = 48000


def pink(n, rng):
    # filtre de Paul Kellet (approx. -3 dB/oct)
    w = rng.standard_normal(n)
    b = np.zeros(7)
    out = np.empty(n)
    for i, x in enumerate(w):
        b[0] = 0.99886 * b[0] + x * 0.0555179
        b[1] = 0.99332 * b[1] + x * 0.0750759
        b[2] = 0.96900 * b[2] + x * 0.1538520
        b[3] = 0.86650 * b[3] + x * 0.3104856
        b[4] = 0.55000 * b[4] + x * 0.5329522
        b[5] = -0.7616 * b[5] - x * 0.0168980
        out[i] = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + x * 0.5362
        b[6] = x * 0.115926
    return out


def write(path, x, peak=0.5):
    x = x / (np.max(np.abs(x)) + 1e-12) * peak
    pcm = np.clip(np.round(x * (2**23 - 1)), -(2**23), 2**23 - 1).astype(np.int32)
    raw = pcm.astype("<i4").view(np.uint8).reshape(-1, 4)[:, :3].tobytes()  # 24 bits LE
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(3)
        w.setframerate(FS)
        w.writeframes(raw)
    print(f"{path}  ({len(x) / FS:.1f} s)")


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "signals")
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(1)

    p = pink(FS * 10, rng)
    write(out / "pink_10s.wav", p)
    write(out / "pink_120s.wav", np.tile(pink(FS * 30, rng), 4))

    t = np.arange(FS * 10) / FS
    x = np.zeros_like(t)
    for k in range(5):  # 5 rafales, chacune suivie de silence exact
        t0 = k * 2.0
        m = (t >= t0) & (t < t0 + 0.4)
        x[m] = rng.standard_normal(m.sum()) * np.exp(-(t[m] - t0) * 15)
    write(out / "decay_to_silence.wav", x)

    f0, f1, T = 20.0, 20000.0, 10.0
    k = np.log(f1 / f0)
    write(out / "sweep_10s.wav", np.sin(2 * np.pi * f0 * T / k * (np.exp(t * k / T) - 1)), peak=0.3)


if __name__ == "__main__":
    main()
