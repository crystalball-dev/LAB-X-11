# LAB X-11 — Design notes and changes from the plan

The plan is `LAB_X11_HANDOFF.md`; the two DSP designs are `docs/SPEC_LOOKAHEAD_GATE_AND_ALIGNMENT.md` (the gate and
the 5 ms alignment) and `docs/SPEC_LEVEL_MATCHER.md` (the level matcher). Each spec ends with an implementation record
that says where the build differs from its text. This file records what the build changed against the plan, the
decisions as built, and the test results, as the sibling LAB X- repositories do.

## Build environment of this round

The plan's machine (Windows, VS 2022, FL Studio 20.6.2) was not reachable from the session that built 0.1.0, so it was
built and tested on Linux: Ubuntu 24.04, GCC 13.3, CMake 3.28 with Ninja, JUCE 9.0.3 (the version the handoff names),
pluginval 1.0.4, Python 3.13 with numpy 2.5 and scipy 1.18. The Windows scripts (`scripts/*.ps1`) follow the siblings'
shape; they were untested in that round, and `build.ps1` has since built the plugin on Windows ("Windows verification"
below). The FL Studio checks (handoff 4.3) are pending.

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
under the noise. +3.5 dB admits 66 %; white and vocal-tract noise are never admitted and pink noise on about 0.02 % of
hops over 16 seeds, never enough to form a level. The consequence, accepted and documented: breathy lines are ridden
about 1.4 to 1.9 times more slowly and settle 1 to 1.5 dB under a clean line of the same loudness. Breaths never move
the gain.

### Review of M2

A second review (real-time and host lens, DSP lens, test lens; two refuters per finding, each finding reproduced)
upheld nine findings and refuted six.

- **Room rumble reads as voice (documented, not fixed).** The voicing test compares two bands, so it reads spectral
  tilt only. Room tone falling 6 dB per octave or more (brown noise, HVAC curves, traffic) is admitted like a voice.
  With the gate open on such room tone (THRESHOLD set below it), the matcher holds it 30 dB under the voice but rides
  it toward MAX BOOST 20 dB under, as it does hum. The specs and the README now say so, and L3 and L23d pin both sides.
  The reviewer's fix, a periodicity test (the low band's normalised autocorrelation over 2.5 to 12.5 ms lags), measured
  brown noise 2.5 %, voice 100 % and breathy voice at HNR 6 dB 99 % in its prototype, which would also lift the breathy
  admission above. It is not in 0.1.0: it changes which hops of real vocals are measured, and only real vocals can
  validate that. **It is yours to decide** after the listening round.
