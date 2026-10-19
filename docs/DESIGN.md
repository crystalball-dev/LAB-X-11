# LAB X-11 — Design notes and changes from the plan

The plan is `LAB_X11_HANDOFF.md`; the two DSP designs are `docs/SPEC_LOOKAHEAD_GATE_AND_ALIGNMENT.md` (the gate and
the 5 ms alignment) and `docs/SPEC_LEVEL_MATCHER.md` (the level matcher). Each spec ends with an implementation record
that says where the build differs from its text. This file records what the build changed against the plan, the
decisions as built, and the test results, as the sibling LAB X- repositories do.

## Build environment of this round

The plan's machine (Windows, VS 2022, FL Studio 20.6.2) was not reachable from the session that built 0.1.0, so it was
built and tested on Linux: Ubuntu 24.04, GCC 13.3, CMake 3.28 with Ninja, JUCE 9.0.3 (the version the handoff names),
pluginval 1.0.4, Python 3.13 with numpy 2.5 and scipy 1.18. The Windows scripts (`scripts/*.ps1`) follow the siblings'
shape and are untested in this round. The FL Studio checks (handoff 4.3) are pending: they need the Windows machine.

## M0: the scaffold on the LAB X-16 base

Decision D1 as planned: the X-16 effect base is ported into this repository (the tree of handoff 2.1), with one plugin
written out in `CMakeLists.txt` instead of X-16's `lab_add_effect` function. Ported files and what changed in them:

| File | Change |
|---|---|
| `core/host/EffectProcessor.h` | `LABX16_HARNESS` -> `LABX11_HARNESS`; the APVTS tag `"LABX11"` and blob tag `"LX11"`; `getName()` returns the product (`"LAB X-11"`) instead of `"LAB X-16 " + product`, so the fault log is `LAB X-11 faults.log` |
| `ui/EffectEditor.h` | the header reads OPERATION FAIRWAY // THE DEEP ZONE above the product (X-16 printed LAB X-16); the footer links lab-x-11; `Lb11` added to the Zone Bus product list |
| `test/harness_main.cpp` | `--selftest` also runs the X-11 DSP self-tests (under `ScopedNoDenormals`) and the defaults check L31; the fixed instance id base is `0x11000001` |
| `test/run_tests.py` | one plugin: reads name, code and version from `CMakeLists.txt`; `--all` and `--plugin x11` are accepted for the family command line; P1's reference signal comes from the plugin's spec (a vocal, not drums). The common tests C1-C9 are otherwise verbatim |
| `core/host/Params.h`, `ZoneBus.h`, `ZoneBusPlatform.cpp`, `core/dsp/{Common, DelayLine, Filters, Follower, Transport}.h`, `ui/{LookAndFeel, Theme, Widgets, Views}.h`, `test/python/{labanalyze, labtest}.py` | none |

`CMakeLists.txt` generates `LabX11Config.h` and force-includes it with `/FI` on MSVC and `-include` elsewhere.

**Known harness quirk, kept.** The ported harness's `--in` reader stops one sample before the end of a file (its
interpolation guard needs a next sample), in LAB X-16 too. The X-11 tests pad their fixtures by one sample instead of
changing the ported code.

## M1: the gate and the alignment

Built as `core/dsp/Gate.h` (`VocalGate`) and `core/dsp/Align.h` (`TransientAlign`). A review of the first build (three
lenses, two refuters per finding, each finding reproduced) found eight defects, all fixed; the gate spec's section 12
lists them. The two that change behaviour a user hears:

- **Preset click.** The base fades a preset change back in over 5 ms, exactly the lookahead, and the first build
  emptied the alignment ring on every preset change: the vocal dropped out for 5 ms and stepped back in. The chain now
  keeps the ring and the gate's state across a preset change. Test T13.
- **Hold counted in output time.** The gate decides 5 ms ahead of the audio, so the first build's hold ended 5 ms early
  and a short HOLD clipped word tails. HOLD now runs from the moment the audio itself falls below the close level.

Also from that review: ramps exact to the sample (double precision), RANGE 0 working at any host block size, no floor
glide after `prepareToPlay`, LOW CUT re-enabled without stale filter state, HOLD changes taking effect within a hold.
A mutation run kills all 16 mutants of the state machine and the detector.

## M2: the level matcher

`core/dsp/Leveler.h` (`LevelMatcher`), designed by a panel (three independent designs, three judges, a synthesis, two
refuters) because the handoff left D7 and D8 open. The continuous "rider" design won on all three judges' scores; the
others' safest ideas were grafted in. Its 125 self-tests reproduce the design prototype's figures to the printed digit.

One deliberate change from the spec, recorded in its implementation record (M1-M9): **the 20 ms voicing test is
+3.5 dB, not +6 dB.** At +6 dB breathy singing (harmonics 6 dB over noise) was admitted on 43 % of its 5 ms hops. No
threshold reaches the spec's 80 % without admitting breath-like noise, because in each syllable dip the voice falls
under the noise. +3.5 dB admits 66 % and still admits no white, pink or vocal-tract noise. The consequence, accepted
and documented: breathy lines are ridden about 1.4 to 1.9 times more slowly and settle 1 to 1.5 dB under a clean line
of the same loudness. Breaths never move the gain.

