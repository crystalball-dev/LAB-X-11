# LAB X-11 — Plan and execution handoff (DRAFT, awaiting approval)

Status: **plan only.** No source, build file or preset has been written in `F:\_DEV\FL PLUGINS\LAB X-11`; the folder
holds this document alone. Nothing below is implemented until the user approves the plan (section 7 lists the decisions
that want a yes or a change first). Written 2026-10-18 from a read-only inspection of LAB X-3, X-10, X-16, X-19 (and
X-0), the three reference FL projects and their stems.

Naming follows the family: every sibling keeps its plan as `LAB_X<n>_HANDOFF.md` (git-ignored, machine paths allowed),
so this file is LAB X-11's.

---

## 0. The product in one paragraph

LAB X-11 is one VST3 effect for a vocal mixer track in FL Studio: a **noise gate** (THRESHOLD, ATTACK, HOLD, RELEASE,
RANGE, with a hysteresis and a sidechain low cut on the detector, tuned so breaths and room tone between phrases go and
word tails stay) followed by a **level matcher** (a slow, dynamics-based gain that holds the vocal's phrase level
even, so it sits at the same place against the snare and other transients from line to line; "quantizer-style" means
level consistency, not pitch). Everything runs on the one vocal track; no send, bus or sidechain is required (section
2.4). It is built on the LAB X-16 effect base (`lab::host::EffectProcessor` + `lab::host::Chain`), ported into its own
repository the way X-16 ported X-19, under the STALKER lab-instrument look of the family (the X-16 "Oxide" theme).

---

## 1. Findings: how the LAB X family is built, and how LAB X-11 conforms

Projects by date: X-3 (Oct 6, 0.3.1) -> X-10 (Oct 7-10, 0.1.2) -> X-19 (Oct 7-9, 0.1.2) -> X-0 (Oct 9-12, 0.2.4) ->
X-16 (Oct 12-15, 23 effects, 0.1.x). X-3/X-10/X-19/X-0 are instruments in the single-plugin layout; X-16 is the effects
suite and the only sibling with an **effect** base, a sidechain bus, a per-stage guard and the newest conventions
("Ported from LAB X-19 (newest lineage)" in `core/dsp/Common.h`). Where the two layouts differ, LAB X-11 follows X-16,
because its code is X-16's.

### 1.1 Build system

| | X-3 / X-10 / X-19 / X-0 | X-16 |
|---|---|---|
| CMake | `cmake_minimum_required(VERSION 3.22)`, `project(LABX<n> VERSION x.y.z LANGUAGES C CXX)` | same, `project(LABX16 VERSION 0.1.0 ...)` |
| C++ | `CMAKE_CXX_STANDARD 20`, required | same |
| Runtime | `CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"` (static CRT, no redistributable) | same |
| JUCE | `set(LABX<n>_JUCE_PATH "F:/_DEV/tools/JUCE" CACHE PATH ...)`, `add_subdirectory(... JUCE)`, fatal error if missing; JUCE **9.0.3** (verified in `F:/_DEV/tools/JUCE/CMakeLists.txt`) | same, `LABX16_JUCE_PATH` |
| Plugin | one `juce_add_plugin(LabX<n> PRODUCT_NAME "LAB X-<n>" VERSION ... COMPANY_NAME "OPERATION FAIRWAY, LLC" COMPANY_WEBSITE "https://github.com/crystalball-dev/lab-x-<n>" BUNDLE_ID com.operationfairway.labx<n> PLUGIN_MANUFACTURER_CODE Opfw PLUGIN_CODE Lb19 / Lb10 / Lbx3 / Lbx0 FORMATS VST3 Standalone IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE ... EDITOR_WANTS_KEYBOARD_FOCUS FALSE COPY_PLUGIN_AFTER_BUILD FALSE VST3_CATEGORIES Instrument Synth)` | a `lab_add_effect()` function makes `Lx16<Name>` per effect: `PRODUCT_NAME "LAB X-16 <PRODUCT>"`, `BUNDLE_ID com.operationfairway.labx16.<slug>`, `PLUGIN_CODE L6xx`, `IS_SYNTH FALSE`, `VST3_CATEGORIES Fx <Category>`, plus a generated `LabX16Config.h` (`LABX16_PRODUCT/SLUG/VERSION/CODE`) force-included with `/FILabX16Config.h` |
| Sources | `target_sources(LabX<n> PRIVATE Source/*.cpp)`, `target_include_directories(... Source)` | header-only INTERFACE libraries `lab_core` and `lab_ui` rooted at the repo, `plugins/<slug>/<Name>.cpp` + `core/host/ZoneBusPlatform.cpp` |
| Definitions | `JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0`; `/utf-8` on MSVC | same |
| Links | `juce::juce_audio_utils juce::juce_dsp` private; `juce_recommended_config_flags`, `_lto_flags`, `_warning_flags` public | same |
| Harness | `juce_add_console_app(LabX<n>Render)`: same sources + `Tests/RenderHarness.cpp`, `LABX<n>_HARNESS=1` | `Lx16<Name>Render`: `test/harness_main.cpp`, `LABX16_HARNESS=1 JUCE_MODAL_LOOPS_PERMITTED=1` (for `--png-wait`) |