- **Eight test gaps closed.** Eleven mutants of `Leveler.h` passed the whole suite: Step's limit re-clamp and flush
  re-quantise, its commit hysteresis, the Hold / Release / Attack contract, Step re-triggered from Release, the burst
  clamp, a detector reading one channel only, and the per-hop voicing test at render level. New and changed cases
  catch all eleven (L1b, L3, L8b, L12, L12b, L20b, L27b, L28b, T12(d')).
- **Test robustness.** T9 and T12 record a failure instead of stopping the suite when a render goes wrong; the runner
  records any test group that raises and always writes its report; T0 pins its case counts (179, of which 146 are the
  matcher's).

## M3: presets, editor, tests

- **Presets** follow handoff 5.4. Every preset writes TARGET -30 (since the review below; -18 PROVISIONAL in the first
  build) and LAW Ride. With the reference vocal at TARGET, every
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
| D6 | Default threshold | -40 dB, the DD Gate reading; validated on eight reference takes (no audible vocal or word tail lost, room tone down 14 to 23 dB). A take recorded about 40 dB low needs it lowered (README) |
| D7 | Matcher law | Ride (continuous) by default and in every preset; Step (1.5 dB grid, set at phrase onsets) as a choice. **Please confirm** |
| D8 | Matcher target | a fixed TARGET, -30: the median voiced loudness of the reference lead takes (was -18 provisional). Its meaning is amended from "dBFS short-term" to "LUFS of the voiced vocal, momentary": set it from the LEVEL readout. **Please confirm** |
| D9 | Names | panel titles and preset names as proposed; **yours to change** (nothing depends on them) |
| D10 | Unit tests | `selfTest()` in the DSP headers, printed by `--selftest` and asserted as T0, as proposed |
| D11 | Theme | Oxide (Deep Zone), 960 x 560 |

Open from the matcher design for your decision: the periodicity voicing test (above), which would reject room rumble
and ride breathy delivery as eagerly as clean singing; and whether a later version should link doubles' TARGET to the
lead (it is copied by hand in 0.1.0).

Open from the review on Windows (below), also yours:

- **Breaths.** At the defaults loud breaths pass at full level and quieter breath noise is lowered with the room tone.
  Reducing loud breaths too (the brief asks for a gate "tuned for vocal breaths and room tone between phrases") needs a
  higher THRESHOLD, with the risk to soft word starts, or a breath-aware mode keyed on the matcher's voicing test.
  Neither is in 0.1.0.
- **Pre-level floor at TARGET -30.** With a misset gate, hum before the first line now forms the first level at Init:
  +4.2 dB on the hum and on the first seconds of voice (spec M12). The -10 dB floor of spec O-17 would close this, at
  the cost O-17 lists.
- **A lookahead mode for the matcher.** The causal rider narrows the line-to-line spread of the reference takes by 11
  to 30 % on most of them. Knowing each line's level before it plays (a 1 to 2 s lookahead, compensated by FL at the
  price of latency) is an option under study.

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
| C8 CPU | 183 to 274 times real time at 48 kHz, 64-sample blocks |
| C9 editor | 960 x 560, every panel title present |
| T0 self-tests | 179 of 179 (33 gate and alignment, 145 matcher, the defaults check) |
| T1-T6 gate | room tone 24 dB down; tails untouched; onsets 0.0 dB; hold 100.0 ms, release 195 ms; latency exact; no steps under a THRESHOLD sweep |
| T7-T12 matcher | +12.00 dB from -30 dBFS in 2.89 s at every rate; limits exact; gain frozen through gaps (0.0 dB pp); bursts move it under 0.012 dB; takes 12 dB apart land 1.2 dB apart; breaths and hold tails ignored (an abrupt breath right after a word moves it at most 0.004 dB); Gate Only bit-exact; no added latency |
| T13 preset switch on a vocal-level tone | no step above the tone's own, no dip |
| P1 loudness | every preset within 0.05 dB |

## Windows verification and the reference takes, 2026-10-19

A review on the plan's own machine (Windows 11, VS 2022 Build Tools with MSVC 14.44, CMake 4.4.4, JUCE 9.0.3,
pluginval, Python 3.14 with numpy 2.5 and scipy 1.18) built this branch with `scripts/build.ps1` and ran the suite.

| Run | Build | `--selftest` | `run_tests.py --all` |
|---|---|---|---|
| The Linux-verified commit (93c40ab) | clean, 0 warnings, 144 s | 179 of 179 | 24 of 24, pluginval 5 and 10 included |
| After the TARGET change below | clean, 0 warnings | 179 of 179 | 24 of 24 |

C8 reads 480 times real time for every preset on Windows. Thread CPU time there has a 15.6 ms resolution, so that
figure is the clock's floor, not a measurement; the margin over the 20 times requirement is large either way.

**Found, not fixed: the readout headers are cut off on Windows.** The editor draws in Consolas there, wider than the
font the Linux render fell back to, so the KEY and LEVEL readouts show "KEY THR -40…" and "LUFS TGT -3…" instead of
the full threshold and target (visible in `docs/labx11-ui.png`, now the Windows render). The knobs below still show
both values. Shorter header text or a smaller header font would fix it; C9 does not check it.

The reference takes, out of reach of the first build's machine, were measured: the first five minutes of nine raw
takes from the three projects, rendered through `LabX11Render` (handoff 5.3 has the method and the per-take figures).
What changed the build or the docs:

- **TARGET -30.** The takes' voiced loudness has a median of -30 LUFS, so the provisional -18 kept the matcher at MAX
  BOOST: a flat +4.5 dB, no line evening. TARGET's default (the parameter table and `LevelMatcher::defaults`) is now
  -30, in every preset. The self-tests and the render tests' PIN keep -18, so their figures stand; T11's Rap Lead
  render and T12(g) place their signals relative to the preset TARGET (spec M12).
- **THRESHOLD -40 validated.** On eight normally recorded takes the gate at its defaults lost 0.03 dB or less of vocal
  and word-tail energy and took the room tone down 14 to 23 dB. A ninth take, recorded about 40 dB low, loses most of
  its words at -40 dB; the README now says the defaults assume a normal recording level.
- **Breaths.** Loud breaths pass and most quieter breath noise is lowered. The README said breaths pass; it now says
  what the takes showed.
- **The matcher on real delivery.** With TARGET at each take's level the line-to-line spread shrinks 11 to 30 % on most
  takes (from 1 to 49 %), the causal limit of spec O-9. The README now says so.
- **README.** The real-vocal render command now passes `--level 0`, `--seconds` and `--sr` (the harness defaults are
  -12 dB and 4 s). Added: the ride runs in voiced time; a take about 25 dB or more quieter than the learned level is not
  measured; Step also commits inside a line after 4 s of voice; FL labels TARGET in dB.

## FL Studio checks (handoff 4.3): pending

| # | Check | Status |
|---|---|---|
| F-A | loads in FL 20.6.2 as one file, "LAB X-11", Fx / Dynamics | pending (Windows build) |
| F-B | latency compensated (impulse on a parallel track) | pending |
| F-C | an FL render of a gated vocal equals the harness render of the same WAV and settings | pending |
| F-D | project save and reopen restores every parameter and the preset | pending |
| F-E | bypass and preset switch while playing: no click on the vocal | pending (T13 covers it offline) |
