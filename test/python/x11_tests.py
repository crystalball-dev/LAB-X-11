"""LAB X-11's own tests for test/run_tests.py: T0 (the C++ self-tests of core/dsp/Align.h, Gate.h and Leveler.h, run by
the harness's --selftest) and the render tests T1-T12 (T1-T6 the gate, T7-T12 the level matcher), plus the settings
the common tests need (guard stages, transparent settings for C2, the editor's panel titles for C9, P1's reference).

Every signal is synthesized here or by labtest.fixture(); nothing is read from outside the repository.
"""

import math

import numpy as np
import soundfile as sf
from scipy.signal import lfilter

import labanalyze as an
from labtest import OUT, fixture, mono_of, sets

SR = 48000
LOOKAHEAD_S = 0.005

# The gate's defaults (plugins/x11/LabX11.cpp, docs/SPEC_LOOKAHEAD_GATE_AND_ALIGNMENT.md section 4.4).
GATE_THR, GATE_HYST, GATE_RANGE = -40.0, 4.0, 24.0

# Settings that take the level matcher out, so the gate tests measure the gate alone.
MATCHER_OFF = sets("lvl_amount=0")
# Settings that take the gate out (RANGE 0), so the matcher tests measure the matcher alone.
GATE_OFF = sets("gate_range=0")


def latency_for(sr):
    """The plugin's latency rule (core/dsp/Align.h): floor(sr * 0.005 + 0.5), never Python's round()."""
    return max(1, int(sr * LOOKAHEAD_S + 0.5))


# ---------------------------------------------------------------------------------------------------------------------
# Synthetic vocal: voiced phrases (a harmonic tone through three formant resonators, with vibrato and syllables),
# consonant bursts at syllable starts, breaths between phrases, decaying word tails, and room tone underneath.

def _resonator(x, sr, hz, q):
    w = 2 * math.pi * hz / sr
    alpha = math.sin(w) / (2 * q)
    b = [alpha, 0.0, -alpha]
    a = [1 + alpha, -2 * math.cos(w), 1 - alpha]
    return lfilter(np.array(b) / a[0], np.array(a) / a[0], x)


def _highpass_noise(rng, n, sr, hz):
    w = rng.standard_normal(n)
    k = math.exp(-2 * math.pi * hz / sr)
    return lfilter([1.0, -1.0], [1.0, -k], w)


def _rms(x):
    return math.sqrt(float(np.mean(np.asarray(x) ** 2)) + 1e-30)


def vocal_fixture(name, phrases, seconds, sr=SR, room_db=-62.0, breath_db=-46.0, seed=11, tail_s=0.25):
    """A WAV in test/out/fixtures. phrases: [(start, end, level_db, f0)], level_db is the phrase's voiced RMS.
    Returns (path, info) where info lists each phrase's voiced RMS and the room tone level actually written."""
    rng = np.random.default_rng(seed)
    n = int(seconds * sr)
    out = np.zeros(n)
    t_all = np.arange(n) / sr
    info = {"phrases": []}

    # Room tone: low-passed noise.
    room = lfilter([0.08], [1.0, -0.92], rng.standard_normal(n))
    room *= 10 ** (room_db / 20) / _rms(room)
    out += room

    previous_end = 0.0
    for start, end, level_db, f0 in phrases:
        a, b = int(start * sr), int(end * sr)
        tail_n = min(int(tail_s * sr), n - b)
        t = t_all[a:b + tail_n] - start
        # Harmonic source with 5 Hz vibrato (+-1 %), harmonics to 4 kHz falling 6 dB per octave.
        # f(t) = f0 (1 + 0.01 sin(2 pi 5 t)); the phase is its integral.
        phase = 2 * math.pi * f0 * (t + 0.01 * (1 - np.cos(2 * math.pi * 5.0 * t)) / (2 * math.pi * 5.0))
        src = np.zeros_like(t)
        for k in range(1, int(4000 / f0) + 1):
            src += np.sin(k * phase) / k
        voiced = 0.6 * _resonator(src, sr, 700, 6) + 0.5 * _resonator(src, sr, 1200, 8) + 0.3 * _resonator(src, sr, 2600, 10)
        # Syllables: 160 ms each, 15 ms rise, a 60 ms dip at the end of each; then the word tail, the same voice
        # decaying 40 dB over tail_s from the phrase's last envelope value.
        body = b - a
        env = np.ones(len(t))
        syl = int(0.16 * sr)
        for s0 in range(0, body, syl):
            m = min(syl, body - s0)
            rise = min(int(0.015 * sr), m)
            env[s0:s0 + rise] *= np.linspace(0.05, 1.0, rise)
            dip = min(int(0.06 * sr), m)
            env[s0 + m - dip:s0 + m] *= np.linspace(1.0, 0.35, dip)
        env[body:] = env[body - 1] * np.exp(-np.arange(tail_n) / sr * (40.0 / 20.0 * math.log(10) / tail_s))
        voiced = voiced * env
        voiced *= 10 ** (level_db / 20) / _rms(voiced[:body])
        out[a:b + tail_n] += voiced
        # Consonants: a 25 ms high-passed noise burst at the start of every second syllable, 3 dB under the phrase.
        for k, s0 in enumerate(range(a, b, syl)):
            if k % 2 == 0:
                m = min(int(0.025 * sr), b - s0)
                burst = _highpass_noise(rng, m, sr, 3000.0)
                burst *= 10 ** ((level_db - 3) / 20) / _rms(burst) * np.hanning(m)
                out[s0:s0 + m] += burst
        # A breath in the gap before the phrase (300 ms, ending 100 ms before it), if there is room.
        if start - previous_end >= 0.4 and breath_db is not None:
            b0, b1 = int((start - 0.4) * sr), int((start - 0.1) * sr)
            breath = _highpass_noise(rng, b1 - b0, sr, 1500.0)
            breath *= 10 ** (breath_db / 20) / _rms(breath) * np.hanning(b1 - b0)
            out[b0:b1] += breath
        previous_end = end + tail_s
        info["phrases"].append({"start": start, "end": end, "level_db": level_db})

    path = OUT / "fixtures" / f"{name}.wav"
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), np.stack([out, out], axis=1), sr, subtype="FLOAT")
    info["room_db"] = room_db
    return path, info


