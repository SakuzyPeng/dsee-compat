# Native offline postprocessing

The C++17 `dsee_postprocess` library and `dsee-compat` CLI provide optional adaptive spectral-envelope
attenuation, complementary frequency-band mixing, or both. FFT, reference resampling, alignment and
synthesis run in the executable. No Python, NumPy, SciPy, FFmpeg or codec runtime is needed to use it.
Python is used only by development/test scripts. The library loads no Sony resources.

Build normally, or pass `-DBUILD_TESTING=OFF` when building without Python. Installation includes
`libdsee_postprocess.a` and `dsee_postprocess.hpp`, as well as the original runtime library and CLI.
The streaming C API retains its 0.1 ABI and processing behavior.

## Two entry points

Apply optional processing directly after DSEE, using its input WAV as the default reference:

```sh
build/dsee-compat process --assets /path/to/resource-root \
  --input input-48k.wav --output mixed.wav --output-rate 96000 \
  --postprocess mix --mix 0.5 --curve cosine \
  --crossover-low 18000 --crossover-high 22000
```

`process` still requires 48 kHz stereo PCM16. It defaults to `--postprocess none`.
Use `--postprocess` explicitly when passing postprocessing controls. A separate `--reference` WAV
can be supplied; otherwise the actual PCM16 input is used, with default reference gain 0 dB.

Process an existing result without loading Sony resources:

```sh
build/dsee-compat postprocess \
  --reference input-48k.wav --input dsee.wav --output smooth.wav \
  --mode combined --mix 0.5 --envelope-strength 1 --max-cut-db 12
```

The standalone command defaults to `--mode mix`. `--mode none` needs no reference and preserves
PCM32 samples exactly; PCM16 is represented exactly in PCM32. Zero-strength/zero-cut `adaptive`
also bypasses processing. A mode that mixes always requires a reference, including `--mix 0`.

Both commands return a JSON summary on stdout. Applied processing includes frame count, effective
reference delay, automatic correlation (or null), linear sample peak/RMS, curve and controls,
including the effective mix and any automatic selection diagnostics. `effective_mix` is the maximum
processed branch weight before adaptive gain: it is always `1` in `none` and `adaptive` modes,
including bypass and empty audio, and the fixed or automatically selected weight in `mix` and
`combined` modes. `parameters.mix` retains the requested control value even when that mode ignores it.
Sample peaks and RMS are normalized to full scale. Empty audio reports zero peak/RMS.

## Formats

Processed input: stereo integer PCM16 or PCM32, RIFF/WAVE format tag 1, at 48, 96 or 192 kHz.
Reference: the same PCM formats, at 44.1, 48, 88.2, 96, 176.4 or 192 kHz. Output: stereo PCM32 `.wav`
at the processed rate. PCM24, float, WAVE_FORMAT_EXTENSIBLE, RF64 and compressed formats are not accepted.
Metadata and artwork are not read or written.

A reference at another rate is resampled with a native rational polyphase windowed-sinc filter.
It uses a Blackman window, 64 input samples of radius (expanded for downsampling), a cutoff at
96% of the lower Nyquist, and normalized phase coefficients. It adds no timeline delay. Same-rate
reference samples pass through directly before reference gain. Audio outside the reference is zero.
This resampler is independent of any resampler previously used to prepare DSEE's input, so using that
actual input WAV as reference is the most reproducible choice.

## Controls

| Option | Default | Meaning |
| --- | --- | --- |
| `--mode` / `--postprocess` | `mix` / `none` | `none`, `adaptive`, `mix`, `combined` |
| `--mix` | `0.5` | Fixed processed amplitude weight, 0..1, or `auto` |
| `--auto-mix-objective` | `seam` | Prefer local continuity, or `envelope` for the original upper-bound policy |
| `--auto-mix-min` / `--auto-mix-max` | `0` / `1` | Bounds for automatic selection |
| `--auto-mix-fallback` | `0.5` | Fallback when evidence is insufficient, clamped to bounds |
| `--auto-mix-margin-db` | `2` | Allowed band-power excess over reference/envelope, 0..12 dB |
| `--auto-mix-coverage` | `0.9` | Required fraction of reliable windows meeting constraints, 0.5..1 |
| `--curve` | `cosine` | `linear` or complementary raised cosine |
| `--crossover-low` / `--crossover-high` | `18000` / `22000` | Mixing transition, Hz |
| `--envelope-strength` | `1` | Attenuation scale in dB, 0..1 |
| `--max-cut-db` | `12` | Maximum adaptive attenuation, 0..60 dB |
| `--fit-low` / `--fit-high` | `15000` / `19500` | Reference bands used to fit roll-off, Hz |
| `--envelope-low` / `--envelope-high` | `18500` / `22000` | Adaptive attenuation fade-in, Hz |
| `--smoothing-ms` | `150` | Gaussian time-smoothing sigma, 0..5000 ms |
| `--reference-gain-db` | `0` | Reference gain before analysis and mixing, -120..24 dB |
| `--reference-delay` | `auto` | Signed delay in output-rate samples, within +/-10 seconds |
| `--alignment-start` | `0` | Start of the up-to-eight-second alignment excerpt, seconds |

