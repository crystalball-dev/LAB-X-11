"""LAB X-11 test suite: the common tests C1-C9 and P1 of the LAB X-16 suite (ported unchanged), plus LAB X-11's own
T-tests (test/python/x11_tests.py): T0 the C++ self-tests of the DSP headers, T1-T12 render tests of the gate and the
level matcher.

Usage:  python -I test/run_tests.py [--all | --plugin x11] [--only C1,C2,T3,P1] [--skip-pluginval] [--fuzz-seconds 300] [--quick]
        (--all and --plugin x11 are accepted for the family's command line; this repository builds one plugin.)
On Linux run it under xvfb-run (C7 pluginval and C9 open the editor): xvfb-run -a python3 -I test/run_tests.py --all
Renders go to test/out/x11 (git-ignored); the JSON report lands in build/test-report-x11.json.
Every render uses fixed instance ids (the harness default), so seeded signals repeat exactly.
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
OUT = ROOT / "test" / "out"
sys.path.insert(0, str(ROOT / "test" / "python"))
import labanalyze as an  # noqa: E402
from labtest import sets, fixture, mono_of  # noqa: E402,F401
import x11_tests  # noqa: E402

STAT = re.compile(r'(\w+)=("([^"]*)"|\S+)')
PLUGINVAL = (os.environ.get("PLUGINVAL") or shutil.which("pluginval")
             or (r"F:\_DEV\tools\pluginval\pluginval.exe" if os.name == "nt" else str(ROOT.parent / "tools" / "pluginval" / "pluginval")))
EXE = ".exe" if os.name == "nt" else ""


def plugin():
    """Name, product, code and version from CMakeLists.txt (one plugin: project(LABX11 VERSION x.y.z))."""
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    version = re.search(r"project\(LABX11 VERSION ([\d.]+)", text).group(1)
    code = re.search(r"PLUGIN_CODE (\w+)", text).group(1)
    product = re.search(r'PRODUCT_NAME "([^"]+)"', text).group(1)
    return {"name": "LabX11", "product": product, "code": code, "version": version}


class Harness:
    def __init__(self, slug, info):
        self.slug = slug
        self.info = info
        self.exe = BUILD / f"{info['name']}Render_artefacts" / "Release" / f"{info['name']}Render{EXE}"
        self.bundle = BUILD / f"{info['name']}_artefacts" / "Release" / "VST3" / f"{info['product']}.vst3"
        self.out = OUT / slug
        self.out.mkdir(parents=True, exist_ok=True)

    def run(self, args, timeout=3600):
        proc = subprocess.run([str(self.exe)] + [str(a) for a in args], capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=timeout)
        stats = {}
        for line in proc.stdout.splitlines():
            for key, value, quoted in STAT.findall(line):
                stats[key] = quoted if value.startswith('"') else value
        stats["exit"] = proc.returncode
        if proc.returncode != 0:
            stats["stderr"] = (proc.stderr.strip() or proc.stdout.strip())[-400:]
        return stats

    def render(self, name, args, timeout=3600):
        wav = self.out / f"{name}.wav"
        return wav, self.run(["--out", wav] + list(args), timeout)

    def params(self):
        proc = subprocess.run([str(self.exe), "--list-params"], capture_output=True, text=True, encoding="utf-8")
        presets = re.findall(r'^preset (\d+) "([^"]*)"', proc.stdout, re.M)
        return [name for _, name in presets]


# ---------------------------------------------------------------------------------------------------------------------
# The common tests, verbatim from LAB X-16 (only P1's reference signal comes from the spec).

def common_tests(h, spec, presets, record, want, args):
    if want("C1"):
        diffs = {}
        for label, extra in (("default", []), ("last_preset", ["--program", str(len(presets) - 1)])):
            _, st = h.render(f"c1_null_{label}", ["--null-check", "--seconds", "3", "--signal", "drums:120"] + extra)
            diffs[label] = float(st.get("null_max_diff", "1"))
        record("C1", "null at BYPASS On (default and last preset)", all(d <= 1e-6 for d in diffs.values()), diffs)

    if want("C2"):
        bad, reported = [], set()
        for sr in (44100, 48000, 96000):
            for label, extra in spec["transparent"]:
                _, st = h.render("c2_latency", ["--latency-check", "--seconds", "1", "--sr", str(sr)] + extra)
                rep, meas = int(st.get("latency_reported", "-1")), int(st.get("latency_measured", "-2"))
                reported.add((sr, rep))
                if rep != meas:
                    bad.append({"sr": sr, "setting": label, "reported": rep, "measured": meas})
        fz = h.run(["--fuzz", "--seconds", "60", "--seed", "3"])
        changes = int(fz.get("latency_changes", "-1"))
        record("C2", "latency: reported = measured (44.1/48/96 kHz, every mode); constant through a 60 s fuzz",
               not bad and changes == 0, {"mismatches": bad, "reported": sorted(reported), "fuzz_latency_changes": changes})

    if want("C3"):
        st = h.run(["--roundtrip"])
        record("C3", "state round trip", st.get("roundtrip") == "PASS",
               {k: st.get(k) for k in ("param_mismatches", "state_identical", "callsign_ok", "state_bytes")})

    if want("C4"):
        seq = list(range(1, len(presets))) + [0]
        times = [0.6 + 0.6 * i for i in range(len(seq))]
        wav, st = h.render("c4_preset_switch", ["--signal", "drums:120", "--seconds", str(times[-1] + 1.0), "--program-at",
                                                ",".join(f"{p}@{t:.2f}" for p, t in zip(seq, times))])
        data, sr = an.load(wav)
        d = np.abs(np.diff(data, axis=0)).max(axis=1)
        mask = np.zeros(len(d), bool)
        for t in times:
            mask[max(0, int((t - 0.01) * sr)):int((t + 0.06) * sr)] = True
        at_switch, steady = float(d[mask].max()), float(d[~mask].max())
        ok = int(st.get("faults", "1")) == 0 and (at_switch < 0.5 or at_switch <= steady * 1.05)
        record("C4", "preset switch during signal: jump below 0.5 (or no larger than the presets' own), no fault", ok,
               {"jump_at_switch": round(at_switch, 4), "jump_steady": round(steady, 4), "faults": st.get("faults")})

    if want("C5"):
        rows, ok = [], True
        seconds = args.fuzz_seconds
        for sr in (44100, 48000):
            for block in ("1", "7", "64", "3000", "vary"):
                extra = ["--vary-blocks"] if block == "vary" else ["--block", block]
                started = time.time()
                st = h.run(["--fuzz", "--seconds", str(seconds), "--seed", str(7 + len(rows)), "--sr", str(sr)] + extra
                           + spec.get("fuzz", []))
                good = (st["exit"] == 0 and int(st.get("non_finite", "1")) == 0 and int(st.get("faults", "1")) == 0
                        and int(st.get("growth_silence", "1")) == 0)
                ok = ok and good
                rows.append({"sr": sr, "block": block, "ok": good, "faults": st.get("faults"), "non_finite": st.get("non_finite"),
                             "growth": st.get("growth_silence"), "peak_db": st.get("peak_db"), "wall_s": round(time.time() - started, 1)})
        record("C5", f"fuzz {seconds:g} s x (1, 7, 64, 3000, variable blocks) x (44.1, 48 kHz): no fault, non-finite or growth", ok,
               {"runs": rows})

    if want("C6"):
        log = h.out / "c6_faults.log"
        if log.exists():
            log.unlink()
        names = spec["stages"] + ["output"]
        rows, ok = [], True
        for i, name in enumerate(names):
            _, st = h.render(f"c6_inject_{name}", ["--signal", "drums:120", "--seconds", "2", "--inject", f"{i}@1.0", "--fault-log", log])
            good = int(st.get("faults", "0")) >= 1 and int(st.get("non_finite", "1")) == 0 and float(st.get("peak_db", "99")) < 6.0
            ok = ok and good
            rows.append({"stage": name, "faults": st.get("faults"), "peak_db": st.get("peak_db")})
        text = log.read_text(encoding="utf-8") if log.exists() else ""
        logged = sorted(set(re.findall(r"FAULT\s+(\S+)", text)))
        ok = ok and all(n.upper() in logged for n in names)
        record("C6", "guard: an injection at every stage is caught, silenced and logged by name", ok, {"runs": rows, "logged": logged})

    if want("C7") and not args.skip_pluginval:
        rows, ok = {}, True
        for level in (5, 10):
            proc = subprocess.run([PLUGINVAL, "--strictness-level", str(level), "--validate", str(h.bundle)],
                                  capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=3600)
            (h.out / f"pluginval-{level}.log").write_text(proc.stdout + proc.stderr, encoding="utf-8")
            rows[str(level)] = proc.returncode
            ok = ok and proc.returncode == 0
        record("C7", "pluginval strictness 5 and 10", ok, {"exit_codes": rows, "bundle": str(h.bundle.name)})

    if want("C8"):
        rows = {}
        for p, name in enumerate(presets):
            st = h.run(["--program", str(p), "--seconds", "30", "--block", "64", "--sr", "48000", "--signal", "drums:120"])
            rows[name] = round(float(st.get("realtime_x", "0")), 1)
        worst = min(rows.values())
        record("C8", "CPU: at least 20x real time at 48 kHz, 64-sample blocks, worst preset", worst >= 20.0,
               {"worst_x": worst, "per_preset_x": rows})

    if want("C9"):
        png = h.out / "editor.png"
        st = h.run(["--png", png])
        titles = st.get("panels", "")
        missing = [t for t in spec["panels"] if t not in titles]
        ok = st.get("ok") == "1" and st.get("size") == "960x560" and not missing and len(titles) > 0
        record("C9", "editor PNG renders; every panel title in the spec appears", ok,
               {"size": st.get("size"), "panels": titles, "missing": missing})


def loudness_test(h, spec, presets, record, want):
    if not want("P1"):
        return
    signal = spec.get("loudness_signal", ["--signal", "drums:120", "--seconds", "8"])
    _, ref = h.render("p1_input", signal + sets("bypass=1"))
    ref_db = float(ref.get("rms_db", "-100"))
    rows, ok = {}, True
    for p, name in enumerate(presets):
        if p == 0 or name in spec.get("untrimmed", []):
            continue   # Init is the defaults; an untrimmed preset keeps OUTPUT 0 dB on purpose (docs/DESIGN.md)
        _, st = h.render(f"p1_preset{p}", signal + ["--program", str(p)])
        diff = float(st.get("rms_db", "-100")) - ref_db
        rows[name] = round(diff, 2)
        ok = ok and abs(diff) <= 1.0
    record("P1", f"presets loudness-trimmed: RMS within 1 dB of the input ({spec.get('loudness_name', 'drums')}, preset's own mix)", ok,
           {"input_rms_db": round(ref_db, 2), "difference_db": rows})


def main():
    # The panel titles are bilingual; a redirected stdout would otherwise use the ANSI code page and fail on Cyrillic.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin", action="append", default=[], help="accepted for the family command line: x11")
    parser.add_argument("--all", action="store_true", help="accepted for the family command line (one plugin here)")
    parser.add_argument("--only", help="comma-separated test ids, e.g. C1,C3,T2")
    parser.add_argument("--skip-pluginval", action="store_true")
    parser.add_argument("--fuzz-seconds", type=float, default=300.0)
    parser.add_argument("--quick", action="store_true", help="30 s fuzz runs")
    args = parser.parse_args()
    if any(p != "x11" for p in args.plugin):
        parser.error("this repository builds one plugin: x11")
    if args.quick:
        args.fuzz_seconds = min(args.fuzz_seconds, 30.0)
    only = {x.strip().upper() for x in args.only.split(",")} if args.only else None

    def want(test_id):
        return only is None or test_id in only

    info = plugin()
    slug = "x11"
    spec = x11_tests.spec()
    h = Harness(slug, info)
    if not h.exe.exists():
        print(f"harness not built: {h.exe}")
        return 2
    presets = h.params()
    report = {"plugin": slug, "version": info["version"], "tests": []}
    print(f"\n== {info['product']} ({info['version']})")

    def record(test_id, name, ok, detail):
        report["tests"].append({"id": test_id, "name": name, "pass": bool(ok), **detail})
        print(f"{'PASS' if ok else 'FAIL'}  {test_id:3s} {name}  {json.dumps(detail, ensure_ascii=False)[:420]}", flush=True)

    # A test group that raises (a regression the analysis did not expect) is recorded as a failure, and the rest of the
    # suite still runs and the report is still written.
    for label, group in (("common", lambda: common_tests(h, spec, presets, record, want, args)),
                         ("x11", lambda: spec["tests"](h, record, want)),
                         ("loudness", lambda: loudness_test(h, spec, presets, record, want))):
        try:
            group()
        except Exception as e:  # noqa: BLE001
            record("ERR", f"{label} tests raised", False, {"error": repr(e)[:400]})
    passed = sum(t["pass"] for t in report["tests"])
    print(f"{passed}/{len(report['tests'])} passed")
    BUILD.mkdir(exist_ok=True)
    (BUILD / f"test-report-{slug}.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    return 0 if passed == len(report["tests"]) else 1


if __name__ == "__main__":
    sys.exit(main())