# The reference vocal for P1 and the matcher tests: six phrases whose levels spread over 12 dB, as a take with no
# fader rides would (handoff 5.1: the user rides the vocal by hand with automation clips).
REFERENCE_PHRASES = [(0.5, 2.3, -24.0, 150.0), (3.1, 4.6, -18.0, 165.0), (5.3, 7.0, -30.0, 140.0),
                     (7.8, 9.2, -21.0, 175.0), (10.0, 11.9, -27.0, 155.0), (12.6, 14.0, -19.0, 160.0)]


def reference_vocal():
    return vocal_fixture("x11_reference_vocal", REFERENCE_PHRASES, 15.0)


# ---------------------------------------------------------------------------------------------------------------------

def gain_curve(out_m, in_m, sr, frame_s=0.002):
    """Per-frame gain in dB of output over input (RMS of each frame)."""
    eo = an.envelope(out_m, sr, frame_s)
    ei = an.envelope(in_m, sr, frame_s)
    k = min(len(eo), len(ei))
    return 20 * np.log10((eo[:k] + 1e-12) / (ei[:k] + 1e-12))


def gate_tests(h, record, want):
    if want("T0"):
        st = h.run(["--selftest"])
        cases = int(st.get("selftest_cases", "0"))
        failed = sorted(k[len("selftest_"):-3] for k, v in st.items() if k.startswith("selftest_") and k.endswith("_ok") and v != "1")
        record("T0", "C++ self-tests of Align.h, Gate.h, Leveler.h and DelayLine.h all pass", st["exit"] == 0 and cases > 0 and not failed,
               {"cases": cases, "failed": failed, "delay_mono": st.get("selftest_delay_mono")})

    if want("T1"):
        noise = fixture("x11_room_tone", SR, 3.0, [(0.0, 3.0, "noise", 0, -55.0)])
        wav, st = h.render("t1_floor", ["--in", noise, "--level", "0", "--seconds", "3"] + MATCHER_OFF)
        m, _, sr = mono_of(wav)
        level = an.rms_db(an.segment(m, sr, 0.5, 3.0))
        limit = -55.0 - GATE_RANGE + 1.0
        record("T1", "room tone at -55 dBFS RMS under the -40 dB threshold comes out RANGE (24 dB) lower", level <= limit,
               {"output_rms_db": round(level, 2), "limit_db": limit, "faults": st.get("faults")})

    if want("T2"):
        # A 220 Hz tone at -12 dBFS from 0.2 to 0.5 s, then a word tail falling 40 dB in 300 ms.
        n = int(1.5 * SR)
        t = np.arange(n) / SR
        x = 10 ** (-12 / 20) * np.sin(2 * math.pi * 220 * t)
        env = np.zeros(n)
        a, b = int(0.2 * SR), int(0.5 * SR)
        env[a:b] = 1.0
        env[a:a + 96] = np.linspace(0, 1, 96)
        decay = 40.0 / 0.3   # dB per second
        env[b:] = 10 ** (-decay * (t[b:] - 0.5) / 20)
        path = OUT / "fixtures" / "x11_word_tail.wav"
        path.parent.mkdir(parents=True, exist_ok=True)
        sf.write(str(path), np.stack([x * env] * 2, axis=1), SR, subtype="FLOAT")
        wav, _ = h.render("t2_tail", ["--in", path, "--level", "0", "--seconds", "1.5"] + MATCHER_OFF)
        m, _, sr = mono_of(wav)
        # The tail's peak falls below the close threshold (-44 dB) at t_close; until then it must be untouched.
        t_close = 0.5 + (-12.0 - (GATE_THR - GATE_HYST)) / decay
        g = gain_curve(m, x * env, sr, 0.01)
        frames = [k for k in range(len(g)) if 0.25 <= k * 0.01 and (k + 1) * 0.01 <= t_close]
        worst = float(np.max(np.abs(g[frames])))
        record("T2", "a word tail passes untouched (within 1 dB) until its peak falls below THRESHOLD - HYSTERESIS",
               worst <= 1.0, {"worst_db": round(worst, 3), "tail_close_s": round(t_close, 3)})

    if want("T3"):
        # Onsets from a closed gate: impulses and 10 ms noise bursts, 400 ms apart (hold + release < 400 ms).
        parts = [(0.2 + 0.4 * k, 0.2 + 0.4 * k, "impulse", 0, -12.0) for k in range(4)]
        parts += [(1.8 + 0.4 * k, 1.81 + 0.4 * k, "noise", 0, -20.0) for k in range(4)]
        clicks = fixture("x11_onsets", SR, 3.5, parts)
        dry, _ = h.render("t3_dry", ["--in", clicks, "--level", "0", "--seconds", "3.5"] + sets("bypass=1"))
        wet, _ = h.render("t3_gated", ["--in", clicks, "--level", "0", "--seconds", "3.5"] + MATCHER_OFF)
        md, _, sr = mono_of(dry)
        mw, _, _ = mono_of(wet)
        worst = 0.0
        rows = []
        for k in range(4):
            i = int((0.2 + 0.4 * k) * sr)
            d = 20 * math.log10((abs(mw[i]) + 1e-30) / (abs(md[i]) + 1e-30))
            rows.append(round(d, 3))
            worst = max(worst, abs(d))
        for k in range(4):
            s = an.segment(md, sr, 1.8 + 0.4 * k, 1.802 + 0.4 * k)
            w = an.segment(mw, sr, 1.8 + 0.4 * k, 1.802 + 0.4 * k)
            d = an.rms_db(w) - an.rms_db(s)
            rows.append(round(d, 3))
            worst = max(worst, abs(d))
        record("T3", "onsets from a closed gate keep their first sample / first 2 ms within 0.5 dB (the lookahead)",
               worst <= 0.5, {"onset_diff_db": rows, "worst_db": round(worst, 3)})

    if want("T4"):
        # A 500 Hz burst over a -50 dBFS 3 kHz probe; after the burst the probe carries the gate's gain curve.
        n = int(1.6 * SR)
        t = np.arange(n) / SR
        probe = 10 ** (-50 / 20) * np.sin(2 * math.pi * 3000 * t)
        burst = np.where((t >= 0.2) & (t < 0.6), 10 ** (-20 / 20) * np.sin(2 * math.pi * 500 * t), 0.0)
        path = OUT / "fixtures" / "x11_burst_probe.wav"
        sf.write(str(path), np.stack([probe + burst] * 2, axis=1), SR, subtype="FLOAT")
        fall_start, durations = {}, {}
        for hold in (0, 100):
            wav, _ = h.render(f"t4_hold{hold}", ["--in", path, "--level", "0", "--seconds", "1.6"] + MATCHER_OFF
                              + sets(f"gate_hold={hold}", "gate_release=200"))
            m, _, sr = mono_of(wav)
            g = gain_curve(m, probe, sr, 0.001)
            k0 = int(0.62 / 0.001)
            start = next((k for k in range(k0, len(g)) if g[k] < -0.3), None)
            end = next((k for k in range(start, len(g)) if g[k] <= -GATE_RANGE + 0.3), None) if start is not None else None
            fall_start[hold] = start * 0.001 if start is not None else float("nan")
            durations[hold] = (end - start) * 0.001 if end is not None else float("nan")
        hold_ms = (fall_start[100] - fall_start[0]) * 1000
        release_ms = durations[100] * 1000
        ok = abs(hold_ms - 100) <= 2.0 and abs(release_ms - 200) <= 200 * 0.05   # NaN (the gate never closed) fails
        record("T4", "HOLD 100 ms delays the close by 100 ms (+-2); RELEASE 200 ms falls the whole RANGE in 200 ms (+-5 %)", ok,
               {"hold_ms": round(hold_ms, 2), "release_ms": round(release_ms, 2)})

    if want("T5"):
        rows, ok = {}, True
        for sr in (44100, 48000, 96000):
            wav, st = h.render(f"t5_impulse_{sr}", ["--signal", "impulse", "--level", "-6", "--seconds", "0.5", "--sr", str(sr),
                                                    "--no-trim"])
            data, _ = an.load(wav)
            m = an.mono(data)
            at = int(np.argmax(np.abs(m))) - int(0.1 * sr)
            rep = int(st.get("latency", "-1"))
            rows[str(sr)] = {"reported": rep, "measured": at, "rule": latency_for(sr)}
            ok = ok and rep == at == latency_for(sr)
        record("T5", "impulse offset in the render = reported latency = floor(sr x 5 ms + 0.5) at 44.1/48/96 kHz", ok, rows)

    if want("T6"):
        wav, st = h.render("t6_ramp", ["--signal", "sine:220", "--level", "-12", "--seconds", "10",
                                       "--ramp", "gate_thr=-80..0@0-10"] + MATCHER_OFF)
        jump = float(st.get("max_jump", "1"))
        ok = int(st.get("faults", "1")) == 0 and int(st.get("non_finite", "1")) == 0 and jump <= 0.03
        record("T6", "THRESHOLD ramped -80..0 dB over a -12 dBFS sine: no fault, no step above 0.03 (the sine's own is 0.007)", ok,
               {"max_jump": jump, "faults": st.get("faults")})


