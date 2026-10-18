# LAB X-11 — Structural specification: lookahead gate and transient micro-delay alignment

Status: **specification, no code.** Derived exclusively from `LAB_X11_HANDOFF.md` (2026-10-18 draft). Where the
handoff says PENDING, this document states a **PROPOSED** value and lists it in section 9 for confirmation against
the "LAB X-11 vocal plugin plan" chat. No external audio, DLL or folder scan informed anything below.

Scope: the two real-time modules that sit under guard stage `gate` in the X-11 chain:

1. `lab::dsp::VocalGate` — the lookahead noise gate (detector, hysteresis state machine, gain ramps, range).
2. `lab::dsp::TransientAlign` — the fixed micro-delay that places the audio LOOKAHEAD samples behind the detector so
   the gate's attack ramp finishes before a consonant onset arrives, and keeps the base's dry path, MIX and BYPASS
   sample-aligned with it.

The level matcher (`Leveler.h`) is out of scope except where it reads the gate's state (section 7.3).

---

## 1. Constraints inherited from the handoff

| Constraint | Source | Consequence for these modules |
|---|---|---|
| X-16 effect base: `EffectProcessor` + `Chain`, header-only DSP in `core/dsp/`, namespace `lab::dsp` | 1.3, 2.1 | `Gate.h` is header-only; the alignment delay reuses `core/dsp/DelayLine.h` |
| Parameters arrive as `const float* v` per **16-sample control block**, already smoothed (15 ms one-pole) | 1.4 | Modules expose `control (const float* v)` and never read APVTS |
| Floats and choices only, never bool; ids `snake_case`, append-only | 1.4 | Hysteresis and low cut are floats; detector mode is a choice |
| Guard after each named stage; stages for X-11 are `gate` and `level` | 1.5, 2.2 | `X11Chain::process` calls `guard ("gate", ...)` once after both modules together |
| Latency fixed per sample rate, read at `prepareToPlay`, never changes (C2) | 1.5, 2.3, D4 | `TransientAlign::latency (sr)` is a pure function of sample rate |
| Dry path delayed by the base's `dryLine` to the reported latency | 1.5, 2.3 | The chain reports LOOKAHEAD; the base aligns dry automatically |
| Detector: own track only, optional sidechain low cut (SVF high-pass, 12 dB/oct), peak/RMS | 2.2, 2.4 | No sidechain bus in 0.1.0 (D5) |
| Gain the chain applies is ramped per sample | 1.4, 3.6 | Gate gain is the attack/hold/release ramp itself |
| Envelope floor 1e-9, `ScopedNoDenormals` in the base | 3.6 | Followers add the floor; no per-sample `isfinite` |
| Meters via `std::atomic<float>` into `Meters::custom`, editor at 30 Hz | 3.6, 1.7 | Gate state lamp and gain-reduction needle |
| `static selfTest()` in each DSP header, printed by `LabX11Render --selftest` (T0) | 1.9, 4.1, D10 | Section 8.1 |
| The working gate catches only the floor: DD Gate threshold at minimum (about -40 dBFS), hold 0-0.155, attack 0.13-0.175 of range, release near minimum; breaths and tails pass | 5.1 (2), D6 | Defaults in section 4.4 are conservative |

---

## 2. Placement in the X-16 tree

```
core/dsp/Common.h        copy   dbToGain, onePoleCoef, blockPeak (used by both modules)
core/dsp/DelayLine.h     copy   integer-sample ring; TransientAlign wraps it, the base's dryLine is a second instance
core/dsp/Filters.h       copy   SVF; the detector low cut
core/dsp/Follower.h      copy   peak follower; the detector's starting point (extended, not replaced)
core/dsp/Gate.h          NEW    lab::dsp::VocalGate     (section 4)  + VocalGate::selfTest()
core/dsp/Align.h         NEW    lab::dsp::TransientAlign (section 5) + TransientAlign::selfTest()
plugins/x11/LabX11.cpp   NEW    X11Chain wires VocalGate -> TransientAlign -> LevelMatcher under the two guard stages
```

The handoff's tree (2.1) names `Gate.h` and `Leveler.h` only and keeps the lookahead ring inside the gate. This spec
splits the alignment delay into `Align.h` so the latency contract (C2) and the gain-application point are testable on
their own; `Gate.h` still owns the lookahead *amount*. If the user prefers the handoff's two-file tree, `TransientAlign`
folds into `Gate.h` unchanged (open point O1).

---

## 3. Signal topology under guard stage `gate`

