"""Analysis for WAV files rendered by the LAB X-16 harnesses (merged from LAB X-19 and LAB X-0 analyze.py).

Usage:
    python test/python/labanalyze.py file.wav [file2.wav ...] [--json]

Prints, per file: peak and RMS in dBFS, DC offset, dominant frequency, band shares and the largest sample jump.
The functions are imported by test/run_tests.py: levels, spectra, tone levels, RT60 (Schroeder), onsets, correlation,
loudness (BS.1770) and the largest sample jump.
"""

import argparse
import json
import math
import sys

import numpy as np
import soundfile as sf

BANDS = [("sub<60", 20, 60), ("60-250", 60, 250), ("250-1k", 250, 1000),
         ("1k-4k", 1000, 4000), ("4k-10k", 4000, 10000), (">10k", 10000, 22000)]


def db(x, floor=-240.0):
    return 20.0 * math.log10(x) if x > 0 else floor


def load(path):
    data, sr = sf.read(str(path), always_2d=True)
    data = np.where(np.isfinite(data), data, 0.0)
    return data, sr


def mono(data):
    return data.mean(axis=1)


def rms(x):
    return math.sqrt(float(np.mean(np.asarray(x, dtype=float) ** 2)) + 1e-300)


def rms_db(x):
    return db(rms(x))


def peak_db(x):
    return db(float(np.abs(np.asarray(x)).max()) if len(x) else 0.0)


def segment(x, sr, start, end):
    return x[int(start * sr):int(end * sr)]


def max_jump(data):
    d = np.asarray(data)
    return float(np.abs(np.diff(d, axis=0)).max()) if len(d) > 1 else 0.0


def dominant_frequency(x, sr, fmin=20.0):
    n = 1 << int(math.floor(math.log2(min(len(x), 1 << 18))))
    seg = x[:n] * np.hanning(n)
    spec = np.abs(np.fft.rfft(seg))
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    lo = int(fmin * n / sr)
    k = lo + int(np.argmax(spec[lo:]))
    if 0 < k < len(spec) - 1:
        a, b, c = np.log(spec[k - 1] + 1e-12), np.log(spec[k] + 1e-12), np.log(spec[k + 1] + 1e-12)
        shift = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
        return (k + shift) * sr / n
    return float(freqs[k])