Frequency intervals must increase. The envelope fit must span at least 1500 Hz. Active intervals
must end below the processed Nyquist; the fit must also end below the reference's original Nyquist.
Non-finite numbers and unknown/duplicate CLI options are rejected.

If a separate reference is the original signal and DSEE's input was lowered by 3 dB, specify
`--reference-gain-db -3`. When the reference is already that attenuated input, leave gain at 0.
No gain is inferred from filenames, and the processed input is not normalized automatically.

### Complementary mixing

For a transition `t` from zero to one between the crossover endpoints, the linear curve is `t`,
and the cosine curve is `(1 - cos(pi*t))/2`. With maximum weight `m`:

```text
processed_weight = m * transition
reference_weight = 1 - processed_weight
output = reference_weight * aligned_reference + processed_weight * processed_branch
```

Both channels share weights. Below the transition only the aligned reference is used. At `--mix 0.5`,
the upper band contains 50% reference and 50% processed amplitude; where the reference has negligible
energy, the generated contribution is about 6 dB lower. At `--mix 1`, the upper band uses only the
processed branch. `--mix 0` selects the aligned reference, while `--mode none` preserves processed PCM.

These are amplitude weights, not power percentages. Common delay correction does not compensate for
arbitrary frequency-dependent phase differences. In `combined` mode the adaptive gain is applied to
the processed branch before mixing, in one synthesis pass.

### Automatic per-track mix (experimental)

Use `--mix auto` with `mix` or `combined` mode in either CLI entry point:

```sh
build/dsee-compat postprocess --input dsee.wav --reference input-48k.wav \
  --output automatic.wav --mode mix --mix auto \
  --auto-mix-min 0 --auto-mix-max 1 --auto-mix-margin-db 2
```

The result is one fixed amplitude weight for the entire track, shared by both channels. The default
`seam` objective prioritizes crossover continuity. `--auto-mix-objective envelope` reproduces the
initial envelope-limited selector. The decision is
estimated after reference gain/delay correction and, in combined mode, after the configured adaptive
envelope. Ordinary numeric `--mix` follows the original path. Automatic controls require `--mix auto`;
automatic mixing currently requires a crossover starting at 12 kHz or higher.

The estimator samples up to 2,048 evenly distributed STFT windows (roughly every eight hops for shorter
files). Reference windows below -70 dBFS or 30 dB below the 80th-percentile window power are excluded.
At least one second of audio and eight active windows are required. The windows contain the full
complex spectra of both channels.

A 4 kHz reference fitting interval is searched downward on the 500 Hz grid. Its upper band center
cannot exceed the lowest of 20 kHz, reference Nyquist minus 1 kHz, and crossover start plus 1 kHz.
Its upper center must reach at least 12 kHz. Adjacent downward jumps above 10 dB, band densities below
-130 dBFS/Hz, weak fit energy and large fitting residuals are rejected. This moves the fit below sharp
bandwidth cutoffs where enough evidence exists. A median pairwise slope estimates roll-off; accepted
raw slopes are -60..6 dB/octave and extrapolation clamps them to -48..0 dB/octave. The fit is a signal
heuristic, not knowledge of the missing high-resolution recording.

The track slope is combined with a per-window anchor. Windows with fit-band energy below -80 dBFS or
median fitting error above 6 dB are excluded. At least eight reliable windows and one quarter of the
active windows must remain. Analysis covers approximately 2 kHz groups from the crossover start to
12 kHz beyond its upper endpoint, limited by output Nyquist. Upper targets use the greater of the
reference band power and extrapolated envelope, plus the configured margin, with a -110 dBFS floor
per 500 Hz band. Unpopulated groups are ignored.

For each group, mixing power is evaluated as a quadratic in the candidate weight using the actual
complex reference and processed spectra. Cross-power terms therefore include phase cancellation.
The `envelope` objective selects the greatest admissible weight that meets the upper energy target
and avoids passing through an overlap dip more than 3 dB below the reference. A lower quantile of
window limits provides the configured statistical coverage. This decision is also retained as the
baseline for `seam` mode.

The default `seam` objective then compares the actual mixed band powers at 500 Hz resolution from
1 kHz below the crossover start to 2 kHz above its end (ending at least 1 kHz below output Nyquist).
Reliable windows are split chronologically into up to eight sections, each containing at least eight
windows. Powers are normalized by reference window energy before aggregation. Sections without a
usable reference anchor immediately below the crossover are excluded; if no section remains, the
envelope result is retained with `seam_unavailable_used_envelope`.

For each section, the shape score is the RMS second difference of log band power plus one quarter of
the largest adjacent-band step. The track score averages the mean and 90th-percentile section scores.
A straight spectral slope is allowed; abrupt corners and notches are penalized. A separate 500 Hz
phase-cancellation guard limits candidates that would pass through a dip more than 3 dB below the
reference, using the configured window coverage. Candidate weights include the envelope baseline,
user bounds, phase limit, and a 0.01 grid. Among candidates within 0.02 score units of the best, the one
closest to the envelope baseline is selected. This uses the envelope to break near ties rather than
forcing a visibly steeper crossover to satisfy a distant high-band level prediction.

