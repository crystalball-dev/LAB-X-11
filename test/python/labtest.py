"""Shared helpers for the LAB X-16 test suite (test/run_tests.py and test/python/effect_tests.py)."""

import math
import pathlib

import numpy as np
import soundfile as sf

import labanalyze as an

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
BUILD = ROOT / "build"
OUT = ROOT / "test" / "out"


def sets(*assignments):
    out = []
    for a in assignments:
        out += ["--set", a]
    return out


def fixture(name, sr, seconds, parts):
    """A WAV in test/out/fixtures. Parts are (start, end, kind, freq, level_db):
    'sine' (level_db is its peak), 'noise' (white, level_db is its RMS), 'impulse' (one sample at start),
    'clipsine' (a full-scale sine hard-clipped at level_db), 'lowpass_noise' (white noise brick-walled below freq)."""
    path = OUT / "fixtures" / f"{name}.wav"
    path.parent.mkdir(parents=True, exist_ok=True)
    n = int(seconds * sr)
    x = np.zeros(n)
    rng = np.random.default_rng(12345)
    for start, end, kind, freq, level in parts:
        a, b = int(start * sr), int(end * sr)
        amp = 10 ** (level / 20)
        if kind == "impulse":
            x[a] += amp
            continue
        t = np.arange(b - a) / sr
        if kind == "sine":
            seg = amp * np.sin(2 * math.pi * freq * t)
        elif kind == "clipsine":
            seg = np.clip(np.sin(2 * math.pi * freq * t), -amp, amp)
        elif kind == "lowpass_noise":
            w = rng.standard_normal(b - a)
            spec = np.fft.rfft(w)
            spec[np.fft.rfftfreq(len(w), 1 / sr) > freq] = 0
            seg = np.fft.irfft(spec, len(w))
            seg = amp * seg / (np.sqrt(np.mean(seg ** 2)) + 1e-30)
        else:
            seg = amp * rng.uniform(-1.0, 1.0, b - a) * math.sqrt(3.0)
        ramp = min(int(0.002 * sr), (b - a) // 2)   # 2 ms edges: the burst itself must not click
        if ramp > 0 and kind != "clipsine":
            w = np.ones(b - a)
            w[:ramp] = np.linspace(0, 1, ramp)
            w[-ramp:] = np.linspace(1, 0, ramp)
            seg = seg * w
        x[a:b] += seg
    sf.write(str(path), np.stack([x, x], axis=1), sr, subtype="FLOAT")
    return path


def mono_of(wav):
    data, sr = an.load(wav)
    return an.mono(data), data, sr


def instance_wav(wav, k):
    """The k-th instance's file of a multi-instance render (the harness appends _k)."""
    p = pathlib.Path(wav)
    return p.with_name(f"{p.stem}_{k}{p.suffix}")


def peak_freq(x, sr, lo, hi, n=None):
    """The frequency of the largest spectral peak between lo and hi (parabolic interpolation on the log spectrum)."""
    n = n or (1 << int(math.floor(math.log2(len(x)))))
    seg = np.asarray(x[:n]) * np.hanning(n)
    spec = np.abs(np.fft.rfft(seg))
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    sel = np.where((freqs >= lo) & (freqs <= hi))[0]
    k = sel[int(np.argmax(spec[sel]))]
    if 0 < k < len(spec) - 1:
        a, b, c = np.log(spec[k - 1] + 1e-30), np.log(spec[k] + 1e-30), np.log(spec[k + 1] + 1e-30)
        d = a - 2 * b + c
        return (k + (0.5 * (a - c) / d if d != 0 else 0.0)) * sr / n
    return float(freqs[k])


def envelope_period(x, sr, frame_s, lo_s, hi_s):
    """The period (s) of an envelope's repetition, from its autocorrelation between lo_s and hi_s."""
    env = an.envelope(x, sr, frame_s)
    e = env - env.mean()
    ac = np.correlate(e, e, mode="full")[len(e) - 1:]
    lo, hi = int(lo_s / frame_s), int(hi_s / frame_s)
    k = lo + int(np.argmax(ac[lo:hi]))
    a, b, c = ac[k - 1], ac[k], ac[k + 1]
    d = a - 2 * b + c
    return (k + (0.5 * (a - c) / d if d != 0 else 0.0)) * frame_s


def cents(f, ref):
    return 1200.0 * math.log2(f / ref)
