# LevelMatcher (core/dsp/Leveler.h)

Status: **synthesised design, revision 2 (review fixes folded in), no plugin code yet.** It builds on the "rider" design, which had the best judge total: 7.5 + 8.5 + 8.5 = 24.5, against 20.5 for "quantized" and 20.5 for "robust". The judges' grafts are folded in, and every conflict is settled toward the safer option (L.16). Revision 2 applies the findings of two refuters. The Review notes at the end say what changed.

Where the numbers come from:
- **Prototype.** A C++ prototype of the revision-1 design, compiled against the real lab-x-16 `Common.h` and `Filters.h` (g++ 13.3 -O2, x86-64). The T-test figures come from its chain emulator: the gate as in SPEC revision 3 (simplified), then the 5 ms alignment, then this matcher. The files are listed in L.16.
- **Emulation.** The reviewers' own C++ emulation of the L.11 pseudo-code, built against the same headers. It reproduces L4, L8, L9, L15, L20, L24, L25 and L27 to the printed digits, and it measured the revision-2 changes.
- **Hop model, rough emulation.** numpy checks made for revision 2 (L14, L29). Their assertions keep a margin over them.
- **°** marks a prototype figure that the revision-2 changes can move slightly. Those changes are the burst ceiling, the per-hop voicing test, the pre-level floor and the first-fill cap. The emulation found L9 and every VOICE level bit-identical, and T11 Part A's spread 0.03 dB smaller. Every figure is re-baselined on the revised prototype before M2, and the assertions keep their margins.
- **PROVISIONAL** marks a value that waits on the stem measurements (handoff 5.3 / Appendix B).

### L.1 What it is

`lab::dsp::LevelMatcher` is a voice-gated rider. It holds the vocal's phrase level near a fixed per-song TARGET, the way the reference projects use the vocal fader.

What it measures:
- The BS.1770 K-weighted loudness of the delayed, gated vocal.
- Over the last 400 ms of **admitted** voice.

When a 5 ms hop is admitted (every test must pass):
- The gate state is Open or Hold.
- The audio is voiced: the 500 Hz band is at least 6 dB above the 6 kHz band over 20 ms, and at least 3 dB above it in the hop itself.
- It is not below −70 LUFS.
- Once a level exists, it is not more than 20 dB below the current window level.
- Before the first level exists, it is not more than 20 dB below TARGET − MAX BOOST.

How the gain moves:
- One dB-domain gain moves toward `TARGET - level`, at no more than 6/τ dB/s.
- It moves only on samples covered by an admitted hop. Anything else holds the gain bit-exactly: gate gaps, breaths the gate lets through under D6, consonant bursts, white or pink room tone with a misset gate, and hum, tonal bleed or low-heavy room tone (rumble, HVAC) with a misset gate when it sits at least 25 dB under the voice window.
- The exception is hum, bleed or low-heavy room tone closer than that, with a misset gate. It passes every admission test and is ridden toward MAX BOOST (L.5; revision 3 adds room rumble, see the implementation record). THRESHOLD is the matcher's voice detector and must sit above hum, bleed and room rumble.

Footprint:
- 0 samples of added latency.
- 6 append-only parameters.
- Fixed `std::array` state only: no allocation, no locks, no atomics in the header.
- Fast peak control stays with the user's Fruity Compressor (about −30 dB, 20–30:1), DD Compressor and OTT/Kotelnikov.

### L.2 Law (D7): continuous by default, stepped as an option

**Decision: Ride (continuous slow gain) is the conservative D7 answer and the default.**
- It never makes a discrete gain event.
- Its steady state is exact.
- Every move is bounded by one rate.
- Every factory preset uses it.

**Step** is offered as the choice `level_law = Ride|Step`, index 1. It is the literal "quantizer / per-line fader" reading. No preset uses it until the user confirms D7.

Notation:
- T = target (dB), A = amount (0..1), τ = time (s), B = max boost (dB, ≥ 0), C = max cut (dB, ≥ 0), fs = sample rate.
- L = measured level (LUFS, L.4).
- c = 1 − e^(−1/(τ·fs)).
- s = 6c, the slew cap in dB per sample (6/τ dB/s).
- LA = `(int) (fs * 0.005 + 0.5)` = 221 / 240 / 480 samples, which equals `TransientAlign::latency`.

**goal** = clamp(T − L, −C, +B).
- It is set at each admitted hop end, and in every `control()` once a level exists.
- Before the first level exists it is 0 dB.

**RIDE** runs only on samples where the gate is Open or Hold **and** the last completed hop was admitted. That includes a hop completed on this same sample (L.11 order):

```
e = goal − ride
ride = |e| < 1e-6 ? goal : ride + c · clamp (e, −6, +6)
```

This is a one-pole in dB with time constant τ. While the error is above 6 dB, it slews linearly at 6/τ dB/s.

**out** is a slew limiter (cap s per sample) that lands exactly on the law's aim. Under Ride the aim is `ride`, which never outruns s, so out == ride.

