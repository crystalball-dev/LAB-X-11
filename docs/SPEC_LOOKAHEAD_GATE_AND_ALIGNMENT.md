# LAB X-11 — Structural specification: lookahead gate and transient micro-delay alignment

Status: **specification, no code.** Derived exclusively from `LAB_X11_HANDOFF.md` (2026-10-18 draft). Where the
handoff says PENDING, this document states a **PROPOSED** value and lists it in section 9 for confirmation against
the "LAB X-11 vocal plugin plan" chat. No external audio, DLL or folder scan informed anything below.

Revision 2: reviewed by a three-lens panel (real-time/JUCE, vocal DSP, X-16 conformance) with two refuters per
finding; the ten confirmed findings are folded in (dB-domain slew ramp, hold-counter order, hold minimum, RMS
accumulator and window, meters owned by the chain, explicit latency rounding, M1 scope). Section 11 lists them.

Revision 3 (implementation): the gate and the alignment are built (`core/dsp/Gate.h`, `core/dsp/Align.h`) on the real
LAB X-16 base and reviewed again, code against this spec. Where the build differs from revisions 1-2, section 12 says
what changed and why, and the affected rows below are corrected; section 12 wins over anything older in this file.

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

The detector therefore sees every sample LOOKAHEAD samples before the audio it governs. With the slew ramp of 4.5 the
attack is complete exactly `attackSamples` after the key crosses the open threshold, so the gate is fully open before
the onset reaches the output whenever `attackSamples <= LOOKAHEAD`; longer attacks open "late" by
`attack - LOOKAHEAD`, which is the ordinary non-lookahead behaviour and acceptable. This sample-exact guarantee
holds for `det_mode = Peak`; in RMS mode the key rises over the window length first (4.3), so the pre-open margin is
`LOOKAHEAD - windowSamples - attackSamples`.

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
    // Called from Chain::prepare. Sizes the RMS ring; initialises every control-derived member (thresholds,
    // slew steps, holdSamples, rangeGain, detector coefficients) from the 4.4 defaults so reset() and computeGain()
    // are fully defined before the first control() (a fault reset on the first block must not read garbage).
    void prepare (double sampleRate, int maxBlock);
    void reset() noexcept;                       // keeps the last control() values; clears state, gain (-> rangeGain),
                                                 // counters and followers; no click because the base fades around it

    // Called once per 16-sample control block with the base's smoothed values.
    // Index constants come from EffectInfo::params order (section 4.4).
    void control (const float* v) noexcept;

    // Writes n gain values (linear, 0..1) from the undelayed key; does not touch the audio.
    void computeGain (const float* keyL, const float* keyR, float* gainOut, int n) noexcept;

    // Lookahead in samples for this sample rate (integer, constant for the life of prepare()).
    static int lookaheadSamples (double sampleRate) noexcept;

    bool  isOpen()        const noexcept;        // state == Open || Hold (for the matcher)
    float gainReductionDb() const noexcept;      // plain float, updated once per control block (the chain publishes it)
    int   stateIndex()    const noexcept;        // 0 closed, 1 attack, 2 open, 3 hold, 4 release (the chain maps to the lamp)

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
| `rmsWindow` | running mean of squares, fixed window | Window length fixed per sample rate (PROPOSED **2.5 ms**, half the lookahead, so RMS mode still pre-opens); ring sized in `prepare()`; the running sum is kept in **double** and clamped: `level = sqrt (max (sum, 0.0) / windowLen) + 1e-9`, so float drift can never produce a negative sum and a NaN key |
| `keyDb` | float | `20 log10 (max (level, 1e-6))`, i.e. floored at -120 dB; one log per sample (acceptable at C8's 20x real-time budget; a 16-sample decimation is the fallback, O3) |
| `state` | enum Closed, Attack, Open, Hold, Release | Section 4.5 |
| `holdCounter` | int samples | Set to `holdSamples + lookahead` on entering Hold from Open (`lookahead` alone after a false start); decremented first, released at `<= 0` (4.5, 12) |
| `gain` | float, current linear gain | Moved every sample by a constant dB-per-sample slew (one multiply) and clamped to `[rangeGain, 1]`; see 4.5 |
| `upStep`, `downStep` | float | `pow (10, (range / attackSamples) / 20)` and `pow (10, -(range / releaseSamples) / 20)`, recomputed once per control block; `range = 0` gives both steps 1 |
| `rangeGain` | float | `dbToGain (-range)`; the closed floor; `range = 0` means the gate is transparent (preset 7 "Level Only") |
| `grDb`, `stateIdx` | plain float / int | Written once per control block; **no atomics in the DSP header**. The chain copies them into `Meters::custom` (6.2) |

### 4.4 Parameters (ParamSpec rows, gate and detector portion; ids are proposals, see O4)

Common `mix`, `output`, `bypass` come first per the base (1.4). The gate's rows follow in this order.

| id | name | kind | min | max | default | skewCentre | smooth | unit | notes |
|---|---|---|---|---|---|---|---|---|---|
| `gate_thr` | Threshold | real | -80 | 0 | **-40 PROPOSED** | 0 | linear | dB | D6: measured floor + margin, never above breath level; -40 is the DD Gate reading |
| `gate_hyst` | Hysteresis | real | 0 | 12 | **4 PROPOSED** | 0 | linear | dB | close threshold = thr - hyst |
| `gate_attack` | Attack | real | 0.1 | 50 | **1 PROPOSED** | 5 | log | ms | time for the full traverse rangeGain -> 1 (exact, 4.5); attacks <= lookahead are fully pre-empted in Peak mode |
| `gate_hold` | Hold | real | **0** | 500 | **40 PROPOSED** | 50 | log | ms | min 0 like DD Gate (0.00 in two reference chains): the base's log smoother clamps at 1e-4 (`Params.h`), so log (0) never occurs (revision 3 reverts R2) |
| `gate_release` | Release | real | 5 | 2000 | **120 PROPOSED** | 200 | log | ms | time for the full traverse 1 -> rangeGain (exact, 4.5); release near minimum in the reference chains, but tails must survive |
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
- **Ramp law (all states):** the gain moves at a constant rate in dB, implemented as one multiply per sample and a
  clamp: rising `gain = min (gain * upStep, 1)`, falling `gain = max (gain * downStep, rangeGain)`. `upStep` carries
  `range / attackSamples` dB per sample, `downStep` carries `range / releaseSamples` dB per sample (4.3). So ATTACK
  is the time for the full traverse floor -> 0 dB and RELEASE the time for 0 dB -> floor, exactly, at every sample rate.
  A constant-dB slew is even over the whole range (a linear-domain one-pole would close fastest at the start and
  crawl at the end, the opposite of what hides room tone), and "reached the target" is an equality after the clamp,
  so no tolerance is needed. Each state has a direction and a step:
  Closed falls at `downStep` (so a `gate_range` change while closed glides at the release rate; if the new floor is
  above the current gain it rises at `upStep`), Attack / Open / Hold rise at `upStep`, Release falls at `downStep`.
- **Closed:** target `rangeGain`; move to Attack when `keyDb > openThr`.
- **Attack:** rise; on `keyDb < closeThr` go to Release (a false start never gets a hold); when `gain == 1` go to Open.
- **Open:** gain stays 1; on `keyDb < closeThr` go to Hold and set `holdCounter = holdSamples + lookahead` (revision 3:
  the key runs ahead of the audio, so the hold is counted in output time).
- **Hold:** gain stays 1. Sample-exact order, tested before decrementing:
  `if (keyDb > openThr) state = Open; else if (holdCounter <= 0) state = Release; else --holdCounter;`
  so Release starts exactly `holdSamples` samples after the key dropped below `closeThr`, and a hold of 1 ms (the
  minimum) releases after 1 ms. `<= 0`, never `== 0`, so the counter can never run away.
- **Release:** fall; `keyDb > openThr` goes to Attack (the rise continues from the current gain, no jump); when
  `gain == rangeGain` go to Closed.
- `reset()` sets state Closed, `gain = rangeGain`, clears followers and counters; it keeps the last `control()` values,
  which `prepare()` has already initialised from the defaults (4.2).

Timing is specified in samples computed from the sample rate at `prepare()`/`control()`, so the same preset times
behave identically at 44.1, 48 and 96 kHz (T0 cases, section 8.1).

### 4.6 Per-block algorithm (`computeGain`)

```
for i in 0..n-1:
    key   = 0.5f * (keyL[i] + keyR[i])
    key   = lowCut.process (key)                     // skipped when bypassed
    level = mode == Peak ? peakFollower.process (|key|) : rmsWindow.process (key*key)   // double sum, clamped (4.3)
    keyDb = 20 * log10 (max (level, 1e-6f))
    advance state (section 4.5)
    gain = rising (state) ? min (gain * upStep, 1.0f) : max (gain * downStep, rangeGain)   // one multiply + clamp
    gainOut[i] = gain
after the loop (once per 16-sample control block, not per sample):
    grDb     = 20 * log10 (gain)
    stateIdx = (int) state
```

No branch inside the loop allocates or calls a library function that may block. `std::log10` and `std::sqrt` are the
only transcendental calls in the loop; `std::pow` for the two steps runs once per control block in `control()`.

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
    static constexpr double lookaheadSeconds = 0.005;   // 5 ms PROPOSED, see O6
    static int latency (double sampleRate) noexcept     // floor (sr * 0.005 + 0.5), clamped >= 1
    {
        return std::max (1, (int) (sampleRate * lookaheadSeconds + 0.5));
    }
    // 44.1 kHz -> 221 (220.5 rounds up), 48 kHz -> 240, 96 kHz -> 480. The Python side uses the same rule,
    // int (sr * 0.005 + 0.5), never round(), whose half-to-even would give 220 at 44.1 kHz.

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

    void control (const float* v) noexcept override
    {
        gate.control (v); level.control (v);
        // Meters live in the base (handoff 3.6); the DSP headers hold plain values, the chain publishes them.
        meters.custom[0].store (lampValue (gate.stateIndex()), std::memory_order_relaxed);   // 0 closed, 1 open, 0.5 hold
        meters.custom[1].store (gate.gainReductionDb(), std::memory_order_relaxed);
        meters.custom[2].store (level.gainDb(), std::memory_order_relaxed);
    }

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

Written by `X11Chain::control()` once per control block with `store (relaxed)` (handoff 3.6); the editor reads at
30 Hz. The DSP headers expose plain values and own no atomics.

| slot | source | value | editor use |
|---|---|---|---|
| 0 | `VocalGate::stateIndex()` | 1 Open or Attack, 0.5 Hold or Release, 0 Closed (PROPOSED, O10) | gate-state lamp in GATE // ШЛЮЗ |
| 1 | `VocalGate::gainReductionDb()` | dB, <= 0 | gain-reduction needle |
| 2 | `LevelMatcher::gainDb()` | dB | level-match needle (out of scope here) |

---

## 7. Cross-module contracts

### 7.1 Latency

`Chain::latency (sr)` = `TransientAlign::latency (sr)` = `VocalGate::lookaheadSamples (sr)`. One function, three
callers. Reported once in `prepareToPlay` via `setLatencySamples`; the base's `dryLine` uses the same value.

### 7.2 Gain timing relative to the detector

For a step in the key at sample *t* in Peak mode: gain begins rising at *t* and equals 1 at *t + attackSamples*
(the slew traverses the full range in exactly that time); the audio step appears at the output at *t + LOOKAHEAD*.
With default values (attack 1 ms, lookahead 5 ms PROPOSED) the gate is fully open 4 ms before the consonant. In RMS
mode the key itself takes up to `windowSamples` (2.5 ms PROPOSED) to cross the threshold, leaving 1.5 ms. `selfTest`
case G7 asserts the Peak figure to the sample and the RMS figure within the window length.

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
| G2 open | step to -20 dBFS sine from Closed (gain starts at rangeGain) | state reaches Open; gain == 1 exactly `attackSamples` after the key crosses openThr | exact sample |
| G3 hold | burst then silence, hold 1 ms and 40 ms | Release starts exactly `holdSamples` after key < closeThr | exact sample |
| G3b hold minimum | as G3 with hold at its 1 ms minimum | gate reaches Closed (never sticks in Hold) | exact |
| G4 release | as G3 | gain == rangeGain exactly `releaseSamples` after Release starts; then Closed | exact sample |
| G5 hysteresis | sine held at thr - hyst/2 after opening | stays Open/Hold, never Release | exact |
| G6 chatter | key toggling ±1 dB around thr at 200 Hz | state changes <= 2 per 100 ms with hyst = 4 dB | count |
| G7 pre-open | impulse in key at *t*; the test composes `computeGain`, `TransientAlign::process` and the multiply itself (no `X11Chain`, which lives in `LabX11.cpp`) | Peak mode: output impulse at *t + LOOKAHEAD* carries gain == 1 | exact sample |
| G7b pre-open, RMS | as G7 with `det_mode = RMS` | output impulse carries gain == 1 | within `windowSamples` |
| G8 range 0 | range = 0 dB | gainOut all 1.0 regardless of state | exact |
| G8b range change closed | range 24 -> 48 dB while Closed | gain falls at the release rate to the new floor, state stays Closed | exact sample |
| G8c RMS silence | 10 s signal, then silence, RMS mode | key level >= 0 (never NaN), state reaches Closed | finite |
| G9 block equivalence | same 2 s signal, blocks 1 vs 512 | identical gain streams | 1e-7 |
| G10 rate equivalence | same ms settings at 44.1 / 48 / 96 kHz | state transition times in ms agree | ±0.05 ms |
| G11 mode switch | Peak -> RMS mid-signal | no NaN, gain continuous (no jump > 0.1) | — |
| G12 denormal / finite | 1e-30 input for 10 s | no NaN/inf; followers at floor | finite |
| G13 determinism | same input twice after reset | bit-identical gain streams | exact |
| A1 latency integer | each sample rate | `latency (sr)` == `(int) (sr * 0.005 + 0.5)`: 221 / 240 / 480 at 44.1 / 48 / 96 kHz | exact |
| A2 impulse offset | impulse at 0 through `process` | appears at `latency (sr)` and nowhere else | exact |
| A3 block independence | 3 s noise, blocks 1/7/64/3000/variable | output identical to block 512 | 1e-7 |
| A4 reset | fill, reset, process silence | output all zero | exact |

### 8.2 Harness / Python tests touching these modules (T1-T6 of the handoff's T1-T12 budget)

`labtest.fixture()` (a copy) builds sine, noise and impulse WAVs only. The burst-with-tail, click-train and
gated-burst signals below are generated by `x11_tests.py` itself with numpy, written to `test/out/`, and rendered with
`--in`; every assertion is a numpy analysis of the rendered WAV (the handoff's T-test shape, 1.9).

| test | signal | harness flags | assertion |
|---|---|---|---|
| T1 floor removal | `fixture()` -55 dBFS noise, 3 s | defaults | output RMS <= -55 - range + 1 dB |
| T2 tail survival | sine burst with 300 ms exponential tail (numpy) | defaults | tail energy within 1 dB of input until it crosses closeThr |
| T3 onset integrity | click train at -12 dBFS, 20 ms apart (numpy) | defaults | first-sample amplitude of each click within 0.5 dB of input (lookahead works) |
| T4 attack/hold/release | gated burst pattern (numpy) | `--set gate_hold=100 --set gate_release=200` | measured hold/release from envelope within 5 % |
| T5 impulse offset in the render | `fixture()` impulse | `--list-params` for the reported latency, at 44.1/48/96 kHz | impulse in the rendered WAV sits exactly at the reported latency, which equals `(int) (sr * 0.005 + 0.5)`; complements C2's `--latency-check` |
| T6 parameter ramp | `fixture()` noise, `--ramp gate_thr -80..0` over 10 s | — | no fault, no sample jump > 0.5 at any 16-sample boundary |

C1 (null at bypass) and C2 are the direct guarantees of section 5; C6 must list `gate` and `level`; C9 must find
every panel title of handoff 1.7, among them GATE // ШЛЮЗ and DETECTOR // ДЕТЕКТОР (D9 names).

---

## 9. Proposed values and open points (confirm against the "LAB X-11 vocal plugin plan" chat)

| # | item | proposed here | handoff status | why it matters |
|---|---|---|---|---|
| O1 | File split | `Gate.h` + `Align.h` | tree names `Gate.h` only | testability of latency vs. one file fewer |
| O2 | Detector key | **built:** low cut per channel, then max (|L|, |R|) (Peak) or the mean square of both (RMS) | unspecified | a mono sum can cancel on out-of-phase material; the louder channel cannot |
| O3 | dB conversion rate | per sample | unspecified | per-16-sample decimation if C8 (20x real time) is tight |
| O4 | Parameter ids | `gate_*`, `det_*` as in 4.4 | section 3 PENDING | append-only once installed (1.4) |
| O5 | Ramp domain | constant dB-per-sample slew (one multiply + clamp), times exact | PENDING (DSP panel) | even in dB over the whole range; a linear one-pole was rejected by review (section 11) |
| O6 | Lookahead length | **5 ms** fixed, `(int) (sr * 0.005 + 0.5)` = 221 / 240 / 480 | D4 PENDING ms | <= 10 ms keeps FL compensation trivial; longer buys nothing for consonants |
| O7 | Default threshold | -40 dB | D6 PENDING dBFS (floor + margin) | from the DD Gate reading; stem measurements may lower it |
| O8 | Hysteresis default | 4 dB | PENDING (DSP panel) | too small chatters, too large delays close |
| O9 | RMS window | 2.5 ms fixed (half the lookahead) | unspecified | keeps RMS mode pre-opening; fixed per sample rate so no run-time resize |
| O10 | Lamp values | 1 open/attack, 0.5 hold/release, 0 closed | unspecified | purely visual |
| O11 | Hold minimum | **built: 0 ms** | handoff range unspecified | the base's log smoother clamps at 1e-4, so 0 is safe and matches DD Gate |

Everything else in sections 4-8 follows directly from the handoff text and needs no confirmation.

---

## 10. Delivery checklist for milestone M1 (Gate) per handoff section 6

- [ ] `core/dsp/Gate.h` with `VocalGate`, `selfTest()` cases G1-G13 (with G3b, G7b, G8b, G8c)
- [ ] `core/dsp/Align.h` with `TransientAlign`, `selfTest()` cases A1-A4 (or folded into `Gate.h` per O1)
- [ ] `EffectInfo::params` rows of 4.4 after the common three; `stages = { "gate", "level" }`
- [ ] `X11Chain` as in section 6; `latency()` from `TransientAlign`; meters written by the chain
- [ ] `test/harness_main.cpp` `--selftest` extended to Gate and Align (Leveler joins in M2)
- [ ] `test/python/x11_tests.py` T0 and T1-T6 of 8.2
- [ ] C1, C2 (non-zero latency) green; F-B in FL. C4-C6 and C8 belong to M2 per handoff section 6
- [ ] `docs/DESIGN.md` records the confirmed values of section 9 and any change from this spec

---

## 11. Review record (revision 2)

Three independent reviewers (real-time/JUCE mechanics, vocal DSP practice, X-16 conformance) read this spec against
the handoff; each finding was then attacked by two refuters. Findings that both refuters upheld, and what changed:

| # | finding | change made |
|---|---|---|
| R1 | Hold counter order undefined at a zero hold: a decrement-then-`== 0` reading sticks the gate open | 4.5 states the sample-exact order and uses `<= 0`; G3b added |
| R2 | `gate_hold` min 0 with `Smooth::log`: the base's log smoother cannot take log (0) | min raised to 1 ms (O11) |
| R3 | RMS running sum in float can go negative at silence, `sqrt` gives NaN, state machine freezes silently | double accumulator, clamp, 1e-9 floor; G8c added |
| R4 | "10 %-90 % equals the parameter" and `onePoleCoef` disagree by a factor 2.2; "attack <= lookahead is fully pre-empted" was only true for attack <= lookahead / 2 | ramp replaced by a constant-dB slew whose traverse time is exactly the parameter; sections 3, 4.4, 4.5, 7.2, G2, G4 rewritten |
| R5 | No coefficient defined for Closed / Open / Hold; `reset()` could read `rangeGain` before the first `control()` | every state has a direction and step (4.5); `prepare()` initialises all control-derived members from the defaults (4.2); G8b added |
| R6 | Linear-domain one-pole "closes fast at the end" is backwards: it is fastest at the start in dB | same fix as R4; O5 updated |
| R7 | G4 could not pass against the stated ramp definition | G4 is now exact under the slew law |
| R8 | DSP header owned atomics with no path into `Meters::custom`; Hold lamp value inconsistent (1 vs 0.5) | atomics removed from the header; `X11Chain::control()` publishes slots 0-2 (6.2); lamp values fixed (O10) |
| R9 | Pre-open guarantee and G7 stated for both detector modes but only true for Peak (RMS window was 10 ms, twice the lookahead) | window shortened to 2.5 ms (O9); Peak/RMS distinguished in 3, 7.2; G7b added |
| R10 | M1 checklist claimed C5/C6; handoff puts them in M2; C9 note named two of five panel titles | section 10 and 8.2 corrected |

Contested findings (one refuter each way), settled anyway because the fix was one line: the latency rule is now
written as `(int) (sr * 0.005 + 0.5)` with the three resulting values (5.2, A1, T5), G7 composes the modules inside
the test rather than calling `X11Chain`, and 8.2 says which signals `x11_tests.py` generates itself.

---

## 12. Implementation record (revision 3)

Built on the real LAB X-16 base (`core/host/EffectProcessor.h`), which differs from what revisions 1-2 assumed in three
places: the chain is called with at most 16 samples (`prepare (sr, controlBlock)`), guard stages are indices
(`guard (0, ...)`, `guard (1, ...)`), and meters are written in the chain's `meter (Meters&)` hook after each host
block, so the code sketch of section 6 is superseded by `plugins/x11/LabX11.cpp`. A second review (three lenses, two
refuters per finding, every finding reproduced with the built harness or a compiled probe) found the defects below in
the first build; all are fixed and each has a test that fails without the fix (a mutation run kills 16 of 16 mutants).

| # | defect in the first build | change |
|---|---|---|
| I1 | **Preset click.** The base's preset fade-in (5 ms) equals the lookahead; `flush()` defaulted to `reset()`, which emptied the alignment ring, so the wet path was silent for the whole fade-in and stepped back in at full level (a 0.6 step on a -3 dBFS tone) | `X11Chain::flush()` keeps the ring and the gate state and resets only the matcher; `reset()` stays the full clear for faults and `prepareToPlay`. Section 5.3's "emptying the ring is inaudible" was wrong. Test T13 |
| I2 | **Hold shortened by the lookahead.** The key runs 5 ms ahead of the audio, so Release started HOLD - 5 ms after the audio fell below close; HOLD 0-5 ms faded word tails early | Hold is entered with `holdSamples + lookahead`; a false start (the key falls below close during Attack) goes through Hold for the lookahead alone, so the audio that triggered it still passes; Hold returns to Attack, not Open, when the gain is not yet up. G3, G16 |
| I3 | Ramp not sample-exact in float (one sample late at the defaults, up to 0.6 % on long, shallow releases) | the gain, floor and steps are kept in double and snapped within half a step of their end. G2 and G4 are exact at 44.1, 48 and 96 kHz |
| I4 | A RANGE glide restarted on every `set()`, so with control blocks under 16 samples the floor never landed and RANGE 0 left the gate in Attack (no voice for the matcher) | the glide restarts only when RANGE changes. G9, G9b |
| I5 | After `prepareToPlay` the floor started at the default 24 dB and glided to RANGE at the release rate | a `fresh` flag: the first `set()` after `reset()` places the floor directly. G8 |
| I6 | LOW CUT switched back on reused stale filter state, which opened the gate on silence | the filters are cleared when LOW CUT turns on. G17b |
| I7 | HOLD turned down during a hold waited out the old hold | `set()` shortens a running hold. G20 |
| I8 | Self-tests left most of the state machine and the detector unchecked | G14 retrigger, G15 short dip, G16 false start, G17 low cut, G18 key calibration, G19 detector release, G20 hold change, G8c floor up, G9b small blocks; G7 now compares the delayed, gated audio sample by sample |

Unchanged decisions confirmed by the build: 5 ms lookahead (221 / 240 / 480 samples), constant-dB slew, hysteresis
4 dB, peak detector by default, 2.5 ms RMS window, the lamp's three values.