def power_spectrum(x, sr, n=8192):
    """Welch-averaged power spectrum with a Hann window."""
    if len(x) < n:
        n = 1 << int(math.floor(math.log2(max(len(x), 64))))
    win = np.hanning(n)
    acc = np.zeros(n // 2 + 1)
    count = 0
    for i in range(0, len(x) - n + 1, n // 2):
        acc += np.abs(np.fft.rfft(x[i:i + n] * win)) ** 2
        count += 1
    return np.fft.rfftfreq(n, 1.0 / sr), acc / max(count, 1)


def band_energy(x, sr, lo, hi, n=8192):
    freqs, p = power_spectrum(x, sr, n)
    return float(p[(freqs >= lo) & (freqs < hi)].sum())


def band_db(x, sr, lo, hi, n=8192):
    return 10 * math.log10(band_energy(x, sr, lo, hi, n) + 1e-300)


def band_split(x, sr):
    freqs, p = power_spectrum(x, sr)
    total = p.sum() + 1e-30
    return {name: round(10 * math.log10(p[(freqs >= lo) & (freqs < hi)].sum() / total + 1e-30), 1)
            for name, lo, hi in BANDS}


def tone_db(x, sr, freq, width=0.03):
    """Level of the spectrum within +-width (relative) of freq, from one long Hann-windowed FFT (dB, arbitrary ref)."""
    n = 1 << int(math.ceil(math.log2(max(len(x), 64))))
    spec = np.abs(np.fft.rfft(np.asarray(x) * np.hanning(len(x)), n)) ** 2
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    sel = (freqs >= freq * (1 - width)) & (freqs <= freq * (1 + width))
    return 10 * math.log10(float(spec[sel].sum()) + 1e-300)


def envelope(x, sr, frame_s=0.005):
    frame = max(1, int(sr * frame_s))
    usable = len(x) // frame * frame
    return np.sqrt((np.asarray(x[:usable]).reshape(-1, frame) ** 2).mean(axis=1))


def windows_db(x, sr, start, end, width=1.0):
    """RMS in dB of consecutive windows from start to end (seconds)."""
    out = []
    t = start
    while t + width <= end + 1e-9:
        out.append(rms_db(segment(x, sr, t, t + width)))
        t += width
    return out


def schroeder_rt60(x, sr, start_db=-5.0, end_db=-35.0):
    """RT60 from the energy decay curve (backward-integrated squared response), fitted between start_db and end_db.
    x starts at the excitation; returns (rt60_seconds, fit_range_db)."""
    e = np.asarray(x, dtype=float) ** 2
    edc = np.cumsum(e[::-1])[::-1]
    edc_db = 10 * np.log10(edc / (edc[0] + 1e-300) + 1e-300)
    i0 = int(np.argmax(edc_db <= start_db))
    i1 = int(np.argmax(edc_db <= end_db))
    if i1 <= i0 + 10:
        return float("nan"), 0.0
    t = np.arange(i0, i1) / sr
    slope, _ = np.polyfit(t, edc_db[i0:i1], 1)
    return float(-60.0 / slope) if slope < 0 else float("inf"), end_db - start_db


def onset(x, sr, threshold_rel_db=-40.0, start=0.0):
    """First time (s) the absolute signal exceeds threshold_rel_db relative to its peak, from start."""
    a = np.abs(np.asarray(x)[int(start * sr):])
    if len(a) == 0 or a.max() <= 0:
        return float("nan")
    thr = a.max() * 10 ** (threshold_rel_db / 20)
    return start + int(np.argmax(a > thr)) / sr


def local_peaks(x, sr, times, half_window_s):
    """For each expected time, the time of the largest |x| within +-half_window_s."""
    a = np.abs(np.asarray(x))
    out = []
    for t in times:
        lo, hi = max(0, int((t - half_window_s) * sr)), min(len(a), int((t + half_window_s) * sr) + 1)
        out.append((lo + int(np.argmax(a[lo:hi]))) / sr if hi > lo else float("nan"))
    return out


def cross_correlation_lag(a, b, max_lag):
    """Lag (samples) that best aligns b to a: positive means b is later."""
    a = np.asarray(a, dtype=float)
    b = np.asarray(b, dtype=float)
    n = 1 << int(math.ceil(math.log2(len(a) + len(b))))
    c = np.fft.irfft(np.conj(np.fft.rfft(a, n)) * np.fft.rfft(b, n), n)
    lags = np.concatenate([c[:max_lag + 1], c[-max_lag:]]) if max_lag > 0 else c[:1]
    idx = int(np.argmax(lags))
    return idx if idx <= max_lag else idx - len(lags)


def correlation(a, b):
    a = np.asarray(a, dtype=float)
    b = np.asarray(b, dtype=float)
    return float(np.dot(a, b) / (math.sqrt(np.dot(a, a) * np.dot(b, b)) + 1e-300))


def lufs(data, sr, momentary_max=False):
    """Integrated loudness per ITU-R BS.1770-4 (K-weighting, 400 ms blocks, absolute and relative gates).
    momentary_max: the loudest 400 ms block instead."""
    from scipy.signal import lfilter
    x = np.asarray(data, dtype=float)
    if x.ndim == 1:
        x = x[:, None]
    f0, gain_db, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = math.tan(math.pi * f0 / sr)
    vh, vb = 10 ** (gain_db / 20), 10 ** (gain_db / 40)
    a0 = 1 + k / q + k * k
    b_shelf = [(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0]
    a_shelf = [1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    f1, q1 = 38.13547087602444, 0.5003270373238773
    k1 = math.tan(math.pi * f1 / sr)
    a01 = 1 + k1 / q1 + k1 * k1
    b_hp = [1.0, -2.0, 1.0]
    a_hp = [1.0, 2 * (k1 * k1 - 1) / a01, (1 - k1 / q1 + k1 * k1) / a01]
    y = lfilter(b_hp, a_hp, lfilter(b_shelf, a_shelf, x, axis=0), axis=0)
    block, hop = int(0.4 * sr), int(0.1 * sr)
    powers = np.array([float(np.sum(np.mean(y[i:i + block] ** 2, axis=0))) for i in range(0, len(y) - block + 1, hop)])
    if len(powers) == 0:
        return -120.0
    loud = -0.691 + 10 * np.log10(powers + 1e-30)
    if momentary_max:
        return float(loud.max())
    gated = powers[loud > -70]
    if len(gated) == 0:
        return -120.0
    rel = -0.691 + 10 * math.log10(gated.mean()) - 10
    gated2 = powers[(loud > -70) & (loud > rel)]
    return -0.691 + 10 * math.log10(gated2.mean() + 1e-30)


def analyse(path):
    data, sr = sf.read(str(path), always_2d=True)
    finite = np.isfinite(data)
    result = {"file": str(path), "sr": sr, "non_finite": int((~finite).sum())}
    data = np.where(finite, data, 0.0)
    m = data.mean(axis=1)
    result["peak_db"] = round(db(float(np.abs(data).max())), 2)
    result["rms_db"] = round(rms_db(data), 2)
    result["dc"] = round(float(m.mean()), 6)
    result["max_jump"] = round(max_jump(data), 4)
    result["dominant_hz"] = round(dominant_frequency(m, sr), 2) if len(m) >= 64 else 0.0
    result["bands_db"] = band_split(m, sr) if len(m) >= 64 else {}
    result["pass"] = result["non_finite"] == 0
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    results = [analyse(f) for f in args.files]
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for r in results:
            bands = " ".join(f"{k}:{v}" for k, v in r["bands_db"].items())
            print(f"{'PASS' if r['pass'] else 'FAIL'} {r['file']} peak={r['peak_db']} rms={r['rms_db']} dc={r['dc']} "
                  f"jump={r['max_jump']} f0={r['dominant_hz']}\n      bands {bands}")
    sys.exit(0 if all(r["pass"] for r in results) else 1)


if __name__ == "__main__":
    main()