## M3: presets, editor, tests

- **Presets** follow handoff 5.4. Every preset writes TARGET -18 and LAW Ride. With the reference vocal at TARGET, every
  preset keeps its loudness within 0.05 dB at OUTPUT 0 dB, so no preset carries an OUTPUT trim (P1).
- **Editor**: the X-16 engine, 960 x 560, Oxide. Row 1: GATE and METERS (gate lamp, KEY / THRESHOLD, gate needle, voice
  lamp, LEVEL / TARGET, match needle). Row 2: DETECTOR, LEVEL, OUTPUT. `docs/labx11-ui.png` is the harness's render.
- **Meter slots** (`Meters::custom`): 0 gate lamp, 1 gate reduction, 2 match gain, 3 LEVEL (LUFS), 4 voice lamp, 5 key,
  6 threshold.

## Decisions as built (handoff section 7)

| # | Decision | As built |
|---|---|---|
| D1 | Code base | X-16 effect base ported into this repository, as proposed |
| D2 | Zone Bus | carried unchanged, `busClient = false`, no PEERS panel |
| D3 | Identity | `LAB X-11`, `Lb11`, `com.operationfairway.labx11`, Fx Dynamics, 0.1.0, as proposed |
| D4 | Lookahead | 5 ms fixed: 221 / 240 / 480 samples at 44.1 / 48 / 96 kHz |
| D5 | Sidechain | none |
| D6 | Default threshold | -40 dB, the DD Gate reading; **provisional** until the stem measurements |
| D7 | Matcher law | Ride (continuous) by default and in every preset; Step (1.5 dB grid, set at phrase onsets) as a choice. **Please confirm** |
| D8 | Matcher target | a fixed TARGET, -18 **provisional**. Its meaning is amended from "dBFS short-term" to "LUFS of the voiced vocal, momentary": set it from the LEVEL readout. **Please confirm** |
| D9 | Names | panel titles and preset names as proposed; **yours to change** (nothing depends on them) |
| D10 | Unit tests | `selfTest()` in the DSP headers, printed by `--selftest` and asserted as T0, as proposed |
| D11 | Theme | Oxide (Deep Zone), 960 x 560 |

Open from the matcher design for your decision: whether breathy delivery should be ridden more eagerly at the cost of
occasionally measuring breath noise (the voicing threshold, above), and whether a later version should link doubles'
TARGET to the lead (it is copied by hand in 0.1.0).

## Verification, Linux build, 2026-10-19

`xvfb-run -a python3 -I test/run_tests.py --all`, 300 s fuzz runs: **24 of 24 pass.**

| Test | Result |
|---|---|
| C1 null at bypass | 0.0 (default and last preset) |
| C2 latency | reported = measured at 44.1 / 48 / 96 kHz (221 / 240 / 480); constant through a 60 s fuzz |
| C3 state round trip | identical |
| C4 preset switch | jump 0.135 at the switch, against 0.399 in the drums themselves; no fault |
| C5 fuzz | 300 s each at blocks 1, 7, 64, 3000 and variable, 44.1 and 48 kHz: no fault, no non-finite sample, no growth |
| C6 guard | injections at GATE, LEVEL and OUTPUT caught, silenced and logged by name |
| C7 pluginval | strictness 5 and 10 pass |
| C8 CPU | 172 to 281 times real time at 48 kHz, 64-sample blocks |
| C9 editor | 960 x 560, every panel title present |
| T0 self-tests | 159 of 159 |
| T1-T6 gate | room tone 24 dB down; tails untouched; onsets 0.0 dB; hold 100.0 ms, release 195 ms; latency exact; no steps under a THRESHOLD sweep |
| T7-T12 matcher | +12.00 dB from -30 dBFS in 2.89 s at every rate; limits exact; gain frozen through gaps (0.0 dB pp); bursts move it under 0.012 dB; takes 12 dB apart land 1.2 dB apart; breaths and hold tails ignored; Gate Only bit-exact; no added latency |
| T13 preset switch on a vocal-level tone | no step above the tone's own, no dip |
| P1 loudness | every preset within 0.05 dB |

## FL Studio checks (handoff 4.3): pending

| # | Check | Status |
|---|---|---|
| F-A | loads in FL 20.6.2 as one file, "LAB X-11", Fx / Dynamics | pending (Windows build) |
| F-B | latency compensated (impulse on a parallel track) | pending |
| F-C | an FL render of a gated vocal equals the harness render of the same WAV and settings | pending |
| F-D | project save and reopen restores every parameter and the preset | pending |
| F-E | bypass and preset switch while playing: no click on the vocal | pending (T13 covers it offline) |