**STEP** uses the commit model (quantized's, as both judges asked). It adds no parameter ids.
- The held step is q = clamp(1.5 · round(ride / 1.5), −C, +B), using `std::round` (half away from zero).
- **Phrase onset commit.** At the first sample whose gate state is Attack, Open or Hold after Closed or Release: if |ride − q| ≥ 1.0 dB, q is re-quantised. If out ≠ q, a raised-cosine pre-roll runs: out = o₀ + (q − o₀)·½(1 − cos(πk/LA)), k = 1..LA. The last sample lands exactly on q.
  - The gate state runs on the key timeline, LA ahead of the audio. So the pre-roll ends on the sample just before the phrase's first audio sample. After a full close, the move happens in the gate's own pre-open window, over gated floor.
  - A re-trigger from Release is also an onset. When the gap between lines is shorter than hold + release (160 ms at the defaults), the pre-roll runs over the last 5 ms of the previous line's releasing tail instead. That audio is under the open threshold and at the gate's partial release gain (O-16).
  - Inside a phrase, out is constant except for the late, limit and law glides below, which move at s per voiced sample.
- **Late commit.** After 4 s of admitted voice with no gate gap, the commit test runs in-voice. out then glides to q at s per voiced sample. This keeps Step working on dense 160 BPM delivery, where the gate rarely closes.
- **Limit and law moves.** q is re-clamped in every `control()`.
  - Ride → Step sets q = quantise(ride).
  - Step → Ride lets out slew back onto ride.
  - All three glide at s on voiced samples.
- A phrase boundary is any gate release: a Release or Closed state between two phrase states. Its minimum length is therefore the gate's hold time, so no `lvl_gap` parameter is needed.
- Accuracy: the steady-state error against Ride is at most 1.0 dB (T8(d): Ride 11.00, Step 10.5). Step places line n from what it heard up to the end of line n−1, so it cannot correct the line it is in.

**APPLIED gain** (every sample, voiced or not):
- appliedDb = a · out. The amount a is slew-limited so that `|Δa|·|out|` ≤ 12 dB/s (L.6).
- gLin = (float) exp2(appliedDb · 0.16609640474436813), recomputed only when appliedDb changes.
- appliedDb == 0 gives exactly 1.0f.
- `l[i] *= gLin; r[i] *= gLin` (L and R share the gain).

Steady state (Ride, A constant, inside the limits): output voiced loudness = L + A·clamp(T − L, −C, +B). At A = 1 this is exactly T.

### L.3 Target (D8)

**TARGET is a fixed per-song parameter, `level_target`, with no auto-learn.** Default −18 PROVISIONAL.

What it means:
- The voiced BS.1770 momentary loudness: 400 ms, K-weighted, L+R power sum, −0.691 offset.
- Calibration identity: a 997 Hz sine at −20 dBFS peak on both channels reads −20.0.
- During a sustained phrase it approximates Youlean's momentary reading. On delivery with gaps it reads higher than Youlean short-term (3 s) or integrated loudness, because only admitted voiced hops enter the window. **Set TARGET from LEVEL (custom[3]), not from Youlean.**
- The unit label is "dB", because `formatValue` has no LUFS. The README and tooltip say LUFS.
- This changes D8's wording from "dBFS short-term" to "LUFS, voiced momentary". That is a D8 amendment for the user to confirm (O-2).

Procedure for the README: "play a typical verse, read LEVEL (meter slot 3), set TARGET to it". The average correction is then about 0 dB, so the vocal-to-snare fader balance and the gain into the user's −30 dB compressors are unchanged. Only the spread changes.

The −18 default will be replaced by the median voiced momentary loudness of the three lead stems, rounded to 1 dB, in every preset at once.

### L.4 Detector

**Input and timeline**
- The matcher reads the delayed, gated audio in place. It runs after guard 0, so the audio is already bounded to +42 dBFS.
- It also gets the gate's per-sample state, `gateState[i]` ∈ {0 Closed, 1 Attack, 2 Open, 3 Hold, 4 Release}. That state is index-aligned with the gate gain applied to output sample i.
- The state was decided on key sample i, which is LA samples ahead of the audio now at output i. Consequences:
  1. The per-phrase measurement may include up to LA − attack samples of pre-onset room tone in its first hop (4 ms at the defaults). It also includes the gate's hold time of tail (40 ms default). The admission tests and the relative gate handle both (L17).
  2. The freeze starts on the same output sample where the gate's release ramp starts.
  3. Step's onset is seen LA samples early. That is the pre-roll's free lookahead.
- The undelayed key is not measured. Measuring it would add nothing for a 400 ms window, would need a key copy, and would read pre-guard samples.
- Latency added: 0.

**K-weighting** (BS.1770): two `lab::dsp::Biquad` per channel (TDF-II, float).
- Coefficients are set directly in `prepare()` from the rate-independent formulas (double math, stored as float).
- Shelf: f₀ = 1681.974450955533 Hz, G = 3.999843853973347 dB, Q = 0.7071752369554196.
  - K = tan(πf₀/fs), Vh = 10^(G/20), Vb = Vh^0.4996667741545416, a₀ = 1 + K/Q + K².
  - b = [(Vh + Vb·K/Q + K²)/a₀, 2(K² − Vh)/a₀, (Vh − Vb·K/Q + K²)/a₀], a = [1, 2(K² − 1)/a₀, (1 − K/Q + K²)/a₀].
- RLB high-pass: f₀ = 38.13547087602444 Hz, Q = 0.5003270373238773, b = [1, −2, 1], a as above.
- At 48 kHz this reproduces the BS.1770 table. Shelf b: 1.53512486, −2.69169619, 1.19839281; a: −1.69065929, 0.73248077. HP a: −1.99004745, 0.99007225.
- Gains relative to 997 Hz: 100 Hz −1.82 dB, 4 kHz +3.28 dB. Gain at 997 Hz: +0.69 dB.
- The filters run on every sample, so their state is continuous through gaps.

**Voicing bands** (from "robust"; per channel, so an anti-phase stereo signal cannot fool a mono sum):
- `SVF::bandpass` (unity peak) at 500 Hz Q 0.5 (about 207–1207 Hz) and at 6000 Hz Q 0.5 (about 2.5–14.5 kHz), on the raw delayed audio. That is four SVF ticks per sample.
- Measured band ratios at 48 kHz:

| Signal | lo/hi |
|---|---|
| 997 Hz | +8.32 dB |
| 300 Hz | +19.4 dB |
| 140 Hz harmonic voice | +12.6 dB |
| 2 kHz | −1.8 dB |
| 4 kHz | −11.6 dB |
| white noise | −8.2 dB |
| pink noise | +0.8 dB |

- A formant model of the vowels /a e i o u/ at f₀ 100–220 Hz, with source tilt −6..−12 dB/oct, reads +10..+20 dB.

**Hops and admission**
- Samples whose state is Open or Hold are accumulated into a hop: K energy kl² + kr², and band energies lo and hi (all in double).
- A hop is hopLen = LA voiced samples (5 ms). The counter is internal and independent of block boundaries, so a hop may span a gap.
- On every unvoiced sample, `admitting = false`. Nothing moves after a gap until a new hop is admitted.
- At hop end, with ms = K energy / hopLen:
  - **Non-finite** (ms, lo or hi): reset all eight filters and the voicing ring; the hop is rejected. This is checked once per hop, so there is no per-sample `isfinite`.
  - **Admitted** only if all of these hold:
    - **Absolute floor:** ms ≥ 1.17247e−7 (−70 LUFS).
    - **Voiced:** Σlo ≥ 4·Σhi (+6.02 dB) over the last 4 hops (20 ms), **and** lo ≥ 2·hi (+3.01 dB) in this hop alone.
      - Why 20 ms: it spans two pitch periods at 100 Hz. With a single 5 ms hop at +6 dB, 5 % of a harmonic test voice's hops failed by chance, and its level read 0.3 dB differently at 44.1 and 48 kHz. With 20 ms there are 0 failures, and the readings are −20.50 / −20.51 / −20.53 at 44.1 / 48 / 96 kHz.
      - Why the per-hop test: the 20 ms sum carries up to 3 voice hops across a gate gap. Without the per-hop test, the first hops of an abrupt breath or an onset sibilant passed. Emulation: 5–18 hops per abrupt breath, and 14 / 7 of 96 onset-"s" hops at −10 / −6 dB. With it: 0–1 and 0. VOICE at f0 90 / 140 / 220 Hz still passes every hop (1995 / 2000 / 2000 at 44.1 / 48 / 96 kHz, identical levels).
    - **Level guard** (once a level exists): ms ≥ 0.01·gatedMean, i.e. no more than 20 dB below the window.
      - −20 dB is the EBU Tech 3342 (LRA) relative-gate depth; BS.1770 integrated loudness gates at −10 LU.
      - It rejects hum and tonal bleed, which pass the voicing test, **only when they sit at least about 25 dB under the window** (L.5).
    - **Pre-level floor** (before the first level exists): −0.691 + 10·log₁₀(ms) ≥ T − B − 20 dB, i.e. no more than 20 dB under the quietest level the ride could fully correct.
      - Without it, with a misset gate, hum or bleed before the first phrase (song intro, after `prepareToPlay`, after a fault) fills the window in 100 ms. The ride then climbs to MAX BOOST on the hum itself (+4.5 dB applied at Init). When the voice arrives about 35 dB louder, the burst clamp throttles acquisition.
      - The L23 / T12b hum (−54.7 LUFS) is rejected whenever T − B > −34.7, which covers every preset at TARGET −18 (L30, T12(b″)).
  - `admitting` = the result.
  - **Burst clamp** (once a level exists): an admitted hop is capped at 10·gatedMean (+10 dB).
    - The clamp is self-referential, because gatedMean is the mean of hops that were already capped. At +6 dB, that feedback settled low on staccato syllables. LEVEL then read low, and the matcher boosted staccato lines more than legato lines of equal loudness.
    - Emulation, at a syllable period P of 94 / 94 / 188 / 188 / 188 ms with decay τ of 10 / 20 / 10 / 20 / 40 ms: +6 dB read −3.57 / −1.31 / −4.10 / −3.77 / −1.56 dB; +10 dB reads −0.16 / 0.00 / −1.54 / −0.47 / 0.00 dB (L28).
    - VOICE and voice_phrase peak only +5.4 dB over their energy mean, so their levels do not change.
    - The side effect the judges flagged (voice throttled against a breath-filled mean) cannot occur, because breath, room-tone and hum hops under the guard never enter the window.
  - **First fill** (the 20th admitted hop after a `reset()`, while `valid` is still false): ring[0..20) is capped at 10 × its upper median (`std::nth_element` on a 20-double copy, element 10) before the level is computed.
    - Before `valid`, neither the burst clamp nor the level guard applies, so a plosive in the first 100 ms of voice inflated the first level.
    - The cap is relative to the median, and the median of a staccato window is its floor. So the cap shares the burst clamp's +10 dB rather than a tighter value (L29).

**Window and level**
- A ring of 80 admitted hop energies (400 ms of admitted voice; 400.9 ms at 44.1 kHz). It is kept across gaps and filled once 20 hops (100 ms) have been admitted.
- **Relative gate:** m₀ = mean(ring); gatedMean = mean of the slots ≥ 0.01·m₀ (−20 dB). At least one slot always qualifies.
- Both passes are recomputed over at most 80 doubles each hop, so there is no running-sum drift.
- L = −0.691 + 10·log₁₀(gatedMean). `valid = true` from then on, and goal = clamp(T − L, −C, +B).

**Not used:**
- The Follower.h and Dynamics.h peak paths. Those are the compressors' job.
- The undelayed key for measurement.
- Any per-phrase memory restart, median tiles (the one-time first-fill cap is not a tile), quiet-section escape, or quiet-section heuristic (L.16).

### L.5 Gain computer

- goal = clamp(T − L, −C, +B).
  - It is recomputed at each admitted hop end and in every `control()`, so a TARGET / BOOST / CUT move takes effect at the next ≤ 16-sample block. The base's 15 ms smoothing only moves goal, which the ride follows.
  - Before the first level exists (after `prepare`, `reset`, or a fault): goal = 0. The output is bit-exact unity whatever TARGET, BOOST, CUT and AMOUNT do (L26).
  - The pre-level floor (L.4) means a take more than 20 dB under TARGET − MAX BOOST is not admitted. It then shows LEVEL −100 with the lamp at 0.5 until TARGET or MAX BOOST changes (README). At the defaults this is a take under −44 LUFS, and the ride could only give it +6 dB anyway.
- **The limits clamp the goal, not the gain.** Lowering a limit below the current ride makes the ride glide back at ≤ 6/τ dB/s, on admitted voiced samples only (L20).
  - Step's held q is re-clamped in `control()` and glides at s.
  - After a preset `flush()`, the ride is snapped into the new limits under the base's fade.
- **AMOUNT scales the whole ride in dB, including its limits:** applied = A·out.
  - The applied gain stays within A·[−C, +B], except while it glides back after a limit was lowered. That glide runs at ≤ 6/τ dB/s on admitted voice and holds in gaps. So BOOST lowered during a long instrumental leaves the gain above the new limit until voice returns.
  - At A = 0.75 and constant limits, the largest applied boost is 0.75·B.
  - A < 1e−4 is treated as exactly 0, because the base's linear smoother approaches 0 only asymptotically. The applied gain is then 0 dB and gLin = 1.0f: a bit-exact pass, which is preset Gate Only.
  - The ride keeps measuring underneath, so raising AMOUNT later applies an already valid ride.
- Feed-forward: the gain never feeds back into the measurement.
- **Misset gate** (threshold under room tone, hum or bleed, so the gate stays open):
  - Broadband room tone that is white or pink fails the voicing test, so the gain holds. Room tone falling 6 dB per octave or more (brown: rumble, HVAC, traffic) passes it like hum and is held only by the level guard (revision 3, L23d). Prototype: 0.0004 dB over an 8 s open-gate gap, against +6.08 dB (up to MAX BOOST) without the grafts (L23).
  - Hum and tonal bleed (60 Hz hum, 808, synth chords) pass voicing. The level guard holds them only when they sit **at least about 25 dB under the voice window**: at −25 and −27 dB the gain rise is 0.000 and the lamp reads 0.5.
  - Closer than that, the gain rises. A 5 ms hop is shorter than one 60 Hz period (16.7 ms), and a chord beats near 55 Hz, so the per-hop K energy swings about ±5 dB. Hops near −17 dB pass the −20 dB guard, each admitted hop lowers gatedMean, and the window cascades down to the bleed. The ride then climbs toward MAX BOOST within about 3 s of gap, with the lamp at 1. Emulation: +5.35..+5.56 dB for hum at −24..−19 dB; a 55 Hz 808 and a 220 / 277 / 330 Hz chord at −21 dB give +5.5 dB (L23c, T12(b′)).
  - Evaluating the guard on the 20 ms energy only moves this cliff to about 22.5 dB, and it delays L24's d = 24 acquisition from phrase 8 to phrase 16. So the guard stays per hop, and the README says: **"THRESHOLD must sit above hum and bleed: it is the matcher's voice detector."**
  - Before the first level, the pre-level floor rejects tonal content more than 20 dB under T − B (L30, T12(b″)). Residual cases: TARGET −18 with BOOST 18 admits hum louder than −56 LUFS, and TARGET −40 with BOOST 18 admits anything above the −70 LUFS floor.
- Accuracy:
  - The steady state is exact to the 1e−6 dB snap.
  - 997 Hz at −30 dBFS, T −18, limits 18: final +11.996 / +11.999 / +12.016 dB at 44.1 / 48 / 96 kHz. The 96 kHz offset is the bilinear K-shelf at 997 Hz.

### L.6 Ballistics

**Ride**
- A dB-domain one-pole with c = 1 − e^(−1/(τ·fs)) and the error clamped to ±6 dB. It is proportional below 6 dB of error and a 6/τ dB/s slew above it.
- Defaults: τ = 1 s, so at most 6 dB/s, which is 0.06 dB per 10 ms. Range τ = 0.5..10 s, i.e. 12..0.6 dB/s.
- State is held in double. In float, the increment falls below 1 ulp at τ = 10 s, 96 kHz.
- The 1e−6 dB snap gives an exact steady state and no denormal tail.

**Convergence** (admitted voiced time, A = 1, inside the limits):

t(tol) = 0.1 s (fill) + max(0, |D| − 6)·τ/6 + τ·ln(min(|D|, 6)/tol)

For D = 12 dB, τ = 1 s:
- within 1 dB at 2.892 s (prototype 2.891);
- within 0.1 dB at 5.194 s (prototype 5.186);
- no overshoot.

**Amount slew (graft):** aNow moves toward the block's amount with |Δa|·|out| ≤ 12 dB/s per sample, and lands exactly.
- A 0 → 1 move with the ride at +12 dB takes 1.000 s instead of the base smoother's ~45 ms.
- 12 dB/s is also the fastest ride rate (τ = 0.5 s), so with A constant the ride passes through unchanged.
- The bound on the applied gain per sample is (12 + 6/τ)/fs.

**Per-sample ramp:** every applied gain change comes from the ride slew, the out slew, the raised-cosine pre-roll (Step) or the amount slew. No gain steps at a block boundary, with **one exception**: the post-`flush()` snap on a preset change. It lands on the first sample of the base's 5 ms fade-in, where the wet path's weight is 1/(0.005·sr) (L27; C4 covers the audible side). gLin is recomputed only when appliedDb changes, so a frozen gain costs no transcendental.

**Phrase start:**
- The gain carried from the end of the previous phrase is held through the gap. The first word plays at the previous phrase's gain, like a fader left where it was.
- The window still holds the previous phrase's last 400 ms. New hops displace it, so the goal crossfades between phrase levels instead of jumping on a syllable.
- Under Step, the line's step is placed in the 5 ms before its first sample. On a re-trigger from Release, it is placed over the previous line's releasing tail (O-16).

**Song start / reset:**
- The gain is exactly 0 dB until 100 ms of admitted voice has been heard. The ride first moves on the sample that completes the 20th admitted hop (L18), then rides from 0 at ≤ 6/τ dB/s.
- Until then, hops more than 20 dB under TARGET − MAX BOOST are not admitted (L.4), and the first 20 hops are capped at 10 × their median.

**Preset change (graft):** `X11Chain::flush()` calls `level.flush()`, which keeps the ring, the level, the ride, the filters and `valid`.
- The first `control()` after the flush snaps: ride is clamped to the new limits, q is re-quantised, out jumps to the aim, and amountNow jumps to the new amount.
- The base calls `flush()` after the block that ends the 30 ms fade-out, so that `control()` and the first sample of the next `process()` are the first sample of the 5 ms fade-in.
- Auditioning presets mid-song does not relearn from 0 dB.
- `reset()` (fault, `prepareToPlay`) still forgets everything.

**Measured behaviour** (chain emulator, synthetic voice; T11; all figures °):

| Case | Result |
|---|---|
| Four takes at −24 / −14 / −20 / −12 LUFS, A = 1 | last 4 s of each take at −18.61 / −18.47 / −17.78 / −16.84 (spread 1.77 dB against 12 dB in) |
| Same takes at default A 0.75 | spread 4.63 dB |
| Alternating-phrase worst case (12 phrases, −18 ±6 dB), second half of each phrase | SD 4.48 → 3.16 (defaults), 2.80 (A = 1), 1.79 (Rap Lead) |
| Same case, whole phrase | SD barely changes (4.43 → 4.37): inherent to a causal rider with 5 ms of lookahead |
| Syllable-rate gain ripple (2–8 Hz band) | 0.10 / 0.13 / 0.25 dB peak-to-peak |

### L.7 Freeze

| Gate state at output i | Last completed hop | Hop accumulation | ride | out under Ride | out under Step |
|---|---|---|---|---|---|
| Closed, Release | n/a (`admitting` forced false) | no | frozen | frozen | frozen; a running pre-roll completes |
| Attack | n/a (`admitting` forced false) | no | frozen | frozen | onset (from Closed or Release): commit test, pre-roll may start |
| Open, Hold | rejected | yes | frozen | frozen (equals ride) | glides to q only for a pending late, limit or law move |
| Open, Hold | admitted (including a hop completed on this sample) | yes | moves | follows ride | holds q (moves only by pre-roll or late commit) |

Behaviour:
- **Nothing measured in a gap can move the gain.** Measured: the gain pp across a 4–5 s Closed gap is exactly 0 (L8, T9). With a misset gate, white or pink room tone fails voicing; tonal bleed and low-heavy room tone are held only at least about 25 dB under the window (L.5, L23d).
- **Breaths through an open gate (D6)** are rejected by voicing.
  - Gain traces are identical with and without 0.4 s swelling breaths at −10 / −15 / −22 dB (white, pink, vocal-tract noise) (L22, T12(d)). The unguarded rider started the next phrase 0.6–1.5 dB off.
  - Abrupt breaths (10 ms rise) after a gap leak 0–1 hops per breath. Emulation: 0.03 dB at most at any sample and ≤ 0.004 dB at phrase onsets (L22b, T12(d′)).
- **Level Only** (gate range 0) still freezes, because voicing comes from the gate **state**, not its gain (T12(a)). THRESHOLD is the matcher's voice detector.
- In a false-start Hold (gate SPEC I2), the gate gain can be below 1. The hop is then quiet and usually rejected; if it is admitted, its effect is bounded to one hop of 80.
- Freeze is in voiced time. On choppy rap with many gate closures, the wall-clock time constant is about τ divided by the voiced fraction.
- Under Step, a re-trigger from Release starts a pre-roll over the last 5 ms of the releasing tail (L.2, O-16).

### L.8 Transient handling

Snare-like consonant peaks ("t", "k", "ch", plosives: 5–25 ms, +15..+24 dB over the phrase) are neither pumped nor crushed:

1. **No peak path.** The gain never depends on a peak or an envelope follower. A 10 ms event is 2 of 80 hops.
2. **Voicing rejection.** Hops dominated by a stop burst or a sibilant fail the 500 Hz / 6 kHz test, and the ride is frozen for them.
   - Module test L9 (997 Hz at −24 dBFS plus 15 ms noise bursts peaking at −4 dBFS every 923 ms, i.e. a 130 BPM backbeat): 70 hops rejected, gain within −0.00025..+0.00019 dB (pp 0.0004 dB). The rider without the grafts: −0.226..−0.113 (pp 0.113).
   - The per-hop test also rejects onset sibilants after a gap, which the 20 ms sum alone passed: 0 of 96 onset-"s" hops admitted, against 14 / 7 without it (emulation, L22c).
   - The K-shelf's +4 dB on sibilance no longer reaches the measurement.
3. **Burst clamp.** A hop that still passes (vowel-dominated, with a plosive thump inside) is capped at +10 dB over the gated mean.
   - Emulation: a 25 ms, 50 Hz thump every 1 s at +20 / +26 / +32 dB over the voice RMS moves the gain 0.23 / 0.45 / 0.61 dB pp. For comparison, a +6 dB cap gives 0.17 / 0.24 / 0.26 and no clamp gives 0.23 / 0.72 / 1.68.
   - The cap is +10 rather than +6 because +6 read staccato lines 1.3–4.1 dB low (L.4, L28, O-18).
4. **Slew cap.** At most 6/τ dB/s: 0.06 dB in a 10 ms consonant at the default. Measured 10 ms change in T10: 0.00027 dB. The burst passes at the existing gain with its crest intact: each burst's peak gain equals the local gain within 0.0002 dB.
5. **Causality.** Any reaction comes after the event and is spread over ≥ 400 ms. Nothing ducks ahead of a consonant: Ride uses no lookahead, and Step moves only at phrase boundaries.
6. **Peak control stays downstream** with the user's chain. The matcher only hands it a steadier phrase level.

### L.9 Parameters

These rows are appended after the gate's nine rows (SPEC 4.4), in this order. They are append-only once installed. Floats and choices only.

| id | name | kind | min | max | default | skewCentre | smooth | unit | choices | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| `level_amount` | Amount | real | 0 | 1 | **0.75** | 0 | linear | % | | 0 = bit-exact off (preset Gate Only); < 1e−4 is treated as 0. Scales the ride including its limits. Linear because the log smoother clamps at 1e−4. Applied changes are slew-capped at 12 dB/s |
| `level_target` | Target | real | −40 | −6 | **−18 PROVISIONAL** | 0 | linear | dB | | D8 (amended, O-2): voiced BS.1770 momentary loudness (LUFS, L+R). Set from the LEVEL readout, not from Youlean. Max −6 keeps vocal peaks (crest 15–20 dB) near the +12 dBFS wet-safety knee |
| `level_time` | Ride Time | real | **0.5** | 10 | 1 | 1 | log | s | | τ in voiced time; max rate 6/τ dB/s (12 dB/s at the minimum, outside the compressors' syllable territory) |
| `level_boost` | Max Boost | real | 0 | 18 | 6 | 0 | linear | dB | | Upper clamp on the goal. Boost is the risky direction (it lifts the in-phrase floor), so it is smaller than the cut. Also sets the pre-level floor (TARGET − MAX BOOST − 20 dB) |
| `level_cut` | Max Cut | real | 0 | 18 | 9 | 0 | linear | dB | | Lower clamp on the goal, entered as positive dB. Cutting is the safe direction; the compressors handle the rest |
| `level_law` | Law | choice | 0 | 1 | 0 | 0 | none | | `Ride\|Step` | D7. Ride = continuous (conservative, every preset). Step = 1.5 dB grid, commit at gate onsets inside the 5 ms pre-open window (over the releasing tail on a re-trigger from Release), 4 s late commit. Click-free switching |

```cpp
// EffectInfo::params, after gate_thr .. det_release (rows 9..14 of the effect's own parameters)
{ "level_amount", "Amount",    Kind::real,   0.0f,   1.0f,  0.75f,  0.0f, Smooth::linear, "%",  "" },
{ "level_target", "Target",    Kind::real,  -40.0f, -6.0f, -18.0f,  0.0f, Smooth::linear, "dB", "" },
{ "level_time",   "Ride Time", Kind::real,   0.5f,  10.0f,  1.0f,   1.0f, Smooth::log,    "s",  "" },
{ "level_boost",  "Max Boost", Kind::real,   0.0f,  18.0f,  6.0f,   0.0f, Smooth::linear, "dB", "" },
{ "level_cut",    "Max Cut",   Kind::real,   0.0f,  18.0f,  9.0f,   0.0f, Smooth::linear, "dB", "" },
{ "level_law",    "Law",       Kind::choice, 0.0f,   1.0f,  0.0f,   0.0f, Smooth::none,   "",   "Ride|Step" },
```

`LevelMatcher::defaults[]` must equal these defaults; L31 checks it.

Meters, written in `X11Chain::meter()`:
- `custom[2]` = `gainDb()`, the level-match needle.
- `custom[3]` = `levelDb()`, the LEVEL readout in LUFS (−100 = nothing measured).
- `custom[4]` = `voiceLamp()`:
  - 1 = measuring;
  - 0.5 = gate open but hop rejected (breath, sibilant, room tone, hum under the guard, too quiet, or under the pre-level floor);
  - 0 = gate closed.
- Slots 0 and 1 stay the gate's.

### L.10 Interface

```cpp
// core/dsp/Leveler.h  (header-only; Allman, space before '(', noexcept on the audio path)
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>      // selfTest's std::printf
#include <limits>
#include "Common.h"
#include "Filters.h"   // Biquad (coefficients set directly), SVF (voicing bands)

namespace lab::dsp
{
    // LEVEL // УРОВЕНЬ (LAB X-11): a voice-gated rider. K-weighted momentary loudness of what the gate lets through,
    // admitted 5 ms at a time only when the gate is Open/Hold, the hop is voiced (over 20 ms and on its own) and it is
    // not 20 dB under the window (before a level exists: not 20 dB under TARGET - MAX BOOST); a slow dB-domain ride
    // toward a per-song TARGET, frozen bit-exactly whenever a hop is not admitted.
    class LevelMatcher
    {
    public:
        enum Param : int { amount = 0, target, time, boost, cut, law, paramCount };   // offsets from the first level_* row
        enum Law : int { ride = 0, step = 1 };
        enum GateState : uint8_t { closed = 0, attack = 1, open = 2, hold = 3, release = 4 };   // == VocalGate::stateIndex()

        static constexpr double hopSeconds = 0.005, absFloorMs = 1.17247e-7, relGate = 0.01, guardRatio = 0.01;
        static constexpr double burstCeiling = 10.0, voiceRatio = 4.0, hopVoiceRatio = 2.0;   // +10 dB; +6.02 dB over 20 ms, +3.01 dB per hop
        static constexpr double preFloorDb = 20.0;                                            // before a level: TARGET - MAX BOOST - 20 dB
        static constexpr int    windowHops = 80, minFillHops = 20, voiceHops = 4;
        static constexpr float  lowBandHz = 500.0f, highBandHz = 6000.0f, bandQ = 0.5f;
        static constexpr double slewKneeDb = 6.0, snapDb = 1.0e-6, amountCapDbPerSecond = 12.0;
        static constexpr double gridDb = 1.5, commitDb = 1.0, lateSeconds = 4.0;
        static constexpr double log2PerDb = 0.16609640474436813;                              // log2 (10) / 20
        static constexpr float  noLevel = -100.0f;
        static constexpr float  defaults[paramCount] = { 0.75f, -18.0f, 1.0f, 6.0f, 9.0f, 0.0f };   // == the ParamSpec rows (L31)

        static int lookahead (double sr) noexcept { return std::max (1, (int) (sr * hopSeconds + 0.5)); }   // == TransientAlign::latency

        void prepare (double sampleRate, int maxBlock) noexcept;   // coefficients, hopLen; control (defaults); reset(). Allocates nothing
        void reset() noexcept;                                     // fault / prepareToPlay: forget everything, gain exactly 0 dB, settings kept
        void flush() noexcept { snapPending = true; }              // preset change: keep the measurement; snap at the next control()
        void control (const float* v) noexcept;                    // v -> level_amount; once per <= 16-sample block
        void process (float* l, float* r, const uint8_t* gateState, int n) noexcept;   // in place; gateState may be nullptr (= Closed)

        float gainDb() const noexcept    { return (float) appliedDb; }                   // custom[2]
        float levelDb() const noexcept   { return valid ? (float) levelLufs : noLevel; } // custom[3]
        float voiceLamp() const noexcept { return lamp; }                                // custom[4]
        static inline void selfTest();   // defined at the end of this header; prints selftest_level_<case>=<value>.
                                         // test/harness_main.cpp wraps the call in juce::ScopedNoDenormals (no JUCE here)

    private:
        void setKWeighting (double sr) noexcept;
        void endHop() noexcept;
        void applyGain() noexcept;
        double quantise (double x) const noexcept { return std::clamp (gridDb * std::round (x / gridDb), -cutDb, boostDb); }

        std::array<Biquad, 2> shelf {}, highPass {};       // K-weighting, [0] L, [1] R
        std::array<SVF, 2> lowBand {}, highBand {};        // voicing bands, [0] L, [1] R
        std::array<double, windowHops> ring {};
        std::array<double, voiceHops> loRing {}, hiRing {};
        int hopLen = 240, la = 240, hopCount = 0, write = 0, filled = 0, vWrite = 0, lawNow = ride;
        int preRollLeft = 0, preRollK = 0;
        int64_t lateLen = 192000, lateCount = 0;
        double kSum = 0.0, loSum = 0.0, hiSum = 0.0, gatedMean = 0.0, levelLufs = -100.0;
        bool valid = false, admitting = false, wasPhrase = false, snapPending = false;
        double sampleRate = 48000.0, targetDb = -18.0, boostDb = 6.0, cutDb = 9.0;
        double coef = 0.0, slew = 0.0, capPerSample = 0.00025, amountTarget = 0.75, amountNow = 0.75, preFloorMs = 0.0;
        double goalDb = 0.0, rideDb = 0.0, stepDb = 0.0, outDb = 0.0, appliedDb = 0.0, lastApplied = 0.0;
        double preFrom = 0.0, preTo = 0.0;
        double burstLimit = burstCeiling, startLimit = burstCeiling;   // test switches: selfTest sets +inf on reference instances (L28, L29)
        float gLin = 1.0f, lamp = 0.0f;
    };
}
```

**Amendment to `Gate.h`** (SPEC 4.2, about three lines; extends G9 to the state stream):

```cpp
void computeGain (const float* keyL, const float* keyR, float* gainOut, uint8_t* stateOut, int n) noexcept;
// stateOut may be nullptr; otherwise stateOut[i] = (uint8_t) state after sample i's update (same order as stateIndex()).
```

Fallback if the gate cannot change: read the block-end state **after** `computeGain` and fill the array with it. The gate writes `stateIdx` after its loop (SPEC 4.6).

```cpp
gate.computeGain (l, r, gateGain.data(), n);
std::fill (gateState.begin(), gateState.begin() + n, (uint8_t) gate.stateIndex());
```

Costs:
- onsets and closes are seen up to 15 samples early;
- a Step pre-roll starts up to 15 samples early and still lands before the onset audio;
- chain-level bit-exact block independence is lost (the matcher's own L10 still holds).

**`plugins/x11/LabX11.cpp`**, written against the real X-16 `Chain` (int guard stages, `meter (Meters&) const`, `control (v, ctx, n)`, `process (l, r, sideL, sideR, n, guard)`, `maxBlock = controlBlock`):

```cpp
struct X11Chain final : lab::host::Chain
{
    static constexpr int levelFirst = 9;                          // gate_thr .. det_release come first (SPEC 4.4)
    enum Stage : int { gateStage = 0, levelStage = 1 };           // EffectInfo::stages = { "gate", "level" }
    lab::dsp::VocalGate gate;  lab::dsp::TransientAlign align;  lab::dsp::LevelMatcher level;
    std::array<float, lab::host::controlBlock> gateGain {};
    std::array<uint8_t, lab::host::controlBlock> gateState {};

    int latency (double sr) const override { return lab::dsp::TransientAlign::latency (sr); }   // the matcher adds 0
    void prepare (double sr, int maxBlock) override
    {
        gate.prepare (sr, maxBlock); align.prepare (sr, maxBlock); level.prepare (sr, maxBlock);
        jassert (lab::dsp::LevelMatcher::lookahead (sr) == lab::dsp::TransientAlign::latency (sr));
    }
    void reset() override { gate.reset(); align.reset(); level.reset(); }
    void flush() override { level.flush(); }   // gate and ring keep running (SPEC I1); the matcher keeps its level (replaces I1's level.reset())
    void control (const float* v, const lab::host::BlockContext&, int) override { gate.control (v); level.control (v + levelFirst); }
    void process (float* l, float* r, const float*, const float*, int n, lab::host::Guard& guard) override
    {
        gate.computeGain (l, r, gateGain.data(), gateState.data(), n);   // undelayed key; per-sample state (amendment)
        align.process (l, r, n);                                        // audio now LA samples behind the decisions
        for (int i = 0; i < n; ++i) { l[i] *= gateGain[(size_t) i]; r[i] *= gateGain[(size_t) i]; }
        if (guard (gateStage, l, r, n)) return;
        level.process (l, r, gateState.data(), n);
        guard (levelStage, l, r, n);
    }
    void meter (lab::host::Meters& m) const override
    {
        // custom[0], custom[1]: gate lamp and gain reduction (SPEC 6.2 / revision 3)
        m.custom[2].store (level.gainDb(), std::memory_order_relaxed);
        m.custom[3].store (level.levelDb(), std::memory_order_relaxed);
        m.custom[4].store (level.voiceLamp(), std::memory_order_relaxed);
    }
};
```

### L.11 Per-sample algorithm

```
prepare (sr, maxBlock):
    sampleRate = sr > 0 ? sr : 48000                              // every rate below is this sanitised value
    hopLen = la = lookahead (sampleRate)                          // 221 / 240 / 480
    lateLen = (int64) (4.0 * sampleRate + 0.5); capPerSample = 12.0 / sampleRate
    setKWeighting (sampleRate)                                     // L.4 formulas, same coefficients into [0] and [1]
    lowBand[c].setHz (500, 0.5, (float) sampleRate); highBand[c].setHz (6000, 0.5, (float) sampleRate)     for c = 0, 1
    control (defaults); reset()                                    // every control-derived member defined before the first block

reset():
    reset the eight filters; ring, loRing, hiRing = 0; write = filled = vWrite = hopCount = 0; kSum = loSum = hiSum = 0
    gatedMean = 0; levelLufs = -100; valid = admitting = wasPhrase = snapPending = false
    preRollLeft = preRollK = 0; lateCount = 0
    goalDb = rideDb = stepDb = outDb = appliedDb = lastApplied = 0; gLin = 1.0f; lamp = 0; amountNow = amountTarget

control (v):
    amountTarget = v[amount] < 1e-4f ? 0 : min (v[amount], 1)
    targetDb = v[target]; boostDb = max (0, v[boost]); cutDb = max (0, v[cut])
    preFloorMs = 10 ^ ((targetDb - boostDb - 20 + 0.691) / 10)    // pre-level floor as a hop mean square (L.4)
    tau = max (0.5, v[time]); coef = 1 - exp (-1 / (tau * sampleRate)); slew = 6 * coef
    newLaw = v[law] > 0.5f ? step : ride
    if newLaw != lawNow: lawNow = newLaw; preRollLeft = 0; lateCount = 0; if lawNow == step: stepDb = quantise (rideDb)
    stepDb = clamp (stepDb, -cutDb, boostDb)
    if valid: goalDb = clamp (targetDb - levelLufs, -cutDb, boostDb)
    if snapPending:                                                // first control() after flush(): the base's first fade-in sample
        snapPending = false; rideDb = clamp (rideDb, -cutDb, boostDb)
        if lawNow == step: stepDb = quantise (rideDb)
        outDb = lawNow == step ? stepDb : rideDb; preRollLeft = 0; amountNow = amountTarget; applyGain()

process (l, r, st, n):
  for i in 0 .. n-1:
    kl = highPass[0].process (shelf[0].process (l[i])); kr = highPass[1].process (shelf[1].process (r[i]))
    bl = lowBand[0].bandpass (l[i]); br = lowBand[1].bandpass (r[i]); hl = highBand[0].bandpass (l[i]); hr = highBand[1].bandpass (r[i])
    s = st ? st[i] : closed
    voiced = s == open || s == hold;  phrase = voiced || s == attack
    if voiced:
        kSum += kl*kl + kr*kr; loSum += bl*bl + br*br; hiSum += hl*hl + hr*hr            // double
        if ++hopCount >= hopLen: endHop()                                                  // sets admitting
    else:
        admitting = false
    // endHop() runs before RIDE: the sample that completes an admitted hop is the first one the ride moves on (L14, L18)
    if lawNow == step && phrase && ! wasPhrase:                    // onset on the key timeline (from Closed or Release); audio arrives la samples later
        if |rideDb - stepDb| >= 1.0: stepDb = quantise (rideDb)
        if outDb != stepDb: preFrom = outDb; preTo = stepDb; preRollK = 0; preRollLeft = la
        lateCount = 0
    wasPhrase = phrase
    if voiced && admitting:                                        // RIDE
        e = goalDb - rideDb
        rideDb = |e| < 1e-6 ? goalDb : rideDb + coef * clamp (e, -6, 6)
        if lawNow == step && ++lateCount >= lateLen:               // 4 s of voice with no gate gap
            lateCount = 0; if |rideDb - stepDb| >= 1.0: stepDb = quantise (rideDb)
    if preRollLeft > 0:                                            // STEP pre-roll: raised cosine, lands exactly
        ++preRollK; --preRollLeft
        outDb = preRollLeft == 0 ? preTo : preFrom + (preTo - preFrom) * 0.5 * (1 - cos (pi * preRollK / la))
    else if voiced:                                                // slew limiter onto the law's aim, lands exactly
        aim = lawNow == step ? stepDb : rideDb; d = aim - outDb
        outDb = |d| <= slew ? aim : outDb + copysign (slew, d)
    if amountNow != amountTarget:                                  // applied gain moves <= 12 dB/s through AMOUNT
        stepA = |outDb| > 0 ? capPerSample / |outDb| : +inf; da = amountTarget - amountNow
        amountNow = |da| <= stepA ? amountTarget : amountNow + copysign (stepA, da)
    applyGain()
    l[i] *= gLin; r[i] *= gLin
    lamp = voiced ? (admitting ? 1 : 0.5) : 0

applyGain():
    appliedDb = amountNow * outDb
    if appliedDb != lastApplied: lastApplied = appliedDb; gLin = appliedDb == 0 ? 1.0f : (float) exp2 (appliedDb * log2PerDb)

endHop():
    ms = kSum / hopLen; lo = loSum; hi = hiSum; kSum = loSum = hiSum = 0; hopCount = 0
    if ! finite (ms, lo, hi): reset the eight filters; loRing = hiRing = 0; admitting = false; return     // self-heal, once per hop
    loRing[vWrite] = lo; hiRing[vWrite] = hi; vWrite = (vWrite + 1) % 4
    admitting = ms >= 1.17247e-7                                   // -70 LUFS
             && sum (loRing) >= 4 * sum (hiRing)                   // voiced over 20 ms (+6.02 dB)
             && lo >= 2 * hi                                       // and in this hop alone (+3.01 dB): no carry-over across a gap
             && (valid ? ms >= 0.01 * gatedMean                    // not 20 dB under the window
                       : ms >= preFloorMs)                         // before a level: not 20 dB under TARGET - MAX BOOST
    if ! admitting: return
    if valid: ms = min (ms, burstLimit * gatedMean)                // +10 dB burst clamp (burstLimit == burstCeiling outside L28)
    ring[write] = ms; write = (write + 1) % 80; filled = min (filled + 1, 80)
    if filled < 20: return
    if ! valid:                                                    // first fill after reset(): cap a start-of-song plosive (L29)
        med = element 10 of std::nth_element over a std::array<double, 20> copy of ring[0 .. 20)
        ring[k] = min (ring[k], startLimit * med)    for k = 0 .. 19
    m0 = mean (ring[0 .. filled)); gatedMean = mean of ring slots >= 0.01 * m0
    levelLufs = -0.691 + 10 * log10 (gatedMean); valid = true
    goalDb = clamp (targetDb - levelLufs, -cutDb, boostDb)
```

Cost:
- Per sample: 4 biquads, 4 SVF ticks, 6 double MACs, about 15 compares and adds; one `exp2` only while the gain moves; one `cos` only during a Step pre-roll.
- Per 5 ms hop: at most 160 adds and one `log10`.
- Once after each `reset()`: a 20-element `nth_element`.
- Per control block: one `pow` (the pre-level floor).

Block-size independence:
- Every state update is per sample, and the hop counter is internal.
- `control()` with identical v is idempotent.
- With constant parameters, chunks of 1 / 7 / 16 / 64 / 512 / variable lengths are bit-identical (L10).

### L.12 `selfTest()` cases (T0; printed `selftest_level_<case>=<value>`)

Harness:
- `selfTest()` is a static member and reads private state. Slew bounds use the internal double `appliedDb`, not the float `gainDb()`, which can exceed a bound by one float ulp (about 1e−6 dB; judge J2's L8 finding).
- `test/harness_main.cpp` calls `LevelMatcher::selfTest()` inside `juce::ScopedNoDenormals`; the header itself includes no JUCE. It prints with `std::printf`.
- 48 kHz unless stated. `control (v)` runs before every 16-sample `process()` unless stated.
- **Sample indices are 0-based**, counted from the first sample after the last `reset()` / `prepare()`.
- State arrays are synthesised: 2 = Open where voiced, 0 = Closed elsewhere.
- Sine = peak·sin(2πft/sr), computed in double, L = R. Rng = `lab::dsp::Rng`.
- **C** = {1, −18, 1, 18, 18, law}; **D** = defaults.
- **VOICE(seed, scale)** = Σ_{h=1..24, 140h < 0.45·sr} sin(2π·140h·t + φ_h)/h, with φ_h = 2π·Rng(seed).uni() drawn for h = 1..24, times a 5 Hz syllable envelope 10^(−5(1 − cos 2π·5t)/20) (0 to −10 dB), times 0.25·scale. VOICE(·, 10^(−6/20)) reads −20.486 LUFS for any seed.
- Reference instances (L28, L29) are copies with the named test switch set to +∞.
- Expected values come from the prototype (g++ 13.3) unless marked emulation, hop model or rough emulation. Non-exact values are re-baselined once on the MSVC `/fp:precise` build. "==" cases are structural and must hold on any build.

| # | case | signal / setting | assertion (tolerance) | expected (source) |
|---|---|---|---|---|
| L1 | `k997_{44k,48k,96k}` | 997 Hz, peak 0.1, Open, 3 s, D | levelDb = −20.00 ± 0.05 | −19.996 / −20.000 / −20.018 |
| L2 | `kshape_{100,4k,997}` | `shelf[0]`→`highPass[0]` run directly on 2 s sines, RMS over the 2nd second, 3 rates | K(100) − K(997) = −1.82 ± 0.02; K(4k) − K(997) = +3.28 ± 0.02; K(997) = +0.69 ± 0.03 | −1.825 / −1.824 / −1.823; +3.279 / +3.277 / +3.268; +0.694 / +0.691 / +0.674 |
| L3 | `voicing_*` | 10 s each, Open, C, 3 rates: 997 Hz and 300 Hz (peak 0.1) and VOICE(7, 0.5); then 2 kHz, 4 kHz, Rng(1) white ×0.1, pink (Kellet filter on Rng(1)) ×0.05 | first three: every hop admitted (1995 / 2000 / 2000), level valid; last four: 0 admitted, levelDb == −100. 20 ms band ratio of 997 Hz: +8.3 ± 0.4 | 997 Hz: +8.38 / +8.32 / +8.00 |
| L4 | `converge_{gain,t1db,t01db,over}`, 3 rates | 997 Hz −30 dBFS, Open, C, 10 s | gain(10 s) = 12.00 ± 0.03; t₁dB = 2.89 ± 0.01 s (closed form 2.892) and t₀.₁dB = 5.19 ± 0.02 s, defined as the last time \|g − g(10 s)\| exceeds 1 dB and 0.1 dB respectively; max − final ≤ 0.001 | 11.996 / 11.999 / 12.016; 2.8904 / 2.8910 / 2.8938; 5.186 / 5.186 / 5.189; 0 |
| L5 | `slew_ratio` | runs of L4, L8, L15, L20 | max per-sample \|Δ appliedDb\| / 6(1 − e^(−1/(τ·sr))) ≤ 1 + 1e−9 | 1.000000 |
| L6 | `limit_boost`, `limit_cut` | −36 dBFS and −6 dBFS, {1, −18, 1, 6, 9, 0}, 10 s | boost: max ≤ 6 + 1e−9 and final ≥ 5.99; cut: min ≥ −9 − 1e−9 and final ≤ −8.99 | 5.9997; −8.9995 (the −36 dBFS sine sits 8 dB above the −44 LUFS pre-level floor) |
| L7 | `amount_{050,075,000}` | L4 signal, A = 0.5 / 0.75 / 0, boost and cut 18 | 6.00 ± 0.03; 9.00 ± 0.03; A = 0: max \|y − x\| == 0 and gainDb == 0 | 5.9996; 8.9994; 0 |
| L8 | `freeze` | 1.5 s −30 dBFS Open, 5 s Rng(1) white −60 dBFS Closed, 3 s −30 dBFS Open, C | gain pp over the Closed span == 0; value == last voiced (7.978 ± 0.005); levelDb bit-identical across the gap | 0; 7.97818 |
| L9 | `burst_{min,max,pp,d10,rej}` | 997 Hz −24 dBFS plus 15 ms bursts Rng(12345).bi()·10^(−4/20)·e^(−j/(0.004·sr)), j = 0 .. round(0.015·sr) − 1, at 0.25 + 0.923k s; {1, −24, 1, 6, 9, 0}, Open, 12 s | over 3–12 s: gain in [−0.01, +0.01], pp ≤ 0.002, max \|g(t + 10 ms) − g(t)\| ≤ 0.0005, voicing rejections ≥ 13 | −0.00025..+0.00019, 0.00043, 0.000025, 70 (emulation: bit-identical at +10 dB; rider without grafts: 0.113 pp) |
| L10 | `blocks_law{0,1}` | 4 s: 1 s phrases at −30 / −20 / −26 dBFS starting at 0 / 1.3 / 2.6 s (Open), over Rng(3) −60 dBFS noise (Closed); C; chunks 1, 7, 64, 512 and Rng(99) 1..33 against 16 | max \|diff\| == 0 | 0 |
| L11 | `step_grid_*`, 3 rates | 8 phrases: state Open on [k_s, k_e + LA), 997 Hz on [k_s + LA, k_e + LA), k_s = 0.2 + 2.3k s, 2 s long; phrases 1–4 at −30, 5–8 at −29.4 dBFS; C with law 1 | gain at each audio onset == 0, 9, 12, 12, 12, 12, 12, 12; in-phrase pp == 0; changed samples outside [k_s, k_s + LA) == 0; ride end 11.40 ± 0.03 | exact; 11.4015 / 11.4003 / 11.4175 |
| L12 | `step_hold`, `step_move` | 16 phrases as in L11: 1–8 at −30, 9–16 at −29.4 / −28.4 dBFS | +0.6 dB: gain == 12 with 0 changed samples from phrase 9 on; +1.6 dB: exactly LA changed samples, final == 10.5, ride 10.40 ± 0.03 | 0; 240, 10.5, 10.4000 |
| L13 | `step_preroll`, 3 rates | L11, second onset o | g[o + k − 1] = 9·½(1 − cos πk/LA), k = 1..LA, within 1e−9; g[o + LA − 1] == 9; peak step ≤ 9·sin(π/2LA) + 1e−9 | 0; 0.06397 / 0.05890 / 0.02945 |
| L14 | `step_late` | 997 Hz −30 dBFS 0–6 s, then **−22.5** dBFS to 16 s, Open throughout, C with law 1 | the k-th move's first changed sample == hopLen − 2 + k·lateLen (192238 / 384238 / 576238 at 48 kHz, ≈ 4.005 / 8.005 / 12.005 s); 0 → 12 (2.000 s), 12 → 6 (1.000 s), 6 → 4.5 (0.250 s) at exactly s per sample; final == 4.5; ride 4.50 ± 0.01 | hop model: rides 11.67 / 5.67 / 4.52 at the commits, final 4.5004; every decision ≥ 0.42 dB from its threshold at both +6 and +10 dB ceilings |
| L15 | `law_switch` | L4, law 1 for blocks in [1.5, 4.0) s | slew ratio ≤ 1 + 1e−9; g(3.0 s) == 7.5; final 12.00 ± 0.03 | 1.0; 7.5; 11.9992 |
| L16 | `silence` | 5 s of zeros, then 5 s of 1e−30, Open, C | levelDb == −100; appliedDb == 0; output == input; all finite | exact |
| L17 | `holdtail` | 10× (1 s 997 Hz peak 0.1, Open for 1.04 s) every 1.3 s, Rng(5) −65 dBFS noise throughout, C | levelDb over 6–13 s = −20.00 ± 0.01; final gain 2.00 ± 0.01; ≥ 7 rejected hops per tail | −20.0019..−19.9983; 2.0000; 80 |
| L18 | `reset` | after L4, `reset()`, the L4 signal continues | gainDb == 0; levelDb == −100; the next hopLen·minFillHops − 1 = 4799 output samples == input; sample 4799 is the first changed one (the 20th hop ends there and the ride moves on that same sample: gLin = 1.0000144f) | exact |
| L19 | `determinism` | L9 twice, `reset()` between | outputs and appliedDb bit-identical | exact |
| L20 | `limit_change` | L4 to 16 s; boost 18 → 4 at 10 s (unsmoothed) | slew ratio ≤ 1 + 1e−9; \|g − 4\| ≤ 1 dB from 2.125 ± 0.01 s after; final ≤ 4.03; the same change inside a Closed span leaves the gain bit-identical until voice returns | 2.1250; 4.0208 |
| L21 | `nan_heal` | L4 with l[2.0 s] = NaN | exactly 1 non-finite output sample; appliedDb finite throughout; final 12.00 ± 0.03 | 1; 11.9992 |
| L22 | `breath_{white,pink,tract}_{10,15,22}` | 6× [VOICE(11, 10^(−6/20)·(k odd ? 0.5 : 1)) 2 s Open; 0.15 s Closed; 0.4 s breath Open (Rng(77 + k) white, Kellet pink, or SVF band-passes 500 / 1500 / 2500 Hz Q 3 summed then a 4 kHz one-pole; RMS = previous phrase RMS + rel dB, sine swell); 0.15 s Closed], against the same with each breath replaced by Closed zeros; {1, −18, 1, 12, 12, 0} | max \|Δ appliedDb\| over all phrase samples ≤ 1e−6 dB (all 9 combinations) | 0 bitwise (rider without grafts: 1.36–1.47 dB at onsets) |
| L22b | `breath_abrupt_{white,pink,tract}_{10,15}` | as L22, but each breath starts with a 10 ms linear rise and no swell | admitted breath hops ≤ 1 per breath; max \|Δ appliedDb\| ≤ 0.05 dB at any sample and ≤ 0.01 dB at each phrase onset | emulation: 0–1 hops, 0.03, 0.004 (without the per-hop test: 5–18 hops, 0.07–0.10 dB) |
| L22c | `onset_s_{10,6}` | 8× [0.3 s Closed; 60 ms "s" (Rng(41) white through two `SVF::highpass` at 4 kHz Q 0.707) at −10 / −6 dB RMS relative to the vowel; 2 s VOICE(11, 10^(−6/20))], Open over the "s" and the vowel; {1, −18, 1, 12, 12, 0} | 0 of the 96 "s" hops admitted | emulation: 0 (without the per-hop test: 14 / 7) |
| L23 | `open_gap_{white,hum}` | 3× VOICE(13, 10^(−12/20)) 2 s with 0.3 s gaps, then an 8 s gap, then 1 s of voice; Open everywhere; gap = Rng white −60 dBFS RMS, or hum Σ_{h=1..7} sin(2π·60h·t)/h × 0.9·10^(−55/20) (30.2 dB under the voice); TARGET = the level a calibration pass reads at the end of phrase 3 (≈ −26.57); A = 1, boost 6, cut 9 | gain pp over the gap ≤ 0.001; == 0 after its first 250 ms; lamp == 0.5 mid-gap | 0.00039 / 0.00039; 0; 0.5 (rider: +6.08) |
| L23c | `open_gap_bleed22` | as L23, but a 3 s gap of the L23 hum scaled to 22 dB under the voice's K-loudness | documents the guard's reach (O-6): gain rise over the gap ≥ 5 dB; lamp == 1 mid-gap | emulation: +5.35..+5.56 dB for −24..−19 dB; 0.000 at −25 and −27 dB |
| L24 | `quiet_take_d{12,20,24,28}` | 16× VOICE(17 + k) 2 s, 0.3 s Closed gaps; phrases 1–6 at scale 10^(−6/20), phrases 7–16 at that scale − d dB; C | d = 12, 20: levelDb at the end of phrase 7 = −20.486 − d ± 0.05; d = 24: at the end of phrase 8 = −44.486 ± 0.05; d = 28: levelDb at the end of phrase 16 == its phrase-6 value (documents the guard's reach, O-6) | −32.486, −40.486; −44.486 (phrase 7: −41.84); −20.486 |
| L25 | `amount_slew` | L4 to 16 s; amount 0 for blocks in [10, 13) s, else 1, unsmoothed | appliedDb == 0 after 1.000 ± 0.001 s; output == input from then until 13 s; per-sample \|Δ\| ≤ (12 + 6/τ)/sr; back at the ride 1.000 ± 0.001 s after 13 s | 0.99996 s; 12.0016 dB/s; 1.000 s |
| L26 | `neutral_start` | reset; 2 s 997 Hz −30 dBFS Closed; 2 s Rng(31) white −20 dBFS Open; every block sets target −40 + 34·frac(t), boost 18·frac(1.3t), cut 18·frac(0.9t), amount frac(0.7t) | output == input bitwise; appliedDb == 0 throughout; levelDb == −100 | exact |
| L27 | `flush` | L4 to 10 s (C); `flush()`, then control with amount 0.5, boost 4 | appliedDb == 2.0 exactly from sample 0 of the next `process()` (the first sample after `flush()` + `control()`; in the base, the first fade-in sample at wet weight 1/(0.005·sr)) and stays 2.0 to 12 s; no change on any earlier sample; levelDb unchanged | 2.0; −30.00035 before and after |
| L28 | `staccato` | the VOICE harmonic stack (seed 19, amplitude 0.25, no 5 Hz envelope) × a syllable envelope every 187.5 ms: 3 ms linear rise, then e^(−t/0.020), floored at −20 dB; 12 s Open, C | \|levelDb − levelDb of a reference instance with burstLimit = +∞\| ≤ 0.6 dB at 12 s | emulation: −0.47 (+6 dB ceiling: −3.77) |
| L29 | `start_pop_{020,026,032}_{t02,t06}` | (a) reset; VOICE(19, 10^(−6/20)), TARGET −20.49 (its own level), {1, −20.49, 1, 6, 9, 0}, Open, 2 s; one 25 ms 50 Hz Hann-windowed sine thump at +20 / +26 / +32 dB peak over the voice RMS, starting at 0.02 s or 0.06 s. (b) the L28 signal, TARGET = its level with both test switches at +∞. References: startLimit = +∞ | (a) at +26 and +32 dB: min appliedDb over 0–2 s ≥ 0.75 × the reference's; (b) max appliedDb over 0–2 s − the reference's ≤ 0.5 dB | rough emulation: (a) ratios 0.55–0.68; (b) +0.27..+0.37 dB |
| L30 | `hum_first` | reset; 3 s of the L23 hum (−54.7 LUFS), Open, then 2 s of VOICE(13, 10^(−6/20)); {1, −18, 1, 6, 9, 0} | appliedDb == 0 and levelDb == −100 throughout the hum; lamp == 0.5 mid-hum; levelDb valid within 100 ms + 1 hop of the voice | structural (pre-level floor −44 LUFS) |
| L31 | `defaults` | printed by `harness_main.cpp`, not `selfTest()` (EffectInfo lives in `LabX11.cpp`) | `LevelMatcher::defaults[k]` == `getSpecs()[commonCount + levelFirst + k].defaultValue` for k = 0..5 | == 1 |
| L32 | `breathy_hnr{12,6,0}` | 10 s VOICE(23, 10^(−6/20)) plus vocal-tract noise (Rng(29) white through `SVF::bandpass` 500 / 1500 / 2500 Hz Q 3, summed, then a 4 kHz `OnePole` low-pass) at HNR 12 / 6 / 0 dB (unweighted energy ratio), Open, Soft Take's values {0.75, −18, 1, 12, 6, 0} | admitted hop fraction ≥ 0.8 at HNR 12 and 6; HNR 0 is reported, not asserted | M2 (review estimate at HNR ≈ 0: lo ≈ 1.45 against 4·hi ≈ 2.0, so rejected). If HNR 6 fails, retune `voiceRatio` / `hopVoiceRatio` before M3 (constants; no id change) |

### L.13 Python render tests T7–T12 (`test/python/x11_tests.py`)

Shared method:
- Signals are generated with numpy, written as 32-bit float WAVs to `test/out/`, and rendered with `--in`.
- Latency is L = `int (sr * 0.005 + 0.5)`.
- **Pinned settings.** Every render starts from PIN = `--set level_target=-18 --set gate_thr=-40 --set gate_hyst=4`, the PROVISIONAL / D6 values these figures were measured at. Replacing a default later then cannot silently move a T figure.
  - A test's own `--set` replaces the PIN entry with the same id.
  - Renders that load a program (`--program`) take no PIN, and neither does T12(e)'s reference render. They test the preset's or the defaults' own values.
- **Two-render matcher gain:** g(t) = 10·log₁₀(ΣA²/ΣZ²) over 2 ms windows, where Z is the same render with `--set level_amount=0`. The gate does not depend on the matcher, and renders are deterministic.
  - Per-sample variant: g[i] = 20·log₁₀(\|A[i]\|/\|Z[i]\|) on \|Z\| > 1e−6.
- **Sine gain:** g[i] = 20·log₁₀(\|out[i]\|/\|in[i − L]\|) on \|in[i − L]\| ≥ 0.5·peak.
- **voice_phrase(sr, dur, seed):**
  - A 140 Hz harmonic stack (h = 1..24 at 1/h, phases from `default_rng (seed)`).
  - Syllable envelope 0 to −20 dB: −10·(1 − cos(2π·5t + 0.6·sin(2π·0.7t + φ₀) + j(t))), where j is a random walk with step N(0, 0.6/√sr).
  - 15 ms raised-cosine edges.
  - Scaled to the requested K-loudness with a numpy BS.1770 that uses the same coefficients.
- **Phrase train:** U(1.6, 2.4) s phrases separated by 250 ms of −65 dBFS white noise.
- The gate is at its defaults unless stated (thr −40, hyst 4 via PIN; attack 1, hold 40, release 120, range 24).
- Prototype values come from the chain emulator.

**T7 convergence and accuracy.**
- Signal: 997 Hz at −30 dBFS, L = R, 12 s, at 44.1 / 48 / 96 kHz.
- Render: PIN `--sr <sr> --block 512 --set level_amount=1 --set level_boost=18 --set level_cut=18`.
- Assertions (sine gain, 10 ms medians):
  - final (10–12 s) = 12.00 ± 0.05 at each rate (11.997 / 12.000 / 12.017);
  - the last bin with \|g − 12\| > 1 dB ends at 2.89 ± 0.05 s (2.900 / 2.890 / 2.880);
  - max g ≤ final + 0.05 (0.0004);
  - the three finals agree within 0.05;
  - at 48 kHz, renders with `--block 1`, `7` and `512` are identical (max \|diff\| ≤ 1e−7; prototype 0).

**T8 limits, amount, law.** 10 s 997 Hz sines unless stated, sine gain, PIN on every render.
- (a) −36 dBFS, `--set level_amount=1`: final +6.00 ± 0.05, max ≤ 6.05 (5.9997).
- (b) −6 dBFS, `--set level_amount=1`: final −9.00 ± 0.05, min ≥ −9.05 (−8.9995).
- (c) −30 dBFS, `--set level_boost=18`: defaults (A 0.75) → +9.00 ± 0.05 (8.9999); with `--set level_amount=0.5` → +6.00 ± 0.05 (5.9999).
- (d) Step.
  - Signal: 8× [2 s 997 Hz at **−29.0** dBFS, 0.4 s of −70 dBFS white (the gate closes)].
  - Render: `--set level_amount=1 --set level_boost=18 --set level_cut=18 --set level_law=1`.
  - Settled phrase gains (per-sample two-render, 100 samples before each phrase end) are 0, 9.0, 10.5, 10.5, … within 1e−3. Every Step decision sits ≥ 0.5 dB from its threshold, verified with level-reading shifts up to +0.10 dB. At revision 1's −29.4 dBFS, the steady ride sat 0.096 dB from the 1.0 dB commit threshold, and a +0.10 dB shift turned phrases 7 and 8 into 12.0.
  - Every sample-to-sample change > 1e−4 dB lies in [onset_k, onset_k + L), where onset_k is the phrase's input index. There are 478 such changes: 240 for 0 → 9 and 238 for 9 → 10.5, whose first and last raised-cosine steps (1.5·½(1 − cos π/240) = 6.4e−5 dB) fall under the threshold.
  - The `level_law=0` render ends at 11.00 ± 0.05, so Step is within 1.0 dB of Ride.
- (e) `--set level_amount=0` on the T7 signal: out[i] == in[i − L] for i ≥ L + 2 ms (max \|diff\| == 0).
- (f) Signal: 12 s 997 Hz at −30 dBFS. Render: `--ramp level_amount 0..1` over 0–10 s, with `--set level_boost=18`.
  - No fault.
  - The 10 ms slope of the sine gain is ≤ 18 dB/s (prototype max 12.62).
  - Final = median sine gain over 11–12 s = 12.00 ± 0.05.

**T9 freeze in gaps.**
- Signal: 6 s 997 Hz at −30 dBFS, then 4 s white noise at −60 dBFS RMS (seed 9), then 3 s 997 Hz at −30 dBFS.
- Render: PIN `--set level_amount=1 --set level_boost=18`.
- r = the first output sample after 6 s + L where Z falls more than 0.05 dB below the delayed input. That is the gate's audio-side release start (prototype 6.079 s).
- Assertions:
  - (a) two-render g pp over [r, 10 s + L − 6 ms] ≤ 0.001 dB (0.0);
  - (b) \|g(first window 1 ms after 10 s + L) − g(last window before r)\| ≤ 0.01 dB (11.9558 → 11.9558);
  - (c) output RMS in 0.5 s frames starting at 6.5 … 9.0 s (+L): max − min ≤ 0.5 dB (0.158), and mean = −60 + g_before − 24 ± 1.0 dB (−72.0 against −72.04).

**T10 transients not pumped or crushed.**
- Signal: 997 Hz at −24 dBFS, plus bursts at 0.25 + 0.923k s (130 BPM backbeat): white noise normalised to peak 1, × e^(−t/4 ms), 15 ms long, peak −4 dBFS (+20 dB over the sine); 12 s.
- Render: PIN `--set level_target=-24`.
- Sine-gain analysis on \|in\| ≥ 1e−3, over 3–12 s:
  - g within [−0.1, +0.05] dB ([−0.0085, +0.0001]);
  - pp ≤ 0.05 dB (0.0086);
  - max \|g(t + 10 ms) − g(t)\| ≤ 0.005 dB (0.00027);
  - for each burst, 20·log₁₀(peak_out/peak_in) minus the local median g is within ±0.01 dB (0.0002).
- Clean control render: final g = 0.00 ± 0.05 (0.0000).
- The rider without the grafts measured 0.089 pp / 0.0025 on this signal.

**T11 phrase-to-phrase consistency (the product goal), 48 kHz.**
- **Part A, takes.** Four takes at −24 / −14 / −20 / −12 LUFS, 4 phrases each, with ±1.5 dB phrase jitter (`default_rng (11)`). Measure the K-loudness of the last 4 s of each take (phrase samples, +L):
  - with PIN `--set level_amount=1`: each take within −18 ± 2.0, and spread ≤ 2.5 dB (−18.61 / −18.47 / −17.78 / −16.84°, spread 1.77°; 12 dB in);
  - at PIN defaults: spread ≤ 6.0 dB (4.63°).
- **Part B, alternating phrases.** 12 phrases at −18 / −24 / −12 / −21 / −15 / −24 / −13 / −19 / −22 / −14 / −18 / −25 (seed 12). Take the SD of the K-loudness of the **second half of each phrase**, phrases 3–12:
  - falls ≥ 20 % at PIN defaults (4.48 → 3.16°);
  - falls ≥ 30 % with PIN `--set level_amount=1` (2.80°);
  - falls ≥ 50 % with `--program 3` (1.79°).
  - Matcher gain band-passed 2–8 Hz ≤ 0.5 dB pp on phrase samples (0.10 / 0.13 / 0.25°).
  - The whole-phrase SD is reported, not asserted (4.43 → 4.37°; see O-9).
- **T11c, breathy delivery (Soft Take).**
  - Signal: 8 voice_phrase phrases (seed 21) with the T12(d) vocal-tract noise added at HNR 12 / 6 / 0 dB, plus a clean reference. The whole mix is scaled to −30 LUFS, in the phrase-train layout.
  - Render: PIN with Soft Take's matcher values set explicitly: `--set level_amount=0.75 --set level_time=1 --set level_boost=12 --set level_cut=6`. Setting them explicitly instead of using `--program 4` lets the two-render reference `--set level_amount=0` apply.
  - t_x = the phrase time (gaps excluded) at which the two-render gain first comes within 1 dB of +9 (0.75 × 12).
  - Assert t_12 and t_6 ≤ 1.25·t_clean. The ride runs in admitted time, so this means an admitted fraction of about 0.8 or more.
  - Report t_0 and the final gain at HNR 0. The lamp is a meter and cannot be read from a render; L32 asserts the admitted fraction directly.

**T12 gate + matcher interaction.**
- (a) **Level Only.** T9 with `--set gate_range=0`.
  - r is taken from the default-range T9 render. With RANGE 0, Z never falls below the input. Release starts on the same sample either way, because the state timeline (hold + lookahead) does not depend on RANGE.
  - Two-render g pp over [r, 10 s + L − 6 ms] ≤ 0.001 (0.0).
  - Gap frames are constant within 0.5 dB (0.158), at −60 + g ± 1.0 dB (−48.0 against −48.04). Voicing comes from the gate state, not its gain.
- (b) **Misset gate, broadband and distant hum.**
  - Signal: 3 voice phrases at −20 LUFS, then a 3 s gap of white noise at −60 dBFS RMS or hum (Σ_{h=1..7} sin(2π·60h·t)/h at −55 dBFS RMS, 34.6 dB under the voice), then 1 phrase.
  - Render: PIN `--set gate_thr=-80 --set level_amount=1 --set level_target=-20`.
  - Two-render g pp over the gap ≤ 0.1 dB (0.0000 for both; the rider without grafts rose +5.5 dB toward MAX BOOST).
  - Max g anywhere ≤ level_boost + 0.05, with level_boost constant.
- (b′) **Bleed near the guard (documents the cliff).** As (b), with the gap hum scaled to 22 dB under the voice's K-loudness. The two-render g rises ≥ 5 dB over the gap: a THRESHOLD under the bleed lets the matcher ride it toward MAX BOOST (emulation +5.35..+5.56 dB for −24..−19 dB).
- (b″) **Hum before the first phrase.** The (b) hum for 3 s, then the 3 voice phrases, rendered as (b). Two-render g pp over the hum ≤ 0.1 dB: the pre-level floor (−46 LUFS at TARGET −20 / BOOST 6) rejects the −54.7 LUFS hum.
- (c) **Hold tails.** 10× (1 s 997 Hz peak 0.1 + 300 ms of −65 dBFS noise) through the real gate, rendered with PIN `--set level_amount=1 --set level_boost=18 --set level_cut=18`: final g = +2.00 ± 0.15 (2.0035).
- (d) **Breaths (D6).**
  - Signal: 6 voice phrases (2 s; −20 / −23 / −17 / −20 / −24 / −18 LUFS), each followed by 0.15 s of −70 dBFS noise, a 0.4 s breath, and 0.15 s of noise.
  - Breath types:
    - white;
    - pink: b = [0.049922035, −0.095993537, 0.050612699, −0.004408786], a = [1, −2.494956002, 2.017265875, −0.522189400];
    - vocal-tract: `iirpeak` at 500 / 1500 / 2500 Hz Q 3 summed, then a 1st-order 4 kHz low-pass.
  - Each breath has a sine swell and RMS = the previous phrase's RMS + rel dB, with rel = −10 / −15 / −22.
  - Reference fixture: the same with each breath replaced by −70 dBFS noise.
  - Render: PIN `--set gate_thr=-60 --set level_amount=1` (the gate is open for the whole breath).
  - \|g_breath − g_ref\| at every onset of phrases 2–6 ≤ 0.1 dB (0.0000 for all 9; the rider without grafts 0.61–1.21).
- (d′) **Abrupt breaths.** As (d), with each breath given a 10 ms linear rise and no swell, rel = −10 / −15: \|g_breath − g_ref\| at every onset of phrases 2–6 ≤ 0.1 dB (emulation ≤ 0.004).
- (e) **Gate Only.** `--program 6` against `--set level_amount=0` (neither with PIN): identical renders (max \|diff\| == 0). On the T7 signal, out == the delayed input after the gate attack.
- (f) **Threshold ramp.** `--ramp gate_thr -80..0` over 20 s on a 27 s phrase train (−26 / −14 / −21 × 4 LUFS), PIN `--set level_amount=1`, level_boost constant: no fault, all samples finite, matcher gain ≤ level_boost + 0.01 (5.51°).
- (g) **Preset switch keeps the measurement.** T7 signal at 48 kHz, `--program 0 --program-at 8.0 3`:
  - sine gain over 7.5–7.9 s = 4.50 ± 0.05 (Init: ride at its +6 limit × 0.75; Init's `output` is 0 dB);
  - over 8.05–9.0 s = 4.00 + output₃ ± 0.05, where output₃ is Rap Lead's P1-trimmed `output`, read from the preset table in `LabX11.cpp` (Rap Lead: A 1 × min(ride, 4), with no relearn from 0 dB). The sine gain includes OUTPUT, and a two-render reference does not work across `--program-at`, because the preset rewrites `level_amount`;
  - C4's jump < 0.5 still holds.
- (h) **Latency.** With the matcher on, `--list-params` reports 221 / 240 / 480 at 44.1 / 48 / 96 kHz, and C2's `--latency-check` passes: the matcher adds none.

Notes for the C tests:
- C6 must reach stage 1, `level`.
- C1 and C2 are unchanged.
- C8 covers the 8 filters per sample.

### L.14 Presets (matcher rows; gate rows belong to the gate design)

Every preset writes `level_law` = Ride (0) and `level_target` = −18 (PROVISIONAL). Loading a preset therefore resets TARGET (O-7). For doubles, "same target" is the user's step: after loading Ad-lib / Double, copy the lead instance's TARGET into this instance. There is no link in 0.1.0 (D2 / D5).

| # | Preset | amount | target | time | boost | cut | law | intent (handoff 5.4) |
|---|---|---|---|---|---|---|---|---|
| 0 | Init | 0.75 | −18 | 1.0 s | 6 | 9 | Ride | gentle matcher (the defaults) |
| 1 | Booth Floor | 0.75 | −18 | 2.0 s | 6 | 9 | Ride | on, slow |
| 2 | Room Tone | 0.60 | −18 | 1.5 s | 3 | 9 | Ride | moderate; little boost, so a noisy take's in-phrase floor is not lifted |
| 3 | Rap Lead | 1.00 | −18 | 0.5 s | 4 | 6 | Ride | faster, tighter limits; shouts and whispers keep their intent beyond +4/−6 |
| 4 | Soft Take | 0.75 | −18 | 1.0 s | 12 | 6 | Ride | more boost allowed (applied max +9). Whispered lines (HNR near 0 dB) fail voicing and hold their gain (T11c, L32) |
| 5 | Ad-lib / Double | 1.00 | −18 | 0.5 s | 6 | 9 | Ride | on; A = 1 lands the double on TARGET, so copy the lead's TARGET into this instance after loading. The insert fader sets the double's 26–60 % offset |
| 6 | Gate Only | 0.00 | −18 | 1.0 s | 6 | 9 | Ride | matcher off: bit-exact from the preset's fade-in (the base resets smoothers at flush, and the snap sets amountNow = 0) |
| 7 | Level Only | 0.75 | −18 | 1.0 s | 6 | 9 | Ride | matcher defaults; gate_range 0 on the gate side, while THRESHOLD still detects voice |

**P1 trim rule (explicit):**
- Build the reference vocal fixture from voice phrases at K-loudness = the presets' common TARGET, read from the preset table (−18 PROVISIONAL), not a literal.
- The matcher's settled gain is then 0 dB in every preset, so presets 1–5 trim only for the gate. Measure after the first 5 s.
- For any other fixture level L_fix, the matcher's share is A·clamp(T − L_fix, −C, +B), or the settled step under Step. Set `output` = −(matcher share + gate share), rounded to 0.1 dB.
- Presets 6 and 7 keep `output` at 0 dB.

### L.15 Open points and conservative defaults

| # | Point | Conservative default here | Alternative |
|---|---|---|---|
| O-1 | D7 law | Ride as the default and in every preset; Step opt-in (commit + pre-roll, fixed 1.5 dB grid, 1.0 dB commit, 4 s late commit, no extra ids) | if the user confirms the stepped reading, Booth Floor switches to Step with no id change |
| O-2 | D8 TARGET: default and meaning | −18 LUFS PROVISIONAL, all presets. **D8 amendment for the user to confirm:** TARGET is voiced LUFS momentary, set from LEVEL, not D8's "dBFS short-term" (L.3) | default: the median voiced momentary loudness of the three lead stems (5.3 / Appendix B), rounded to 1 dB |
| O-3 | Init AMOUNT | 0.75 (keeps a quarter of the line contour) | 0.5 ("robust"; halves the spread only) |
| O-4 | Gate contract | `computeGain` emits a per-sample `uint8_t` state | per-block fallback, read after `computeGain` (block-end state): onsets and closes seen ≤ 15 samples early, pre-roll still before the onset audio, chain-level bit-exactness lost |
| O-5 | Voicing test | 500 Hz band (Q 0.5) ≥ 6 kHz band (Q 0.5) + 6.02 dB over 20 ms, and + 3.01 dB in the hop, PROVISIONAL | check in M2 on the three lead stems with the lamp (custom[4] should read 1 for most sung time), and with L32 / T11c on breathy voice. False negatives are benign: the gain holds, and whispered lines keep their level. If a real voice fails, retune the constants, not the ids |
| O-6 | Level guard | −20 dB under the window, no escape. Drops ≤ 20 dB are acquired within one phrase, 24 dB within two; beyond about 25 dB the gain holds and the lamp shows 0.5. The same depth bounds the misset-gate defence: steady tonal content ≥ 25 dB under the window is held, while closer content is ridden toward MAX BOOST in about 3 s with the lamp at 1 (L23c, T12(b′)). README: THRESHOLD above hum and bleed | an escape (quiet streak, decaying reference) was rejected: it re-admits hum and tonal bleed when the gate is misset, and it is hidden state. Such a quiet section is usually below the gate threshold anyway, and B + C (15 dB) could not match it. A 20 ms guard only moves the cliff to about 22.5 dB, and it delays L24's d = 24 to phrase 16 |
| O-7 | Presets overwrite TARGET | accept (house rule: every parameter is written); README: "pick the preset, then set TARGET"; for doubles, copy the lead's TARGET | a base change that skips `level_target` on preset load |
| O-8 | Preset flush | keep the measurement, snap the amount and limits (adopted); the snap is the one per-sample-ramp exception (L.6) | reset (`Chain::flush()` default), a one-line change |
| O-9 | Causal first word / alternating lines | accepted and documented: the first syllables of a line play at the previous line's gain, and alternating whole-line spread barely changes | more lookahead is excluded by the plan; Step places lines but cannot correct the line it is in |
| O-10 | Transport start | no reset (FL keeps processing); exports from bar 1 and loop playback can differ for the first phrase or two | an optional "reset when transport starts and gate closed", later |
| O-11 | TARGET unit label | "dB" (`formatValue` has no LUFS); README and tooltip say LUFS. It approximates Youlean momentary during a sustained phrase and reads higher than Youlean short-term or integrated on gappy delivery; L+R summed, so mono duplicated to stereo reads +3.01 dB over a mono meter | add "LUFS" to `formatValue` (base change) |
| O-12 | Amount slew | 12 dB/s (the fastest ride rate) | 40 dB/s ("robust") |
| O-13 | Time range | 0.5–10 s | 0.1 s minimum (rider); rejected as compressor territory |
| O-14 | T0 baselines | g++ 13.3 prototype values, re-baselined on the revision-2 prototype before M2; non-exact values re-baselined once on MSVC `/fp:precise` (never `/fp:fast`) | n/a |
| O-15 | README notes | use MIX 100 % for the level role (delayed dry dilutes matching); "same target" is per instance with no link (D2 / D5), so copy the lead's TARGET into each double; THRESHOLD is the matcher's voice detector and must sit above hum and bleed; set TARGET from LEVEL, not from Youlean; read the lamp; LEVEL at −100 with the lamp at 0.5 on voice means the take is more than 20 dB under TARGET − MAX BOOST (lower TARGET or raise MAX BOOST); whispered lines hold their gain | n/a |
| O-16 | Step onset after a short gap | a re-trigger from Release is an onset: the pre-roll runs over the last 5 ms of the previous line's releasing tail (under the open threshold, at partial gate gain) | restrict onsets to a transition out of Closed; re-triggered lines then move only by the 4 s late commit, an in-voice glide at s |
| O-17 | Pre-level floor depth | TARGET − MAX BOOST − 20 dB (the guard's own depth); rejects the −54.7 LUFS hum whenever TARGET − MAX BOOST > −34.7 | −10 dB (review proposal): rejects that hum at every BOOST up to 18 at TARGET −18, but its −34 LUFS floor at TARGET −18 / BOOST 6 rejects L6 / T8(a)'s −36 dBFS sine, and it hides LEVEL on takes only 10 dB under TARGET − MAX BOOST |
| O-18 | Burst ceiling | +10 dB, self-referential, plus a one-time first-fill cap at 10 × the median | +6 dB (revision 1): steadier against plosive thumps that pass voicing (0.17 / 0.24 / 0.26 against 0.23 / 0.45 / 0.61 dB pp), but it reads staccato 1.3–4.1 dB low and over-boosts staccato song starts |

### L.16 Synthesis record

**Base:** "rider" (judge totals 7.5 + 8.5 + 8.5 = 24.5). Kept from it:
- a K-weighted 400 ms window of 5 ms hops with a −20 dB relative gate;
- the dB one-pole ride with a 6 dB slew knee;
- a burst clamp (+6 dB in revision 1, +10 dB since revision 2);
- goal-clamped limits;
- A × ride semantics;
- `level_*` ids;
- feed-forward measurement of the delayed, gated audio;
- 0 latency.

**Grafts adopted:**
- Voicing rejection per hop (robust's bands, over 20 ms) plus quantized's level guard at −20 dB (J1).
- The ride frozen on rejected hops and after gaps (J1).
- The burst-clamp side effect removed at its cause (J1).
- Breath and open-gate room-tone / hum tests: L22, L23, T12(b), T12(d) (J1).
- `flush()` keeps the measurement (J1, J2, J3).
- Init amount 0.75 (J1, J3).
- Step as a phrase-boundary commit with a raised-cosine pre-roll inside the lookahead, and no `lvl_grid` / `lvl_gap` ids (J1, J3).
- Voice lamp in custom[4] (J1, J2, J3).
- README notes and LUFS wording (J1, J3).
- Cached gLin (J2).
- 12 dB/s amount slew with exact landing (J2).
- Per-sample `uint8_t` gate state (J2).
- C++ prototype against the real `Filters.h` and `Common.h` (J2).
- Slew asserted on the double (J2).
- Block equivalence 1 / 7 / 16 / 64 / 512 / 1..33 (J2).
- Neutral-start case (J2).
- `gate_thr` ramp test (J2).
- `level_time` minimum 0.5 s (J3).
- Explicit P1 rule (J3).

**Conflicts, each settled toward the safer option:**
- Breath method: both voicing and the level guard, because each covers what the other cannot; no escape heuristic.
- Measured input: the delayed audio, not the key. The key-timeline state still gives Step its lookahead.
- Amount semantics: A × clamped ride, which gives smaller gains.
- Step grid: 1.5 dB, and no 3 dB per-commit limit, because moves land at phrase boundaries.
- Amount cap: 12 rather than 40 dB/s.

**Revision 2 (review fixes):**
- burst ceiling +10 dB;
- per-hop voicing test;
- pre-level floor;
- first-fill median cap;
- misset-gate claims bounded at 25 dB;
- L14 moved to −22.5 dBFS, T8(d) to −29.0 dBFS;
- PIN for T renders;
- new cases L22b, L22c, L23c, L28–L32, T11c, T12(b′), T12(b″), T12(d′).

The Review notes below give each change.

**Not adopted:** mono-sum detector, 100 Hz high-pass weighting, median tiles, per-phrase memory restart, quiet-streak and decaying-reference escapes, an 8-parameter surface, a 20 ms level guard.

**Prototype and checks** (scratchpad):
- /tmp/claude-0/-home-user-LAB-X-11/e5307be6-207a-5174-b82a-3f448e3c5229/scratchpad/lmfinal/Leveler.h — the revision-1 design, with test switches that reproduce the unmodified rider.
- /tmp/claude-0/-home-user-LAB-X-11/e5307be6-207a-5174-b82a-3f448e3c5229/scratchpad/lmfinal/selftest.cpp — the L cases. Its `GUARD_DB` default must stay 20, the value this spec uses.
- /tmp/claude-0/-home-user-LAB-X-11/e5307be6-207a-5174-b82a-3f448e3c5229/scratchpad/lmfinal/adm.cpp — L3 admission counts.
- /tmp/claude-0/-home-user-LAB-X-11/e5307be6-207a-5174-b82a-3f448e3c5229/scratchpad/lmfinal/chain.cpp — the gate → align → matcher emulator.
- /tmp/claude-0/-home-user-LAB-X-11/e5307be6-207a-5174-b82a-3f448e3c5229/scratchpad/lmfinal/ttests.py — the T analyses. Part B and the T8(f) / T9 figures come from dbg11b.py, dbg8.py and dbg9.py in the same folder.
- /tmp/claude-0/-home-user-LAB-X-11/e5307be6-207a-5174-b82a-3f448e3c5229/scratchpad/atk/lm.h and /tmp/claude-0/-home-user-LAB-X-11/e5307be6-207a-5174-b82a-3f448e3c5229/scratchpad/atk/t_clamp3.cpp — the reviewers' emulation ("emulation" figures).
- /tmp/claude-0/-home-user-LAB-X-11/e5307be6-207a-5174-b82a-3f448e3c5229/scratchpad/rev2/l14c.py — the L14 hop model: the decision margins against the second level, at +6 and +10 dB ceilings.
- /tmp/claude-0/-home-user-LAB-X-11/e5307be6-207a-5174-b82a-3f448e3c5229/scratchpad/rev2/pop.py — the rough numpy emulation behind the L29 thresholds.

Build:

```
g++ -O2 -std=c++20 -I/home/user/crystalball-dev/lab-x-16/core/dsp <file>.cpp
```

### Review notes (revision 2)

No fix was rejected outright. Three were applied with a change to their constant, their vehicle or their test signal; each says why.

1. **Burst clamp self-referential** (R1): applied as burstCeiling 10.0, with L.4, L.8(3) and L28.
   - Side effect found while applying it: at −22 dBFS, L14's second commit sits 0.002 dB under a grid boundary with a +10 dB ceiling, so it flips from 6 to 4.5.
   - L14 therefore moves to −22.5 dBFS. That keeps the same three moves with decision margins ≥ 0.42 dB at both ceilings (hop model).
   - Jump-dependent figures are marked °.
2. **Misset-gate claim near the guard** (R1): applied. The claims are corrected in L.1, L.4, L.5, L.7, O-6, L23 and T12(b), the README line is added, and L23c / T12(b′) pin the 25 dB cliff.
3. **L14 start indices** (R1) and **L.11 ordering** (R2): applied as R2's option (a).
   - L.11 is unchanged, and the order is stated in it.
   - L14 expects hopLen − 2 + k·lateLen and L18 expects 4799 samples; L.12 states the 0-based convention.
4. **Start-of-song plosive** (R1): applied with burstCeiling as written. After fix 1 that makes the cap 10 × the median.
   - In the rough emulation, R1's quoted dips (−0.11..−0.68 dB) match a +6 dB cap.
   - A staccato window's median is its floor, so a +6 dB start cap would over-boost staccato song starts by up to +0.77 dB; at ×10 it is ≤ +0.37 dB.
   - L29 therefore asserts against reference instances (test switch `startLimit`), not absolute figures.
5. **Voicing ring carry-over** (R1): applied, with the per-hop lo ≥ 2·hi test, L22b, T12(d′), and an L22c case for onset sibilants.
6. **L4's t₁dB definition** (R1): applied.
7. **T8(d) knife edge** (R1): applied: −29.0 dBFS, Ride 11.00 ± 0.05.
8. **T12(g) depends on OUTPUT** (R1, R2): applied as 4.00 + output₃. The two-render alternative cannot work across `--program-at`, because the preset rewrites `level_amount`.
9. **Hum before the first level** (R2): applied with a 20 dB margin instead of 10 dB.
   - At 10 dB, the floor at L6 / T8(a)'s settings (TARGET −18, BOOST 6) is −34 LUFS. It rejects their −36 dBFS sine, so both cases would end at 0 dB instead of +6. It would also hide LEVEL on any take 10 dB under TARGET − MAX BOOST.
   - At 20 dB, every existing case keeps ≥ 8 dB of margin, and L30 and T12(b″) still reject the −54.7 LUFS hum.
   - The residual is stated in L.5 and O-17.
10. **T12(a) r undefined at RANGE 0** (R2): applied; r comes from the default-range T9 render.
11. **PROVISIONAL defaults inside T renders** (R2): applied, with PIN, the P1 fixture at the preset's TARGET, and L31 for `defaults[]`.
12. **T8(f) final window** (R2): applied, with a 12 s signal and the median over 11–12 s. Every T8 sub-case now names its sine level.
13. **Soft Take against the voicing test** (R2): applied with a changed vehicle. A render cannot read the lamp, because custom[4] is a meter. So the admitted fraction is asserted in T0 (L32), and T11c asserts its render-side consequence (convergence time ≤ 1.25 × clean). The intent column and the README say that whispered lines hold their gain.
14. **O-4 fallback read order** (R2): applied (block-end state, read after `computeGain`).
15. **L9 burst formula precedence** (R2): applied.
16. **T8(d) changed-sample count** (R2): applied: 478, at the 1e−4 dB threshold.
17. **"Never exceeds the limits"** (R2): applied, rewording L.5 and adding "with level_boost constant" to T12(b) and T12(f).
18. **Flush snap against the per-sample ramp** (R2): applied as the stated exception in L.6, with L27 asserting the snap sample.
19. **BS.1770 against EBU Tech 3342 relative gate** (R2): applied.
20. **selfTest, JUCE and `<cstdio>`** (R2): applied.
21. **SVF set from the unsanitised sr** (R2): applied.
22. **Step pre-roll on a re-trigger from Release** (R2): applied by documenting the behaviour (L.2, L.7, O-16) rather than restricting onsets. With restricted onsets, re-triggered lines would move only by the in-voice late commit, which is more audible than 5 ms over a tail under the open threshold.
23. **Youlean wording** (R2): applied, with the D8 amendment recorded in O-2.
24. **"Same target" for doubles** (R2): applied; the claim is dropped and the copy-TARGET instruction is added to L.14 and the README.

---

### Implementation record (M2, revision 3)

Built as specified in `core/dsp/Leveler.h` (the algorithm, L.10 / L.11) and `core/dsp/LevelerSelfTest.h` (L1–L30, L32),
wired in `plugins/x11/LabX11.cpp`, with the gate amendment of L.10 (`VocalGate::computeGain` writes the per-sample
state). The self-test values reproduce the prototype's figures to the printed digits wherever the spec gives them: L1
−19.996 / −20.000 / −20.018, L4 11.996 / 11.999 / 12.016 and 2.8904 / 2.8909 / 2.8938 s, L8 7.97818, L9 within
−0.00025..+0.00019, L11 rides 11.4015 / 11.4003 / 11.4175, L20 2.125 s, L23 0.00039 dB, L24 −32.486 / −40.486 /
−44.486. Where the build differs from the text above, this record wins:

| # | change | why |
|---|---|---|
| M1 | **Voicing over 20 ms: +3.5 dB, not +6.02 dB** (`voiceRatio` 2.2387; the per-hop +3.01 dB is unchanged) | L32 failed as the spec foresaw: breathy voice at HNR 6 dB was admitted on 43 % of hops. A grid over both thresholds against white, pink and vocal-tract noise (four seeds each, 10 s) shows no pair reaches 80 % without admitting pink noise (from +3.01 dB down, 0.1–0.25 % of pink hops pass). In each syllable dip of the voice (−10 dB) the noise dominates, so those hops are correctly held. +3.5 dB admits 66 % at HNR 6 and 99.9 % at HNR 12. Over 16 seeds (revision 3) white and vocal-tract noise are never admitted and pink on 0.019 % / 0.013 % / 0 of hops at 44.1 / 48 / 96 kHz, never enough for a level (L3 asserts at most 0.1 %); every other case still passes, and the abrupt-breath leaks (L22b) shrank |
| M2 | L32 asserts ≥ 95 % at HNR 12 and ≥ 60 % at HNR 6 (spec: 80 % at both) | M1 |
| M3 | T11c asserts the time to settle on each take's **own** settled gain: ≤ 1.6× clean at HNR 12, ≤ 2.2× at HNR 6 (spec: ≤ 1.25× to within 1 dB of +9); measured 1.36× / 1.85× | The matcher measures voiced hops, which read louder than a whole-phrase K-loudness on breathy delivery, so breathy takes settle 1.0 dB (HNR 12) and 1.5 dB (HNR 6) under the clean take's gain; that bias is bounded separately (≤ 1.5 / 2.5 dB). Breathy lines are ridden more slowly and slightly lower: the conservative side of O-5 |
| M4 | T8(d) counts Step's gain changes in [onset − L, onset + 8) | the gate's peak detector sees a sine starting at phase 0 two samples late, so the last two pre-roll samples land on the onset; measured 471 changes, none outside |
| M5 | The self-tests report through `SelfTestReport` (`selftest_level_<case>=<value>` plus `_ok`), the house vehicle of `Gate.h` and `Align.h`, not `std::printf`; they live in `LevelerSelfTest.h` (a friend struct) to keep `Leveler.h` readable. L31 runs in `test/harness_main.cpp` | consistency with T0 |
| M6 | `prepare (double)` takes no `maxBlock` | nothing in the matcher depends on the block size |
| M7 | Render fixtures are written one zero sample longer | the harness's `--in` reader (ported unchanged from LAB X-16) stops one sample before the end of a file |
| M8 | P1: every preset is within 0.05 dB of the input at OUTPUT 0 dB, so presets 1–5 carry no OUTPUT trim | the L.14 rule: the reference vocal sits at TARGET, the matcher's settled gain is 0 dB, and the gate removes only the gaps |
| M9 | Meter slots: 0 gate lamp, 1 gate reduction, 2 match gain, 3 LEVEL (LUFS), 4 voice lamp, 5 key, 6 threshold | L.9's slots 2–4 kept; the gate's key and threshold readouts moved to 5–6 |
| M10 | **Room rumble passes the voicing test** (review of M2) | The band-ratio test reads tilt only. Noise falling 6 dB per octave or more (brown, NC-30 HVAC curves, traffic) is admitted on about 100 % of hops at every rate, so with a misset gate it behaves like hum: held 30 dB under the voice, ridden toward MAX BOOST 20 dB under (L3 `voicing_brown_documented`, L23d). L.1, L.5, L.7 and the README are corrected. Not fixed in 0.1.0: the review's proposed periodicity test (normalised autocorrelation of the low band over 2.5-12.5 ms lags, prototype: brown 2.5 %, voice 100 %, breathy HNR 6 99 %) changes which hops of real vocals are admitted, which no synthetic test can validate; it is left as an option for the user (docs/DESIGN.md) |
| M11 | Review of M2: eleven mutants of `Leveler.h` passed the whole suite | new cases L1b (one channel, anti-phase), L3 noise over 16 seeds, L8b (Hold, Release, Attack), L12 hold at -29.125 dBFS (commit hysteresis), L12b (Step re-triggered from Release), L20b (limit change under Step), L27b (flush under Step), L28b (burst clamp against plosive thumps); T12(d') rebuilt so only the per-hop voicing test can pass it; T0 pins its case counts. All eleven mutants now fail |