```
in (L, R, n samples, undelayed)
  |
  +-- detector key = 0.5 * (L + R)  (mono sum; PROPOSED, see O2)
  |      -> LOW CUT   SVF high-pass, 12 dB/oct, cutoff = v[det_lowcut]   (bypassed below 20 Hz)
  |      -> DETECTOR  peak or RMS follower (choice det_mode)             -> keyLevel (dB, floored at -120)
  |      -> STATE MACHINE  threshold + hysteresis, attack / hold / release  -> state
  |      -> GAIN RAMP      per-sample target g[t] in [rangeGain .. 1]       -> gainEnvelope[n]
  |
  +-- TransientAlign: L, R delayed by LOOKAHEAD samples                     -> Ld, Rd
  |
  v
  out = (Ld, Rd) * gainEnvelope          (the envelope computed on the undelayed key is applied to the delayed audio)
  |
  guard ("gate", out)
  v
  LevelMatcher (reads VocalGate::isOpen() for voice gating)
  guard ("level", out)
```

The detector therefore sees every sample LOOKAHEAD samples before the audio it governs. An attack ramp that starts when
the key crosses the open threshold is complete by the time the onset reaches the output, provided
`attackSamples <= LOOKAHEAD`; longer attacks still open "late" by `attack - LOOKAHEAD`, which is the ordinary
non-lookahead behaviour and acceptable.

---

## 4. Module A: `lab::dsp::VocalGate` (`core/dsp/Gate.h`)

### 4.1 Responsibilities

- Produce one gain value per sample from the undelayed key signal.
- Hold a deterministic five-state machine with hysteresis so breaths and word tails are not chattered.
- Never allocate, lock or call I/O after `prepare()`.
- Publish state and gain reduction for the editor's lamp and needle.
- Expose the lookahead length so `TransientAlign` and `Chain::latency` agree by construction.

### 4.2 Public interface (sketch)

```cpp
namespace lab::dsp
{
struct VocalGate
{
    // Called from Chain::prepare. Sizes nothing but the detector state; the ring lives in TransientAlign.
    void prepare (double sampleRate, int maxBlock) noexcept;
    void reset() noexcept;                       // detector, state, gain -> closed, no click (gain starts at range)

    // Called once per 16-sample control block with the base's smoothed values.
    // Index constants come from EffectInfo::params order (section 4.4).
    void control (const float* v) noexcept;

    // Writes n gain values (linear, 0..1) from the undelayed key; does not touch the audio.
    void computeGain (const float* keyL, const float* keyR, float* gainOut, int n) noexcept;

    // Lookahead in samples for this sample rate (integer, constant for the life of prepare()).
    static int lookaheadSamples (double sampleRate) noexcept;

    bool  isOpen()        const noexcept;        // state == Open || Hold (for the matcher)
    float gainReductionDb() const noexcept;      // for the needle, last control block
    int   stateIndex()    const noexcept;        // 0 closed, 1 attack, 2 open, 3 hold, 4 release (lamp)

    static void selfTest();                      // section 8.1, prints selftest_<case>=<value>
};
}
```

Design rule from the handoff (1.3): JUCE call style with a space before the parenthesis, `noexcept` on the audio
path, `(float)` casts, `std::` numerics, no member prefixes.

### 4.3 Internal structure