**LAB X-11 will:** `project(LABX11 VERSION 0.1.0)`, one `juce_add_plugin(LabX11 PRODUCT_NAME "LAB X-11" ... BUNDLE_ID
com.operationfairway.labx11 PLUGIN_CODE Lb11 IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE VST3_CATEGORIES Fx Dynamics)` written
out like X-19 (no `lab_add_effect` function: there is one plugin), `LABX11_JUCE_PATH`, the same definitions, flags,
links and static CRT, a generated `LabX11Config.h` as X-16 does (so the base's `LABX16_*` macros become `LABX11_*`),
and `LabX11Render` from the ported `test/harness_main.cpp`. Targets: `LabX11_VST3`, `LabX11_Standalone`,
`LabX11Render`.

### 1.2 Scripts (`scripts/build.ps1`, `install.ps1`, `validate.ps1`, identical shape in every sibling)

- `build.ps1 [-Config Release] [-JucePath F:/_DEV/tools/JUCE]`: cmake from PATH or
  `F:\_DEV\tools\cmake\cmake-4.4.4-windows-x86_64\bin\cmake.exe` (present), `-G "Visual Studio 17 2022" -A x64`,
  `-DLABX<n>_JUCE_PATH=...`, then `--build build --config Release --target LabX<n>_VST3 LabX<n>_Standalone LabX<n>Render --parallel`.
- `install.ps1`: copies the **inner single file**
  `build\LabX<n>_artefacts\Release\VST3\LAB X-<n>.vst3\Contents\x86_64-win\LAB X-<n>.vst3` to
  `C:\Program Files\Common Files\VST3\LAB X-<n>.vst3`; refuses if a bundle *folder* of that name exists ("FL does not
  list it twice") and if the file is locked (FL has it loaded). The VST3 folder today holds `LAB X-0/3/10/19.vst3` and
  the 23 + 2 `LAB X-16 ....vst3` files, all single files.
- `validate.ps1 [-Strictness 5]`: pluginval from PATH or `F:\_DEV\tools\pluginval\pluginval.exe` (present) against the
  bundle, log to `build\pluginval-level<N>.log`.

**LAB X-11 will:** copy the three scripts with `LabX11` / `LAB X-11` substituted; nothing else changes.

### 1.3 Directory layout and file naming

Single-plugin siblings: `Source/{Config.h, Params.h, PluginProcessor.h/.cpp, PluginEditor.h/.cpp, FactoryPresets.h/.cpp,
DSP/*.h (header-only), UI/{CrtLookAndFeel.h, Widgets.h}}`, `Tests/{RenderHarness.cpp, run_tests.py, analyze.py, out/}`,
`scripts/`, `docs/{DESIGN.md, labx<n>-ui.png}`, `README.md`, `LICENSE` (AGPL-3.0), `LAB_X<n>_HANDOFF.md`,
`lab_x<n>_lore_summary.md`, `.gitignore`, `.gitattributes`.

X-16: `core/host/{EffectProcessor.h, Params.h, ZoneBus.h, ZoneBusPlatform.cpp}`, `core/dsp/*.h` (header-only, namespace
`lab::dsp`), `ui/{EffectEditor.h, LookAndFeel.h, Theme.h, Widgets.h, Views.h}` (namespace `lab::ui`),
`plugins/<slug>/<Name>.cpp` (one file per effect: `EffectInfo`, the `Chain`, the `Processor`, the `Editor`,
`createPluginFilter`), `test/{harness_main.cpp, bus_host.cpp, run_tests.py, audibility.py, trim_presets.py,
python/{effect_tests.py, labanalyze.py, labtest.py}, out/}`, `docs/{DESIGN.md, EFFECTS.md, ui/*.png}`.

Common to all: `#pragma once`; PascalCase file names; header-only DSP; 4-space indent; Allman braces; JUCE call style
with a space before the parenthesis (`onePoleCoef (0.005f, sr)`); camelCase members without prefixes; `noexcept` on
audio-path functions; `(size_t)` / `(float)` C-style casts; `std::` numerics over JUCE's where possible; comments in
plain sentences that name the lore and the sibling a piece came from; `juce::ignoreUnused`; `jassert` only off the
audio path; `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR` on processors; LF line endings for sources and CRLF for
`.ps1` (`.gitattributes`); `.gitignore` excludes `build/`, `out/`, `test(s)/out/`, `__pycache__`, IDE files, every
audio extension, `*.flp` and the HANDOFF.

**LAB X-11 will** use the X-16 tree, single plugin (the full tree is in section 2.1).

### 1.4 Parameters

- X-16 `core/host/Params.h` (lineage: X-19 `Source/Params.h`): `struct ParamSpec { const char* id; const char* name;
  Kind kind /*real|integer|choice*/; float minValue, maxValue, defaultValue, skewCentre /*0 = linear*/; Smooth smooth
  /*none|linear|log*/; const char* unit; const char* choices /*"A|B|C"*/; }`; `createLayout()` builds the APVTS
  (`juce::ParameterID { id, 1 }`, `AudioParameterFloat` with `setSkewForCentre`, `AudioParameterChoice`,
  `AudioParameterInt`); `formatValue` / `parseValue` give the house units (`"%"` -> `round(v*100)%`, `"s"` prints ms
  under 1 s, `"ms"` one decimal under 10, `"dB"` one decimal, `"Hz"` with kHz).
- Ids are `snake_case` (`"click_level"`, `"crush_mix"`, `"quiet_thr"`, `"low_split"`), names Title Case, **append-only
  once installed** (FL saves indices), **floats and choices only, never bool** (pluginval's state restore failed on a
  bool in LAB X-3).
- Common parameters first, always: `mix` (0..1, default 1, linear, "%"), `output` (-24..+12 dB, 0, linear), `bypass`
  (choice Off|On, none); the effect's own follow in table order.
- Audio-thread access: the base caches `apvts.getRawParameterValue()` atomics, advances one-pole **15 ms** smoothers
  once per **16-sample control block** (`controlBlock = 16`; linear or log-domain by `Smooth`), and hands the chain a
  `const float* v` of this block's values in `control()`. Convention: frequencies and times `Smooth::log`, levels
  `Smooth::linear`, choices `Smooth::none`. Gains the chain applies itself are ramped per sample inside the chain (as
  the base ramps mix and output over each block).

**LAB X-11 will** declare its parameters as `ParamSpec` rows in `EffectInfo::params` (table in section 3) and read them
only through `control()`.

### 1.5 Real-time safety and faults (X-16 base, inherited verbatim)

- `prepareToPlay` sizes everything (`dryLine.prepare`, chain `prepare`); `processBlock` allocates nothing, takes no
  lock, uses `juce::ScopedNoDenormals`, zeroes non-finite host samples, and ramps mix / output / bypass per sample.
- **Guard:** the chain calls `guard (stage, l, r, n)` after each named stage; a block peak above **+42 dBFS** or
  non-finite is a fault (block silenced, `chain->reset()`, logged with the stage name); above **+18 dBFS** is "hot"
  (logged at most every 2 s per stage). The wet path then passes `dsp::wetSafety` (transparent to +12 dBFS, soft
  ceiling +18 dBFS); the dry and bypassed signal is never touched.
- **Fault log:** a 64-entry ring written on the message thread (500 ms timer) to
  `%APPDATA%\OPERATION FAIRWAY, LLC\<plugin name> faults.log` with sample rate, block, latency, preset and a parameter
  dump on a fault; the editor's SYSTEM readout shows FAULTS.
- **Latency:** `Chain::latency (sampleRate)` is fixed per sample rate, read at `prepareToPlay` only, reported with
  `setLatencySamples`, and the dry path (mix, bypass) is delayed to match. FL compensates it (X-16 check F2 passed to
  the sample).
- **Bypass:** 20 ms crossfade to the aligned dry signal; **presets:** every parameter written once, 30 ms fade to dry,
  `flush()`, 5 ms fade in.

**LAB X-11 will** name two guard stages, `gate` and `level`, keep its lookahead fixed per sample rate, and put no
allocation, lock, `rand()` or I/O on the audio thread (section 3.6 checklist).

### 1.6 Presets and state

- `struct Preset { const char* name; std::vector<std::pair<const char*, float>> values; }`; preset 0 is `"Init"`;
  ids not listed take their defaults; `bypass` is never written by a preset. Six to eight per effect, lore-named,
  **loudness-trimmed** (`output` set so RMS on the reference signal stays within 1 dB of the input; test P1).
- State: APVTS XML (`"LABX16"` tag in X-16; LAB X-11 uses `"LABX11"`) with `version`, `program`, `instanceId`,
  `callsign` properties and an optional gzip+base64 blob child for non-parameter state (LAB X-11 needs no blob).

### 1.7 UI

- X-16 `ui/`: one `EffectEditor` engine, **960 x 560 fixed, not resizable**; `Theme` values (Oxide: background
  `0xff0b0907`, panel `0xff16110c`, border `0xff4a3620`, faint `0xff23190f`, dim `0xffa06a2a`, phosphor `0xffe0b23a`,
  bright `0xfff6e7b8`, accent `0xffff4a2a`; scanlines overlay, flicker 0.02, grain 0.35; Vacuum: ice `0xffb9f6ff` on
  `0xff040709`, accent `0xffffd36b`); `monoFont` = Consolas; rows of panels titled `"NAME // КИРИЛЛИЦА"`, `knob (id,
  caption, accent)`, `choices ({ { id, caption } })`, `Needle` meters, `outputPanel()` (MIX, OUTPUT, BYPASS), preset box
  with previous/next, SYSTEM line (version, latency, faults), 30 Hz `refresh()`.
- Single-plugin siblings: `Source/UI/CrtLookAndFeel.h` + `Widgets.h`, amber/green CRT looks, also fixed size.

**LAB X-11 will** use the X-16 engine with the Oxide (Deep Zone) theme: two rows of panels (GATE // ШЛЮЗ, DETECTOR //
ДЕТЕКТОР, LEVEL // УРОВЕНЬ, meters, OUTPUT // ВЫХОД), a gate-state lamp, a gain-reduction needle and a level-match
needle; names and Cyrillic subtitles are proposals for the user to confirm (open decision D9).

### 1.8 Docs

- README headings, single-plugin siblings (X-19 / X-0): `# LAB X-<n>`, `## What it does`, `## Playing it in FL
  Studio`, `## Sound sources`, `## Building on Windows`, `## Installing in FL Studio`, `## Testing`, `## Licence`.
  X-16: `# LAB X-16`, `## The effects`, `### In FL Studio`, `## Building`, `## Licence`.
- `docs/DESIGN.md`: design notes and "changes from the plan", dated verification tables (X-16 records the FL checks
  there); `LAB_X<n>_HANDOFF.md`: the plan plus an execution log (git-ignored); `lab_x<n>_lore_summary.md`: the user's
  lore brief; `docs/labx<n>-ui.png`: the editor snapshot the harness renders (`--png`).

### 1.9 Tests

- X-16: `python -I test/run_tests.py --plugin <slug> | --all [--only C1,T2] [--quick] [--skip-pluginval]` drives the
  render harness (`--sr --block --seconds --bpm --set id=v --ramp --step --program --program-at --in --signal --level
  --out --fuzz --inject stage@t --null-check --latency-check --roundtrip --list-params --selftest --png`) and
  asserts: **C1** null at bypass (1e-6), **C2** latency reported = measured at 44.1/48/96 kHz and constant through a
  60 s fuzz, **C3** state round trip, **C4** preset switch mid-signal (jump < 0.5, no fault), **C5** 300 s fuzz at
  44.1/48 kHz and blocks 1/7/64/3000/variable (no fault, non-finite or growth), **C6** `--inject` at every stage is
  caught and logged by name, **C7** pluginval 5 and 10, **C8** >= 20x real time (thread CPU time), **C9** editor PNG
  with every panel title, **P1** preset loudness trim; per-effect **T** tests in `test/python/effect_tests.py` analyse
  rendered WAVs with numpy (`labanalyze.py`, `labtest.py`'s `fixture()` builds sine/noise/impulse WAVs). C++ unit checks
  live in the DSP headers as `static selfTest()` and are printed by the harness `--selftest` (`DelayLine::selfTest`).
- Single-plugin siblings: the same shape (`Tests/run_tests.py` + `RenderHarness.cpp` + `analyze.py`).

**LAB X-11 will** port `harness_main.cpp`, `run_tests.py` and the two helpers unchanged where possible and add
`test/python/x11_tests.py` (T tests) plus `selfTest()` functions in `Gate.h` and `Leveler.h` (section 4).

### 1.10 Versioning and commits

`VERSION` in `project()` and `juce_add_plugin`; commit subjects `LAB X-19 0.1.2: fix past-the-end read in DelayLine
(idle blowouts)`, milestone commits `M0: ...`, `Wave 1: ...`; no tags in the single-plugin repos (X-16 plans
`suite-0.1.0`). Repo-local git identity `crystalball-dev` (no global identity on this machine). Licence AGPL-3.0
(`LICENSE` is the GNU AGPL v3 text in every sibling); "VST is a registered trademark of Steinberg Media Technologies
GmbH" in every README.

### 1.11 FL Studio facts recorded by the siblings (X-16 `docs/DESIGN.md`, `README.md`, handoff)

- FL 20.6.2 build 1549 compensates reported plugin latency to the sample (F2); the slot MIX knob with a latent plugin
  did not comb (F3), but the README still says to use the plugin's own MIX.
- A sidechain bus that starts **inactive** crashed FL on project reload (access violation inside
  `Fruity Wrapper_x64.dll`); the base therefore declares any sidechain bus active by default (F4 + reload passed).
  "Sidechain to this track" + the wrapper's *Processing > Connections* exposes it.
- FL keeps processing a mixer track with the transport stopped (self-generated sound needs `PlayGate`); *Smart
  disable* respects reported tails (F6); MIDI reaches a mixer effect through a MIDI Out port (F5); a command-line
  render is `FL64.exe /R /Ewav "<project>.flp"` (F8).
- Install one single-file `.vst3`, never next to a bundle folder of the same name; a loaded plugin keeps its file
  locked.
- Parameter ids and choice lists are append-only once installed; floats and choices only.

### 1.12 Conventions audit cross-check

[PENDING: the five-project audit workflow (one reader per project, a synthesis, three verifiers) is running; its
corrections to the table above, if any, land here before approval.]

---

## 2. Proposed structure, signal chain and routing

### 2.1 Directory tree (every file named; "copy" = X-16 file carried over with only the LABX16 -> LABX11 renames)

```
F:\_DEV\FL PLUGINS\LAB X-11\
  CMakeLists.txt                 project(LABX11 0.1.0); juce_add_plugin(LabX11 ...); LabX11Render harness; LabX11Config.h
  LICENSE                        AGPL-3.0 (copy)
  README.md                      section 8
  LAB_X11_HANDOFF.md             this plan + execution log (git-ignored)
  docs/DESIGN.md                 design notes, FL check results, changes from the plan
  docs/labx11-ui.png             rendered by LabX11Render --png
  .gitignore / .gitattributes    copies (X-16's)
  scripts/build.ps1, install.ps1, validate.ps1     copies with LabX11 / "LAB X-11"
  core/host/EffectProcessor.h    copy: the effect base (buses, params, smoothers, control loop, dry delay, mix, bypass,
                                 preset fade, guard, fault log, meters, scope, state)
  core/host/Params.h             copy (ParamSpec, commonParams, createLayout, Smoother)
  core/host/ZoneBus.h, ZoneBusPlatform.cpp   copy, unused (busClient = false): open decision D2
  core/dsp/Common.h              copy (dbToGain, onePoleCoef, wetSafety, blockPeak, hash, PlayGate)
  core/dsp/DelayLine.h           copy (the base's dry-path delay; the gate's lookahead ring)
  core/dsp/Transport.h           copy (the base reads the playhead into it)
  core/dsp/Filters.h             copy (SVF for the detector low cut)
  core/dsp/Follower.h            copy (peak follower; the detector's starting point)
  core/dsp/Gate.h                NEW  lab::dsp::VocalGate: detector, state machine, lookahead, range, selfTest()
  core/dsp/Leveler.h             NEW  lab::dsp::LevelMatcher: voice-gated measurement, slow gain, limits, selfTest()
  plugins/x11/LabX11.cpp         NEW  EffectInfo (params, stages, presets), X11Chain, X11Processor, X11Editor,
                                 createPluginFilter (the Gauss.cpp / Dosimeter.cpp shape)
  ui/EffectEditor.h, LookAndFeel.h, Theme.h, Widgets.h, Views.h   copies (PEERS view stays compiled, hidden)
  test/harness_main.cpp          copy (LABX11_HARNESS), --selftest extended to Gate/Leveler
  test/run_tests.py              copy, single plugin, C1-C9 + P1 + T1-T12
  test/python/labanalyze.py, labtest.py   copies
  test/python/x11_tests.py       NEW  the T tests (section 4.2)
  test/out/                      renders and fixtures (git-ignored)
```

Not copied: the other 22 `core/dsp` headers, `plugins/*`, `docs/EFFECTS.md`, `bus_host.cpp`, `audibility.py`,
`trim_presets.py` (its loudness-trim step is folded into the preset work, section 5.4).

Smallest-change-set argument: every line that is not `Gate.h`, `Leveler.h`, `LabX11.cpp`, `x11_tests.py` and the
renames already runs in 23 installed plugins. Alternatives considered: (a) the single-plugin `Source/` layout with a
fresh processor — re-implements what the base owns (guard, fault log, preset fade, dry delay); (b) adding X-11 as a
24th X-16 effect — not self-contained, wrong product name. Rejected; see D1.

### 2.2 Signal chain inside the plugin

```
host in (stereo; mono duplicated by the base)
  |
  +--> DETECTOR path (never heard): optional LOW CUT (SVF high-pass, 12 dB/oct) -> peak/RMS detector -> gate logic
  |
  v
[1] GATE   gain ramp from the state machine, applied LOOKAHEAD samples later than the detector decision   guard("gate")
  v
[2] LEVEL  slow matching gain, measured only while the gate is open                                       guard("level")
  v
base: wet safety -> MIX (against the dry signal delayed by LOOKAHEAD) -> OUTPUT -> BYPASS crossfade -> host out
```

Why this order: the gate first removes what the matcher must not measure (room tone, breaths) and the matcher reads the
gate's own state instead of guessing voice activity; the reference chains put the gate first in two of three projects
(section 5.1) and nothing between gate and compression that LAB X-11 replaces. EQ, saturation, delay and reverb stay in
the user's later slots, as today.

[PENDING: the exact gate state machine, detector, hysteresis, lookahead length, matcher law and limits come from the
DSP design panel (three designs, three judges, synthesis, three refuters, fixes); section 3 receives them.]

### 2.3 Latency

The gate's lookahead is the plugin's only latency: fixed per sample rate (open decision D4 for the length), reported by
`Chain::latency`, compensated by FL (F2) and matched by the base's dry delay, so MIX and BYPASS stay aligned and C1/C2
hold.

### 2.4 Routing: one track

Everything the user asked for works on the vocal insert alone. No stage *requires* another track:

- The gate's detector is the track's own signal (with its low cut), as DD Gate is used today (External off in all
  eight instances read from the projects).
- The level matcher holds the vocal to a **target level** on its own track. "Against the mix" is delivered by the
  user setting the target once per song (the fader relationship vocal-to-drums that the reference projects keep by
  hand: vocal insert 87-100 %, snare 75-108 %, kick 86-100 %) and the matcher then holding the vocal there phrase to
  phrase. A sidechain from the drum bus would let the target follow the drums instead; it is **not** in 0.1.0
  (D5): it needs an FL send into the plugin's sidechain input, and X-16's lesson is that a sidechain bus must then be
  declared active from the first build (ids and buses are append-only once installed), so the decision has to be made
  before code, not after.
- Doubles and ad-libs on their own inserts (DIEHARD inserts 21/22/34/38, SCALES 32/33) get their own LAB X-11 instance,
  as they get their own DD Gate today.

---

## 3. Parameters, defaults and ranges

[PENDING: the ParamSpec table (id, name, kind, min, max, default, skewCentre, smooth, unit, choices), the custom meter
slots and the smoothing choices come from the DSP design panel; provisional defaults marked PROVISIONAL are then set
from the stem measurements (section 5.3) and the DD Gate settings read from the projects (section 5.2).]

### 3.6 Real-time safety checklist (applies to every stage above)

- All buffers (lookahead ring, RMS window) sized in `prepare()` for the maximum lookahead and window at the sample rate;
  `process()` allocates nothing and calls nothing that can block.
- Every gain the chain applies is ramped per sample (gate: the attack/release ramps themselves; matcher: a one-pole or
  slew limit); parameter values arrive already smoothed per 16-sample block from the base; choice parameters that
  change a structure (detector mode) snap at a block boundary and reset only the detector, not the audio.
- `std::isfinite` is not needed per sample: the base zeroes non-finite input and the guard catches a non-finite stage.
- Denormals: `ScopedNoDenormals` in the base; envelopes decay toward zero with an added floor (1e-9) as the siblings do.
- Meters are written with `std::atomic<float>::store(relaxed)` into `Meters::custom`; the editor reads at 30 Hz.
- No `rand()`, no file or logging on the audio thread (faults go through the base's ring).
- Latency never changes at run time (C2).

---

## 4. Test plan

Framework: the X-16 pattern (section 1.9), unchanged, so the plugin inherits C1-C9 and P1 on day one; LAB X-11 adds
(a) deterministic C++ unit tests inside the DSP headers, run by `LabX11Render --selftest` and asserted by
`run_tests.py` as **T0**, and (b) Python render tests **T1-T12** that measure the gate's transitions and timing and the
matcher's behaviour on synthetic signals, plus one optional real-vocal regression on a user-supplied WAV.

### 4.1 C++ unit tests (`VocalGate::selfTest()`, `LevelMatcher::selfTest()`, printed as `selftest_<case>=<value>`)

[PENDING: the numbered cases, signals, expected results and tolerances from the design panel; every edge of the state
machine, attack/hold/release timing at 44.1/48/96 kHz and block sizes 1/16/64/512, hysteresis, range depth, no-click on
close, matcher convergence/accuracy, transient tolerance, freeze in gaps, gate+matcher interaction, denormal and
non-finite safety, determinism.]

### 4.2 Harness / Python tests

[PENDING: T1-T12 definitions mapped onto `fixture()` signals and `--set` / `--ramp` / `--step` / `--in`; C1-C9 and P1
notes specific to LAB X-11 (which settings C2 must cover, C6 stage names, C9 panel titles, P1 reference signal).]

### 4.3 FL Studio checks (recorded in `docs/DESIGN.md` as X-16 did)

- F-A: loads on a vocal insert in FL 20.6.2, single file, name "LAB X-11", category Fx/Dynamics.
- F-B: latency compensated (impulse on a parallel track, the X-16 F2 method).
- F-C: a gated vocal rendered from FL (`FL64.exe /R /Ewav`) equals the harness render of the same WAV and settings
  within 1e-5 (the plugin is deterministic; this proves the FL path).
- F-D: project save / reopen restores every parameter and the preset index (C3 inside FL).
- F-E: bypass and preset switch while playing produce no click audible on the vocal.

---

## 5. Presets derived from the reference projects

### 5.1 What the three projects do (read from the .flp files; method and limits in Appendix A)

| | GOLD `something.flp` (160 BPM) | DIEHARD `DIEHARD.flp` (130 BPM) | SCALES `scales.flp` (70 BPM) |
|---|---|---|---|
| Vocal insert | 50 (`vocal1` audio clip), 95 % | 30 (six clips: voc1, voc2, vocals, vocal 2, lyric take, lyric take clean), 100 %; doubles on 32 (26 %), 33, 38 (60 %) | 30 (vt, verse take 1, vocals maybe, first/second half takes), 87 % |
| Chain, slot order | Maximus -> **DD Gate** -> Parametric EQ 2 -> Fruity Compressor -> DD Compressor -> OTT -> (Delay 3, Convolver, Vocal Doubler, Kotelnikov all off) | **DD Gate** -> Maximus -> DD Expander -> Parametric EQ 2 -> Fruity Compressor -> DD Compressor -> (OTT off) -> Parametric EQ 2 -> (Fast Dist, Soft Clipper off) | **DD Gate** -> Maximus -> Parametric EQ 2 x2 -> Fruity Compressor -> DD Compressor -> TDR Kotelnikov -> Stereo Enhancer |
| Vocal routing | master + insert 51 (SlamPup, DD MonoDelay 54 %, Kotelnikov) | inserts 31 (vocal bus: SlamPup, Temper, Graillon 2, RoughRider3, EQ, Kotelnikov, Maximus; 80 %), 32 (tape delay / PanCake, 26 %), 36 (Kerovee, Gross Beat, Delay Bank; 87 %); not to master directly | master + sends 31-34 (convolver, doubler + PanCake 35 % / 27 %, OTT 50 %) |
| Drums | kick 2 (100 %, no FX), snare 4 (108 %, SlamPup), 808 7 (109 %, Fruity Limiter), hats 8 (94 %) | kick 8 (86 %, EQ, SlamPup, DD Overdrive) -> 808 bus 1 and drum bus 9 (SlamPup, Kotelnikov; volumes automated), snare 4 (75 %, EQ, OTT, Fast Dist, SlamPup, convolvers) | kick 1 (87 %, EQ, OTT, SlamPup, Soft Clipper), snare 4 (83 %, EQ, OTT, SlamPup), 808 6 (48 %, EQ, SlamPup, Fruity Limiter) |
| Master | Soundgoodizer -> Kotelnikov -> Fruity Limiter | Soundgoodizer -> Kotelnikov -> Youlean -> Fruity Limiter (94 %) | Soundgoodizer -> Kotelnikov -> Fruity Limiter |
| DD Gate (normalised 0..1) | Input 0.485, **Thresh 0.00**, Attack 0.13, Hold 0.00, Release 0.095, Low Cut 0, Trim 0.5, External off | Input 0.305, **Thresh 0.00**, Attack 0.175, Hold 0.155, Release 0.00 (insert 38: Attack 0.175, Hold 0.15) | Input 0.5, **Thresh 0.00**, Attack 0.14, Hold 0.00, Release 0.00 |
| Fruity Compressor (tentative reading) | threshold 0 dB (idle), ratio 4.8:1, gain -7 dB, attack 39 ms, release 387 ms | -30.8 dB, 21.1:1, +1.9 dB, 16 ms, 54 ms | -28.8 dB, 30:1, 0 dB, 15 ms, 200 ms |
| DD Compressor (normalised) | Thresh 0.26, Ratio 0.185, Attack 0, Release 0, Makeup 0.475, Mix 1 | Thresh 0.23, Ratio 1.0, Attack 0, Release 0, Makeup 0.085 | Thresh 0.26, Ratio 1.0, Attack 0, Release 0, Makeup 0.605 |

What this says about practice:

1. **One vocal insert, gate first.** Every take and comp of the lead feeds one insert; the gate is the first slot in
   two of three songs (GOLD puts Maximus before it). LAB X-11 therefore goes in slot 1 of the vocal insert and
   replaces DD Gate (and the level-control part of the Maximus / Fruity Compressor / DD Compressor stack); EQ and the
   colour stages keep their later slots.
2. **The gate is set to catch only the floor.** DD Gate's threshold sits at its minimum (normalised 0.0 = the lowest
   value the knob reaches; the plugin's listing says "threshold down to -40 dB", so the gates run at about -40 dBFS
   after the Input trim) in all three lead chains, with hold 0-0.155 and attack 0.13-0.175 of their ranges and the
   release at or near minimum. The user's working setting is a conservative gate that removes room tone and lets
   breaths and tails through; the default preset must do the same (D6: default threshold from the measured floor,
   never above the measured breath level).
3. **Level consistency is bought with three to four serial dynamics stages,** none of them a rider: Maximus,
   Fruity Compressor at about -30 dB with 20-30:1 (DIEHARD, SCALES), DD Compressor at maximum ratio with the fastest
   times, then Kotelnikov or OTT. The matcher's job is the phrase-to-phrase part of that work (the part the fader and
   the automation clips "Insert 30 - Volume" x3 in SCALES do by hand), not the fast peak control: it must leave
   transients to the compressors that follow.
4. **Drums are individually limited and clipped** (SlamPup, Fruity Limiter, Soft Clipper, OTT on kick, snare and 808),
   so their peaks are already consistent; the vocal is placed against them by fader at roughly unity (vocal 87-100 %
   vs snare 75-108 % and kick 86-100 %). The matcher's target is therefore a fixed level the user sets per song, and
   the "sits evenly against the snare" result comes from holding that level, not from reading the drums (section 2.4).
5. **No pitch processing precedes the gate** anywhere (Graillon 2 and Kerovee sit on buses after it); the matcher's
   detector can assume a dry, unpitched vocal.

### 5.2 Settings carried into presets

[PENDING: the stem measurements (noise floor, breath level, phrase RMS spread, gap and tail times) from the
reference-stems workflow; this section then states each preset value and the measurement it rests on.]

### 5.3 Measured stems

[PENDING: table of the measured values per project, with the method and caveats the analysts reported.]

### 5.4 Preset list (names are proposals; the user owns naming — D9)

| # | Name | Intent | Gate | Matcher |
|---|---|---|---|---|
| 0 | Init | the defaults: the measured-floor gate and a gentle matcher | defaults | defaults |
| 1 | Booth Floor | the three lead chains as read: floor-only gate, tails untouched | threshold at the floor + margin, long hold | on, slow |
| 2 | Room Tone | a noisier take: deeper range, slower release, higher threshold | PROVISIONAL | moderate |
| 3 | Rap Lead | DIEHARD / SCALES material: tight phrases, fast delivery | shorter hold, faster release | faster, tighter limits |
| 4 | Soft Take | quiet, airy delivery: shallow range so nothing disappears | shallow range, low threshold | more boost allowed |
| 5 | Ad-lib / Double | the DIEHARD 21/22/34/38 and SCALES 32/33 doubles: harder gate, matcher follows the lead's level | deeper range, higher threshold | on, same target |
| 6 | Gate Only | matcher amount 0 | defaults | off |
| 7 | Level Only | gate range 0 (open) | open | defaults |

Loudness trim: presets 1-5 set `output` so RMS on the reference vocal fixture stays within 1 dB of the input (P1);
6 and 7 keep `output` 0 dB (the X-16 "untrimmed" rule for utility presets).

---

## 6. Build targets and steps (Windows)

FL Studio on Windows loads VST3 (and VST2, which JUCE 9 cannot build without the retired VST2 SDK; the family ships
VST3 only). Targets, as every sibling:

| Target | Output | Purpose |
|---|---|---|
| `LabX11_VST3` | `build\LabX11_artefacts\Release\VST3\LAB X-11.vst3\Contents\x86_64-win\LAB X-11.vst3` | the plugin FL loads (installed as a single file) |
| `LabX11_Standalone` | `build\LabX11_artefacts\Release\Standalone\LAB X-11.exe` | quick GUI check without FL |
| `LabX11Render` | `build\LabX11Render_artefacts\Release\LabX11Render.exe` | offline render / test harness (`LABX11_HARNESS=1`) |

Steps (the sibling scripts, renamed):

```
powershell -ExecutionPolicy Bypass -File scripts/build.ps1            # cmake -G "Visual Studio 17 2022" -A x64; Release; the three targets
python -I test/run_tests.py --all                                     # C1-C9, P1, T0-T12 (add --quick for 30 s fuzz runs)
powershell -ExecutionPolicy Bypass -File scripts/validate.ps1 -Strictness 10
powershell -ExecutionPolicy Bypass -File scripts/install.ps1          # -> C:\Program Files\Common Files\VST3\LAB X-11.vst3 (close FL first)
```

Toolchain verified present: JUCE 9.0.3 at `F:\_DEV\tools\JUCE`, CMake 4.4.4 at `F:\_DEV\tools\cmake\...`, VS 2022
Build Tools at `F:\_DEV\tools\VS2022BuildTools`, pluginval at `F:\_DEV\tools\pluginval\pluginval.exe`, Python 3.14 with
numpy 2.5 / scipy 1.18 / soundfile. Debug builds: `-Config Debug` (static debug CRT).

Execution milestones (each ends with the tests named and an execution-log entry):

- **M0 Scaffold:** tree, CMake, renamed base, stub chain passing audio; C1, C2 (latency 0), C3, C7 pass; installs and
  loads in FL (F-A).
- **M1 Gate:** `Gate.h` + its `selfTest()`, parameters, guard stage; T0 gate cases, T1-T6; lookahead latency: C1, C2,
  F-B.
- **M2 Level matcher:** `Leveler.h` + `selfTest()`; T7-T11; gate + matcher interaction T12; C4-C6, C8.
- **M3 UI and presets:** editor rows, lamp and needles, the eight presets, P1 trim, C9, `docs/labx11-ui.png`.
- **M4 Listening and docs:** F-C, F-D, F-E in FL on the three reference vocals (the user's ears decide the defaults'
  last 1-2 dB), README, DESIGN.md, 0.1.0 commit `LAB X-11 0.1.0: vocal gate and level matcher`.

---

## 7. Open decisions and conservative defaults (please confirm or change)

| # | Decision | Conservative default in this plan | Alternatives | Why it matters |
|---|---|---|---|---|
| D1 | Code base | Port the X-16 effect base into a standalone LAB X-11 repo (tree in 2.1) | (a) single-plugin `Source/` layout with a new processor; (b) 24th effect inside X-16 | D1 fixes the whole tree; (a) re-implements the guard, fault log, preset fade and dry delay; (b) breaks "self-contained" and the product name |
| D2 | Zone Bus code | Carried unchanged, `busClient = false`, PEERS panel hidden (zero runtime effect) | strip ZoneBus.h / ZoneBusPlatform.cpp and the ~60 lines that touch them | keeping it means future X-16 base fixes port line for line; stripping it is cleaner but a modified base |
| D3 | Product identity | `PRODUCT_NAME "LAB X-11"`, `PLUGIN_CODE Lb11`, `BUNDLE_ID com.operationfairway.labx11`, `VST3_CATEGORIES Fx Dynamics`, version 0.1.0 | `Lbx11`-style code; "LAB X-11 <NAME>" with a lore product name as X-16 does | the plugin code and ids cannot change after the first install |
| D4 | Lookahead / latency | fixed [PENDING ms] at every sample rate, reported; never a run-time parameter | 0 (no latency, consonant onsets clipped by the attack); a choice parameter that requests the base's switch fade | C2 requires constant latency; FL compensates either way |
| D5 | Sidechain reference for the matcher | none in 0.1.0; no sidechain bus declared | declare an active-by-default sidechain bus now and add a REFERENCE choice later | buses are append-only once installed (X-16 F4 crash lesson); adding one later changes the plugin's bus layout |
| D6 | Default gate threshold | measured room-tone floor + margin, never above the measured breath level ([PENDING dBFS]) | the DD Gate setting read from the projects (about -40 dBFS) | too high chops soft consonants and tails; too low lets room tone through (fixable with THRESHOLD) |
| D7 | Matcher law | [PENDING: continuous slow gain vs stepped "quantized" gain] | the other | the user's word "quantizer" can mean either; the stepped mode is the lab-instrument reading |
| D8 | Matcher target | a fixed TARGET parameter (dBFS short-term), default [PENDING] | auto-learned from the first N seconds | auto-learn is less predictable in a mix and harder to test |
| D9 | Names and lore | panel titles GATE // ШЛЮЗ, DETECTOR // ДЕТЕКТОР, LEVEL // УРОВЕНЬ; presets as in 5.4; subtitle to be chosen by the user | anything the user prefers | naming is the user's; nothing in the code depends on it |
| D10 | Unit-test vehicle | `selfTest()` in the DSP headers + harness `--selftest` + Python assertions (the house pattern) | a separate `LabX11Tests` console target (JUCE UnitTest / CTest) | the house pattern needs no new target; a test executable is the more conventional "unit test" shape |
| D11 | Theme | Oxide (Deep Zone), fixed 960 x 560 | Vacuum (Ethereal Signal); a smaller window for a two-stage effect | the user's visual theme for X-11 decides; both themes exist in the engine |

[PENDING: the DSP design panel's own open decisions (detector type, hysteresis width, range floor, matcher limits,
transient tolerance) join this table.]

---

## 8. README outline (`README.md`, the single-plugin headings)

1. `# LAB X-11` — one paragraph: what it is, the family, the theme; the editor PNG.
2. `## What it does` — the two stages in order, what each control does, the meters; a short "how to set it on a vocal"
   (threshold from the lamp, target from the needle).
3. `## Using it in FL Studio` — slot 1 of the vocal insert, before EQ; latency (lookahead) and that FL compensates it;
   use the plugin's own MIX; one instance per vocal insert (doubles get their own); Smart disable is fine (no tail).
4. `## Signal chain` — the diagram of 2.2 with the guard stages and where MIX/OUTPUT/BYPASS act.
5. `## Parameters` — the table of section 3 (id, range, default, unit) exactly as the plugin exposes them.
6. `## Presets` — the list of 5.4 and what each is for.
7. `## Defaults and assumptions` — where each default came from (the three reference projects, the measured stems),
   the DD Gate reading and its limits, the Fruity Compressor reading marked tentative.
8. `## Building on Windows` — the four commands of section 6, toolchain versions.
9. `## Installing in FL Studio` — `install.ps1`, the single-file rule, rescan.
10. `## Testing` — `run_tests.py`, what C1-C9, P1 and T0-T12 check, the real-vocal regression (`--in`, never committed).
11. `## Licence` — AGPL-3.0, the VST trademark line.

`docs/DESIGN.md` carries the design notes, the FL check table (4.3) and every change from this plan with its reason.

---

## Appendix A. How the reference projects were read, and what is assumed

- The three `.flp` files (FL Studio 20.6.2 build 1549) were parsed directly with a small Python reader written for
  this plan (`flp_events.py`, `flp_model.py`, `flp_brief.py` in the session scratchpad, kept out of the repository):
  the FLhd/FLdt event stream, channel records (name, sample path, type, target insert), mixer inserts in file order
  (per-insert flags, ten slots with plugin internal name, display name and state, routing table, input/output) and
  the insert parameter block (slot enabled/mix, volume, pan, EQ). These fields follow the published FLP event
  numbering and are consistent across all three files (127 inserts, 1270 slots each). **Reliable:** chain order, plugin
  identities, routing, insert volumes, slot mix/enable.
- **Insert volume** is shown as raw/128 % (12800 = 100 % = 0 dB); FL's fader law between values is not derived, so
  percentages are reported, not dB.
- **VST plugin states** sit in Fruity Wrapper chunk 53 as one float32 per parameter, normalised 0..1. The parameter
  order of the Dead Duck plugins was inferred from their DLL string tables (DD Gate: Bypass, External, Listen, Trim,
  Low Cut, Input, Thresh, Attack, Hold, Release; DD Compressor adds Ratio, Makeup, Mix) and checked for consistency
  (Input 0.5 = unity and Mix 1.0 in every compressor instance). The dB/ms mapping of the normalised values is **not**
  known: the plugin's public listing gives Input +/-20 dB and "threshold down to -40 dB", nothing for attack, hold,
  release or the low cut (no manual is published). Assumption: threshold 0.0 = -40 dB.
- **Native FL plugin states** are undocumented binaries. The Fruity Compressor's eight int32 values are read as
  (version, threshold x10 dB, ratio x10, gain x10 dB, attack x10 ms, release ms, type, ?) because the ratio field
  reaches exactly 300 (= the plugin's 30:1 maximum) and the thresholds fall at -28.8/-30.8 dB; this is a plausible
  reading, marked tentative wherever used. Maximus, Parametric EQ 2 and Fruity Limiter states were not decoded.
- Send/route *levels* (as opposed to the routing table) were not decoded (the parameter ids FL uses for them did not
  match the documented layout); routing destinations are reliable, their levels are not stated.
- FL Studio itself is not installed under `C:\Program Files\Image-Line\FL Studio 20` on this machine (only the
  Downloader folder); the FL checks in 4.3 assume the FL 20.6.2 installation X-16 used on 2026-10-13.

## Appendix B. Stem measurements

[PENDING: the reference-stems workflow's tables (per file: role, sample rate, duration, peak, RMS, loudness, crest,
short-term percentiles; per raw take: floor, breath level, phrase RMS spread, gap and tail times; per mix: vocal-to-
instrumental ratio and drum-hit levels) and the independent re-measurement verdicts.]

## Execution log

Executed on request (2026-10-18 to 2026-10-19) in a cloud session on branch `claude/task-4jz9sq`. The Windows machine,
`F:\_DEV` and FL Studio were not reachable from it, so the build ran on Linux against JUCE 9.0.3 (cloned) and the X-16
base cloned read-only from github.com/crystalball-dev/lab-x-16. `docs/DESIGN.md` has the details and the test table.

- **Specs.** `docs/SPEC_LOOKAHEAD_GATE_AND_ALIGNMENT.md` (gate and alignment; reviewed before code, revision 3 records
  the build) and `docs/SPEC_LEVEL_MATCHER.md` (the matcher, from a design panel; its implementation record wins over
  its text). They fill sections 2.2, 3 and 4 marked PENDING above.
- **M0 scaffold.** The X-16 base ported into the tree of 2.1 with the LABX16 -> LABX11 renames; one `juce_add_plugin`
  (`Lb11`, Fx Dynamics) plus `LabX11Render`; the three scripts. Builds clean; C1-C3 pass.
- **M1 gate.** `Gate.h`, `Align.h`, 33 self-tests. A three-lens review with two refuters per finding found a click on
  every preset change (the 5 ms fade-in equals the 5 ms lookahead) and a hold shortened by the lookahead, among eight
  defects; all fixed, each with a test that fails without the fix.
- **M2 matcher.** `Leveler.h` + `LevelerSelfTest.h`, 125 self-tests reproducing the design prototype to the digit. One
  change from its spec: the 20 ms voicing test at +3.5 dB (breathy voice admitted on 66 % of hops, breath noise on
  none).
- **M3.** The eight presets of 5.4 (no OUTPUT trims needed, P1), the editor, T7-T13 render tests, the editor PNG.
- **Review of M2.** Nine findings upheld: room rumble reads as voice to the matcher (documented and pinned by tests; a
  periodicity test is left as the user's option) and eight test gaps (eleven mutants now caught).
- **Verification.** `run_tests.py --all`: 24 of 24 (C1-C9 with pluginval 5 and 10, T0-T13 with 179 self-tests, P1).
- **Still open:** section 5.2 / 5.3 / Appendix B stem measurements (THRESHOLD and TARGET defaults stay PROVISIONAL);
  the FL checks F-A to F-E on Windows; decisions D7 and D8 (as built, see `docs/DESIGN.md`) and D9 naming for the user.