JSON reports `effective_mix` and an `auto_mix` record: fallback/reason, analyzed/active/reliable window
counts, fit interval, slope, fitting error and `achieved_coverage`. That coverage always describes the
original envelope constraint and may be below the requested value when seam continuity takes priority.
Additional optional fields are `envelope_mix`, `phase_limit`, `seam_score_before` and `seam_score_after`.
The latter compare the baseline and selected mix under the same shape score, not a perceptual score.
A normal seam selection reports `seam_continuity`. An explicit minimum can override a guard and is
reported as `minimum_overrides_constraint` or `minimum_overrides_phase_guard`.

Short, silent, narrow-band or irregular material may instead report a fallback. Automatic delay
alignment has its own requirements; an explicit delay is needed for silence/short clips even when mix
selection will fall back. Empty audio reports the configured fallback without analysis. Numeric
`--mix` remains a fixed, manually chosen weight, and the estimator does not restore loudness.

The existing `--fit-low`, `--fit-high` and envelope controls still govern adaptive attenuation, while
automatic mixing searches its own interval. In C++, set `PostprocessOptions::auto_mix`; inspect
`PostprocessResult::effective_mix` and its optional `AutoMixAnalysis`. Select `AutoMixObjective::Seam`
(default) or `AutoMixObjective::Envelope` in `PostprocessOptions`. Parameter defaults are trial
settings to calibrate by loudness-matched listening. Loudness restoration/limiting remains a separate
operation, and this option does not change the crossover endpoints automatically.

### Adaptive envelope

Stereo-averaged 500 Hz band powers from aligned reference and processed audio are smoothed in time.
A median of pairwise slopes over the reference fit bands estimates roll-off, constrained to
-48..-12 dB/octave. The reference envelope is extrapolated with a 2 dB margin. Excess processed power
is attenuated, limited by `--max-cut-db`; no positive spectral gain is applied.

Frequency smoothing has a 750 Hz sigma. Gain time smoothing uses 0.8 times `--smoothing-ms`; slope
smoothing uses 5/3 times it. Confidence fades between -90 and -70 dBFS of reference fit-band power,
so a silent reference does not impose an arbitrary high-frequency envelope. Both channels use the
same gain. Zero smoothing disables time smoothing, while frequency smoothing remains in place.

The periodic Hann STFT uses 2048/4096/8192 samples at 48/96/192 kHz, with 75% overlap (about 42.7 ms
per window). Center padding and normalized overlap-add preserve the exact processed frame count,
including short files, initial delay and tail. These controls are listening adjustments rather than
an estimate of an unavailable high-resolution master. This native implementation does not claim
bit identity with earlier experimental Python processing or its external resampler.

## Alignment and failure behavior

Automatic alignment searches +/-100 ms, low-passing the selected excerpt at 8-10 kHz and choosing
the reference channel with greater energy. Anti-phase stereo therefore does not disappear through
mono summing. At least 250 ms is required. Silent excerpts, a search-boundary result or low-band
correlation below 0.99 fail with a diagnostic. Choose another `--alignment-start` or provide a known
`--reference-delay`. Periodic tones can give ambiguous delays even with high correlation; use a
measured explicit delay for those. Silence also requires an explicit delay in an active mode.

Positive delay shifts the reference later; negative delay advances it. Reference padding/truncation
matches the processed duration. Empty processed audio produces an empty WAV without estimating delay.

Existing outputs, including symlinks, are refused. WAV output is staged beside its destination and
published atomically without replacement after success. Non-finite samples, I/O errors and PCM32
clipping fail the operation and remove the incomplete output. No limiter is silently applied.

Audio transforms use bounded windows. Adaptive mode stores per-frame band measurements in memory,
so memory grows with duration. Automatic alignment uses at most eight seconds. Integrated `process`
uses one anonymous temporary PCM file for its native result, removed on success or failure; standalone
`postprocess` needs only its pending output WAV. RIFF limits output to less than 4 GiB (about 93 minutes
at 96 kHz). There are no model downloads, dependency caches or persistent audio caches.

## C++ use and validation

Include `dsee_postprocess.hpp` and link `libdsee_postprocess.a` plus the C++ standard library. Implement
`AudioSource` as a stable, seekable stereo source, configure `PostprocessOptions`, and call
`dsee::postprocess(processed, reference, options, sink)`. Default options bypass DSP. The sink receives
interleaved PCM32 frames; its buffer is valid only during the callback. Errors throw standard C++
exceptions, and the sink may already have received a prefix before a later failure. File consumers
should publish only after success. Calls with distinct sources and sinks are independent.

The default CTest suite exercises native reconstruction/boundaries at all three rates, measured
crossover responses, signed and automatic delay (including anti-phase stereo), reference resampling
and anti-aliasing, envelope bounds/strength/stereo linkage, exact bypass and error propagation.
The standard-library Python CLI driver also verifies WAV formats, atomic cleanup and operation with
an empty executable search path. No Sony resources are used in those tests. Optional local integration
checks compare both entry points at all three output rates against the same native output.