| Component | Type | Behaviour |
|---|---|---|
| `lowCut` | `Filters.h` SVF, high-pass, Q 0.707, 12 dB/oct | Cutoff from `det_lowcut`; coefficient recomputed in `control()` only; when cutoff <= 20 Hz the stage is bypassed (key passes untouched) |
| `peakFollower` | `Follower.h` peak follower | Instant attack, release one-pole; coefficient `onePoleCoef (detRelease, sr)`; floor 1e-9 |
| `rmsWindow` | running mean of squares, fixed window | Window length fixed per sample rate (PROPOSED 10 ms); ring sized in `prepare()`; square-root once per sample |
| `keyDb` | float | `20 log10 (max (level, 1e-6))`, i.e. floored at -120 dB; one log per sample (acceptable at C8's 20x real-time budget; a 16-sample decimation is the fallback, O3) |
| `state` | enum Closed, Attack, Open, Hold, Release | Section 4.5 |
| `holdCounter` | int samples | Loaded with `holdSamples` on entering Hold |
| `gain` | float, current linear gain | Ramped every sample toward `target` with the state's coefficient |
| `rangeGain` | float | `dbToGain (-range)`; the closed floor; `range = 0` means the gate is transparent (preset 7 "Level Only") |
| `meterOpen`, `meterGr` | `std::atomic<float>` | Stored with `memory_order_relaxed` once per control block |

### 4.4 Parameters (ParamSpec rows, gate and detector portion; ids are proposals, see O4)

Common `mix`, `output`, `bypass` come first per the base (1.4). The gate's rows follow in this order.

| id | name | kind | min | max | default | skewCentre | smooth | unit | notes |
|---|---|---|---|---|---|---|---|---|---|
| `gate_thr` | Threshold | real | -80 | 0 | **-40 PROPOSED** | -30 | linear | dB | D6: measured floor + margin, never above breath level; -40 is the DD Gate reading |
| `gate_hyst` | Hysteresis | real | 0 | 12 | **4 PROPOSED** | 0 | linear | dB | close threshold = thr - hyst |
| `gate_attack` | Attack | real | 0.1 | 50 | **1 PROPOSED** | 5 | log | ms | ramp time rangeGain -> 1; attacks <= lookahead are fully pre-empted |
| `gate_hold` | Hold | real | 0 | 500 | **40 PROPOSED** | 50 | log | ms | DD Gate hold 0-0.155 of range in the reference chains |
| `gate_release` | Release | real | 5 | 2000 | **120 PROPOSED** | 200 | log | ms | ramp time 1 -> rangeGain; release near minimum in the reference chains, but tails must survive |
| `gate_range` | Range | real | 0 | 80 | **24 PROPOSED** | 24 | linear | dB | attenuation when closed; 0 = open |
| `det_lowcut` | Low Cut | real | 20 | 500 | **80 PROPOSED** | 100 | log | Hz | 20 = off; keeps rumble and plosive energy out of the detector |
| `det_mode` | Detector | choice | — | — | **Peak PROPOSED** | — | none | — | `"Peak\|RMS"`; snaps at a block boundary, resets only the detector |
| `det_release` | Det Release | real | 1 | 200 | **20 PROPOSED** | 20 | log | ms | peak follower release; shorter = more responsive, more chatter |

Smoothing follows the house convention: times and frequencies `log`, levels `linear`, choices `none`.
The base hands `control()` all rows; the gate converts ms to samples and dB to linear once per block.

### 4.5 State machine

```
                keyDb > openThr                  gain reached 1.0
   CLOSED  ───────────────────────►  ATTACK  ──────────────────────►  OPEN
     ▲                                  │                               │
     │                                  │ keyDb < closeThr              │ keyDb < closeThr
     │                                  ▼                               ▼
     │  gain reached rangeGain       RELEASE  ◄──── hold expired ────  HOLD
     └──────────────────────────────────┘ ▲                             │
                                          └──── keyDb > openThr ────────┘ (back to ATTACK from HOLD or RELEASE)
```

- `openThr = gate_thr`, `closeThr = gate_thr - gate_hyst` (dB on the detector's key).
- **Closed:** target `rangeGain`; move to Attack when `keyDb > openThr`.
- **Attack:** target 1, coefficient from `gate_attack`; on `keyDb < closeThr` go to Release (a false start never gets a
  hold); on reaching 1 within 0.1 dB go to Open.
- **Open:** target 1; on `keyDb < closeThr` go to Hold and load `holdCounter`.
- **Hold:** target 1; `holdCounter--` each sample; `keyDb > openThr` returns to Open (counter cleared); counter zero
  goes to Release.
- **Release:** target `rangeGain`, coefficient from `gate_release`; `keyDb > openThr` goes to Attack (ramp continues
  from the current gain, no jump); gain within 0.1 dB of `rangeGain` goes to Closed.
- The ramp is a one-pole in the linear domain toward `target`, with time constant such that the 10 %-90 % transition
  equals the parameter (`onePoleCoef` from `Common.h`, as the siblings do). Linear-domain ramps give the "closes fast
  at the end" shape that hides room tone; a log-domain option is O5.
- Changing `gate_range` while Closed or in Release re-targets without a state change (the base's 15 ms smoothing makes
  this a slow glide).
- `reset()` sets state Closed, `gain = rangeGain`, clears followers and counters.

Timing is specified in samples computed from the sample rate at `prepare()`/`control()`, so the same preset times
behave identically at 44.1, 48 and 96 kHz (T0 cases, section 8.1).

### 4.6 Per-block algorithm (`computeGain`)

```
for i in 0..n-1:
    key   = 0.5f * (keyL[i] + keyR[i])
    key   = lowCut.process (key)                     // skipped when bypassed
    level = mode == Peak ? peakFollower.process (|key|) : rmsWindow.process (key*key)
    keyDb = 20 * log10 (max (level, 1e-6f))
    advance state (section 4.5)
    gain += coef[state] * (target[state] - gain)     // one-pole ramp, per sample
    gainOut[i] = gain
after the loop (once per 16-sample control block, not per sample):
    meterOpen.store (state is Open or Hold ? 1 : 0, relaxed)
    meterGr.store (20 * log10 (gain), relaxed)
```

No branch inside the loop allocates or calls a library function that may block. `std::log10` and `std::sqrt` are the
only transcendental calls.

---

## 5. Module B: `lab::dsp::TransientAlign` (`core/dsp/Align.h`)

### 5.1 Purpose

The gate's decision is made on the undelayed key. The audio is delayed by exactly LOOKAHEAD samples so the gain ramp
computed from a transient at time *t* is applied to the audio that carries that transient at *t + LOOKAHEAD*. This is
the only latency in the plugin (2.3) and the only source of "micro-delay" in the chain. The module also guarantees
that the base's dry path, MIX and BYPASS are aligned, by being the single authority on the latency value.

### 5.2 Public interface (sketch)

```cpp
namespace lab::dsp
{
struct TransientAlign
{
    // Fixed lookahead per sample rate (D4). Never a parameter, never changes after prepare().
    static constexpr float lookaheadMs = 5.0f;          // PROPOSED, see O6
    static int latency (double sampleRate) noexcept     // round (lookaheadMs * 1e-3 * sr), clamped >= 1
    {
        return std::max (1, (int) std::lround (lookaheadMs * 1e-3 * sampleRate));
    }

    void prepare (double sampleRate, int maxBlock) noexcept;   // sizes two DelayLine rings to latency (sr) + maxBlock
    void reset() noexcept;                                     // clears rings (silence), no click because gain is also reset

    // In place: writes the audio delayed by latency (sr) samples. n <= maxBlock. Allocation-free.
    void process (float* l, float* r, int n) noexcept;

    static void selfTest();
};
}
```

### 5.3 Behaviour and invariants

- **Integer delay only.** VST3 latency reporting is in whole samples; a fractional delay would misalign MIX/BYPASS and
  break C1. `latency()` returns the same integer `Chain::latency (sampleRate)` reports and `VocalGate::lookaheadSamples`
  returns (they call the same function; the chain asserts equality off the audio path in `prepare()`).
- **Fixed per sample rate.** The value is computed once in `prepare()` from the sample rate given to `prepareToPlay`;
  `process()` never reads a parameter. C2 (latency reported = measured at 44.1/48/96 kHz, constant through a 60 s
  fuzz) holds by construction.
- **Ring storage.** Two `DelayLine` instances (L, R) from `core/dsp/DelayLine.h`, sized `latency + maxBlock` so a
  block of any size the host sends (C5 blocks 1/7/64/3000/variable) is read and written without wrap errors. The X-19
  "past-the-end read in DelayLine (idle blowouts)" fix noted in 1.10 is the reason the copy is taken from X-16, not
  reimplemented.
- **Order of operations in the chain.** `VocalGate::computeGain` runs on the *undelayed* buffer first, then
  `TransientAlign::process` delays the buffer in place, then the gain is multiplied in. The gain array and the audio
  buffer are both `n` long; the gain for sample *i* was decided from key sample *i*, which after the delay sits at
  output sample *i*. No extra bookkeeping is needed because the gate's own ramp provides the "pre-open" (section 3).
- **Reset without clicks.** `reset()` is only called by the base on a fault or a preset switch, both of which the base
  wraps in a fade (1.5), so emptying the ring is inaudible.
- **No mode switch.** A "lookahead off" choice would change latency and need the base's switch fade; D4 rejects it for
  0.1.0.

### 5.4 Why a separate module

The base already owns a dry delay; the wet path needs its own. Keeping the wet delay out of `VocalGate` means the
gate's `selfTest()` can check timing in detector samples without a delay offset, and `TransientAlign::selfTest()` can
check the impulse offset and C2 integer rule alone. The cost is one more header and one more `prepare()` call.

---

## 6. Chain integration (`plugins/x11/LabX11.cpp`, class `X11Chain`)

```cpp
struct X11Chain : lab::host::Chain
{
    int latency (double sr) const noexcept override { return lab::dsp::TransientAlign::latency (sr); }

    void prepare (double sr, int maxBlock) override
    {
        gate.prepare (sr, maxBlock); align.prepare (sr, maxBlock); level.prepare (sr, maxBlock);
        gainBuf.assign ((size_t) maxBlock, 1.0f);        // the one allocation, here only
        jassert (lab::dsp::VocalGate::lookaheadSamples (sr) == lab::dsp::TransientAlign::latency (sr));
    }

    void reset() noexcept override { gate.reset(); align.reset(); level.reset(); }

    void control (const float* v) noexcept override { gate.control (v); level.control (v); }

    void process (float* l, float* r, int n) noexcept override
    {
        gate.computeGain (l, r, gainBuf.data(), n);     // decision on undelayed audio
        align.process (l, r, n);                        // audio now LOOKAHEAD samples behind the decision
        for (int i = 0; i < n; ++i) { l[i] *= gainBuf[i]; r[i] *= gainBuf[i]; }
        guard ("gate", l, r, n);
        level.process (l, r, n, gate.isOpen());         // measured only while the gate is open (2.2)
        guard ("level", l, r, n);
    }
};
```

`EffectInfo::stages = { "gate", "level" }` so C6's `--inject stage@t` reaches both by name. The base handles wet
safety, MIX against the dry signal delayed by `latency()`, OUTPUT and the BYPASS crossfade (2.2, 1.5).

### 6.1 Block-size independence

`computeGain` and `process` carry all state across calls; a block of 1 and a block of 3000 produce the same sample
stream (C5). The 16-sample control cadence is the base's; parameter changes inside a block take effect at the next
control call, as every X-16 effect does.

### 6.2 Meters (`Meters::custom` slots)

| slot | source | editor use |
|---|---|---|
| 0 | `VocalGate::meterOpen` (0/1, plus 0.5 for Hold, PROPOSED) | gate-state lamp in GATE // ШЛЮЗ |
| 1 | `VocalGate::meterGr` (dB, <= 0) | gain-reduction needle |
| 2 | `LevelMatcher` gain (dB) | level-match needle (out of scope here) |

---

## 7. Cross-module contracts

### 7.1 Latency

`Chain::latency (sr)` = `TransientAlign::latency (sr)` = `VocalGate::lookaheadSamples (sr)`. One function, three
callers. Reported once in `prepareToPlay` via `setLatencySamples`; the base's `dryLine` uses the same value.

### 7.2 Gain timing relative to the detector

For a step in the key at sample *t*: gain begins rising at *t*, reaches 90 % at *t + attackSamples*; the audio step
appears at the output at *t + LOOKAHEAD*. With default values (attack 1 ms, lookahead 5 ms PROPOSED) the gate is fully
open 4 ms before the consonant. `selfTest` case G7 asserts this to the sample.

### 7.3 Gate -> matcher

`LevelMatcher::process` takes `gateOpen` and freezes its measurement when false, so room tone and breaths that the gate
removes are never measured (2.2 "why this order"). The matcher receives the *delayed, gated* audio, so its own timing is
relative to the output stream; it needs no knowledge of LOOKAHEAD.

---

## 8. Test specification for the two modules

### 8.1 `VocalGate::selfTest()` and `TransientAlign::selfTest()` (T0; printed as `selftest_<case>=<value>`)

Run at 44.1, 48 and 96 kHz and blocks 1/16/64/512 unless stated. Signals are synthetic and generated in the test, not
read from files.

| case | signal | assertion | tolerance |
|---|---|---|---|
| G1 closed floor | silence, then -60 dBFS noise | state stays Closed, gain == rangeGain | exact after reset |
| G2 open | step to -20 dBFS sine | state reaches Open; gain hits 0.9 within attack ms | ±1 sample per 1 ms of attack |
| G3 hold | burst then silence | Release starts exactly `holdSamples` after key < closeThr | ±1 sample |
| G4 release | as G3 | gain reaches rangeGain + 10 % of span within release ms; then Closed | ±2 % of release |
| G5 hysteresis | sine held at thr - hyst/2 after opening | stays Open/Hold, never Release | exact |
| G6 chatter | key toggling ±1 dB around thr at 200 Hz | state changes <= 2 per 100 ms with hyst = 4 dB | count |
| G7 pre-open | impulse in key at *t* | through the chain, output impulse at *t + LOOKAHEAD* carries gain >= 0.99 | exact sample |
| G8 range 0 | range = 0 dB | gainOut all 1.0 regardless of state | exact |
| G9 block equivalence | same 2 s signal, blocks 1 vs 512 | identical gain streams | 1e-7 |
| G10 rate equivalence | same ms settings at 44.1 / 48 / 96 kHz | state transition times in ms agree | ±0.05 ms |
| G11 mode switch | Peak -> RMS mid-signal | no NaN, gain continuous (no jump > 0.1) | — |
| G12 denormal / finite | 1e-30 input for 10 s | no NaN/inf; followers at floor | finite |
| G13 determinism | same input twice after reset | bit-identical gain streams | exact |
| A1 latency integer | each sample rate | `latency (sr)` is int, >= 1, == round (ms * sr / 1000) | exact |
| A2 impulse offset | impulse at 0 through `process` | appears at `latency (sr)` and nowhere else | exact |
| A3 block independence | 3 s noise, blocks 1/7/64/3000/variable | output identical to block 512 | 1e-7 |
| A4 reset | fill, reset, process silence | output all zero | exact |

### 8.2 Harness / Python tests touching these modules (T1-T6 of the handoff's T1-T12 budget)

| test | fixture (`labtest.fixture()`) | harness flags | assertion |
|---|---|---|---|
| T1 floor removal | -55 dBFS noise, 3 s | defaults | output RMS <= -55 - range + 1 dB |
| T2 tail survival | sine burst with 300 ms exponential tail | defaults | tail energy within 1 dB of input until it crosses closeThr |
| T3 onset integrity | click train at -12 dBFS, 20 ms apart | defaults | first-sample amplitude of each click within 0.5 dB of input (lookahead works) |
| T4 attack/hold/release | gated burst pattern | `--set gate_hold=100 --set gate_release=200` | measured hold/release from envelope within 5 % |
| T5 latency vs reported | impulse | `--latency-check` at 44.1/48/96 kHz | C2 passes, value == `TransientAlign::latency` |
| T6 parameter ramp | `--ramp gate_thr -80..0` over 10 s | — | no fault, no sample jump > 0.5 at any 16-sample boundary |

C1 (null at bypass) and C2 are the direct guarantees of section 5; C6 must list `gate` and `level`; C9 must find the
panel titles GATE // ШЛЮЗ and DETECTOR // ДЕТЕКТОР (D9 names).

---

## 9. Proposed values and open points (confirm against the "LAB X-11 vocal plugin plan" chat)

| # | item | proposed here | handoff status | why it matters |
|---|---|---|---|---|
| O1 | File split | `Gate.h` + `Align.h` | tree names `Gate.h` only | testability of latency vs. one file fewer |
| O2 | Detector key | mono sum 0.5 (L + R) | unspecified | max (|L|, |R|) is the alternative; vocals are mono so either works |
| O3 | dB conversion rate | per sample | unspecified | per-16-sample decimation if C8 (20x real time) is tight |
| O4 | Parameter ids | `gate_*`, `det_*` as in 4.4 | section 3 PENDING | append-only once installed (1.4) |
| O5 | Ramp domain | linear one-pole | PENDING (DSP panel) | log-domain closes more gently; audibility decides |
| O6 | Lookahead length | **5 ms** fixed | D4 PENDING ms | <= 10 ms keeps FL compensation trivial; longer buys nothing for consonants |
| O7 | Default threshold | -40 dB | D6 PENDING dBFS (floor + margin) | from the DD Gate reading; stem measurements may lower it |
| O8 | Hysteresis default | 4 dB | PENDING (DSP panel) | too small chatters, too large delays close |
| O9 | RMS window | 10 ms fixed | unspecified | fixed per sample rate so no run-time resize |
| O10 | Hold lamp value | 0.5 for Hold state | unspecified | purely visual |

Everything else in sections 4-8 follows directly from the handoff text and needs no confirmation.

---

## 10. Delivery checklist for milestone M1 (Gate) per handoff section 6

- [ ] `core/dsp/Gate.h` with `VocalGate`, `selfTest()` cases G1-G13
- [ ] `core/dsp/Align.h` with `TransientAlign`, `selfTest()` cases A1-A4 (or folded into `Gate.h` per O1)
- [ ] `EffectInfo::params` rows of 4.4 after the common three; `stages = { "gate", "level" }`
- [ ] `X11Chain` as in section 6; `latency()` from `TransientAlign`
- [ ] `test/harness_main.cpp` `--selftest` extended to both modules
- [ ] `test/python/x11_tests.py` T1-T6 of 8.2
- [ ] C1, C2 (non-zero latency), C5, C6 green; F-B in FL
- [ ] `docs/DESIGN.md` records the confirmed values of section 9 and any change from this spec