def switch_test(h, record, want):
    """T13: a preset change on a sustained vocal-level tone is click-free. C4 (drums, an absolute 0.5 jump) cannot see a
    vocal click: the drums' own steps are larger than one."""
    if not want("T13"):
        return
    base = ["--signal", "sine:220", "--level", "-3", "--seconds", "2"]
    rows, ok = {}, True
    for sr in (44100, 48000, 96000):
        steady_wav, steady = h.render(f"t13_steady_{sr}", base + ["--sr", str(sr)])
        wav, st = h.render(f"t13_switch_{sr}", base + ["--sr", str(sr), "--program-at", "0@1.0"])
        m, _, rate = mono_of(wav)
        s, _, _ = mono_of(steady_wav)
        # Window by window against the same render without the switch (a 1 ms window of a 220 Hz tone swings with phase).
        dip = max(an.rms_db(an.segment(s, rate, t, t + 0.001)) - an.rms_db(an.segment(m, rate, t, t + 0.001))
                  for t in np.arange(0.98, 1.06, 0.001))
        jump, steady_jump = float(st.get("max_jump", "1")), float(steady.get("max_jump", "0"))
        rows[str(sr)] = {"max_jump": jump, "steady_jump": steady_jump, "worst_1ms_dip_db": round(dip, 2)}
        ok = ok and jump <= 1.5 * steady_jump and dip <= 1.5 and int(st.get("faults", "1")) == 0
    record("T13", "preset switch on a -3 dBFS 220 Hz tone: no step above 1.5x the tone's own, no 1 ms dip over 1.5 dB", ok, rows)


def x11_tests(h, record, want):
    gate_tests(h, record, want)
    matcher_tests(h, record, want)
    switch_test(h, record, want)


def matcher_tests(h, record, want):
    """T7-T12: filled in with the level matcher (core/dsp/Leveler.h)."""
    return


def spec():
    return {
        "stages": ["gate", "level"],
        "panels": ["GATE // ШЛЮЗ", "DETECTOR // ДЕТЕКТОР", "LEVEL // УРОВЕНЬ", "METERS // ПРИБОРЫ", "OUTPUT // ВЫХОД"],
        # C2: the impulse must come out at the reported latency with the defaults and with both stages out.
        "transparent": [("default", []), ("both_off", GATE_OFF + MATCHER_OFF)],
        # The utility presets keep OUTPUT 0 dB on purpose (handoff 5.4).
        "untrimmed": ["Gate Only", "Level Only"],
        "loudness_signal": ["--in", str(reference_vocal()[0]), "--level", "0", "--seconds", "15"],
        "loudness_name": "reference vocal",
        "tests": x11_tests,
    }
