# Local validation — 2026-10-21

Host: Apple M4 Pro, macOS 27.0.1, AppleClang 21.0.0, Release build. Firmware profile: `67.1.A.2.229`. Original resources remained in an external local research directory and were not copied into this repository.

## Correctness

- Default CTest checks require no Sony resources: ELF range/overflow rejection, RELA/APS2/RELR decoding, generated WAV parsing, atomic output publication/refusal, CLI errors, C header linkage and API errors.
- **36/36 original-wrapper reference PCM prefixes matched SHA-256 exactly:** silence, impulse, tone and seeded noise; PCM/AAC128/MP3128 metadata; 48/96/192 kHz output. The CLI's additional drained tail is excluded from the historic prefix hash comparison.
- **9/9 tail comparisons:** a final-sample impulse in a 4,097-frame input was tested through arbitrary fragmentation. Automatic finish output matched the prefix of a 64-block zero-extended reference; the remaining reference output was entirely zero.
- Empty input produces empty output. Input after finish is rejected. Existing destinations are not overwritten. A same-size resource with one corrupt byte fails SHA-256 before output creation.
- Concurrent calls were rejected with `DSEE_BUSY`; a second active context was rejected.
- 200 create/destroy cycles completed with no observed increase in peak RSS in this test. A 5,000-block stream (about 106.7 seconds of input) also passed.
- A real WAV with 48,001 input frames completed through the new CLI, including partial-block padding and draining.

This establishes compatibility with the local research reference, **not** bit identity with captured Xperia hardware output. Synthetic PCM plus codec metadata is not an AAC/MP3 decoder quality evaluation. Real-time scheduling, other host models, other firmware versions and unsupported input formats remain unvalidated.

## Performance

The new C API was measured with original wrapper processing, a 64-call warmup, 1,440 measured calls (30.72 seconds of audio) and three repeats per configuration. Timing excludes file I/O and initialization. Input is a fixed deterministic 1024-frame noise block reused during the measurement; this differs from the earlier research benchmark's 64-block ring, so these figures are a regression sanity check rather than a tightly controlled comparison.

| Profile | 48 kHz output | 96 kHz output | 192 kHz output |
| --- | ---: | ---: | ---: |
| PCM | 108.3× | 79.7× | 74.5× |
| AAC128 | 59.7× | 51.2× | 48.6× |
| MP3128 | 60.1× | 51.4× | 48.8× |

Values are median multiples of real time. These are close to the earlier probe's performance; allocation tracking and API guards did not introduce a large slowdown. The system was not pinned to a CPU core or isolated from other applications.

Raw run records are local build artifacts (`build/integration.json` and `build/benchmark.json`) and are not part of the source distribution. Commands for rerunning the tests are in the README.

## Native postprocessing — 2026-10-22

The optional DSP is implemented in C++17. The default Release CTest suite passed **5/5**, including
native synthetic coverage for all three output rates, irregular/short/empty streams, PCM32 bit-exact
bypass, complementary reconstruction, crossover gains/curves, positive and negative delay, anti-phase
automatic alignment, reference resampling/anti-aliasing, envelope limits/strength/stereo linkage,
quiet references and malformed-input/error paths. CLI tests also cover output collisions, symlinks,
clipping cleanup and audio processing with no external programs on PATH. There are no optional DSP
package skips.

Local native regression passed **36/36 original-wrapper prefix hashes**, the API's nine tail cases,
fragmentation/concurrency/lifecycle/long-stream checks, and **12/12 integrated-versus-standalone WAV
comparisons** (four modes at 48/96/192 kHz). The latter compare complete output files byte for byte.
The native runtime and hash-pinned resource profile were not changed.

A full 313.6-second music recording was prepared as 48 kHz PCM16 with 3 dB headroom using an external
conversion tool solely for the test. Native DSEE produced 30,124,032 frames at 96 kHz, including its
delay and tail. The C++ postprocessor used that actual input WAV as reference and retained every frame:

| Mode | Postprocessing time | Peak resident memory | Sample peak |
| --- | ---: | ---: | ---: |
| 50% cosine mix | 7.7 s | 167 MiB | about -1.97 dBFS |
| Adaptive + 50% cosine mix | 13.9 s | 169 MiB | about -1.97 dBFS |

Both inferred a 2,182-sample delay with low-band correlation above 0.9999998 and completed without
clipping. The tests used an empty external-program search path for the postprocessing executable.
These are single-run local timings including I/O, not a controlled performance benchmark. Temporary
WAVs were removed. The local record is `build/postprocess-native-real-validation.json`; no music,
metadata, artwork or reference audio is included in the repository.

Installation and a separately compiled consumer of the installed C++ header/static library passed.
`BUILD_TESTING=OFF` was checked without Python package discovery, and the normal test configuration was
restored afterward. The executable links only the system C++ and system libraries.

UndefinedBehaviorSanitizer, bounds instrumentation and libc++ extensive hardening passed the native
synthetic suite. AddressSanitizer could not run on this host: both installed compiler runtimes hung
in ASan initialization before `main`. A process sample located the reentrant allocator lock in shadow
memory setup; the hung process was stopped. This is not an ASan test pass.

## Automatic per-track mixing — 2026-10-22

The experimental `--mix auto` path passed the default 5/5 CTest suite. Synthetic checks cover
monotonic reduction as generated high-band excess increases, unity selection for identical signals,
fixed-versus-automatic PCM identity at the selected weight, overlap cancellation, common-gain
invariance, bounded selection/override reporting, bandwidth-cutoff-aware fitting, silence/short/empty
fallbacks and option rejection. CLI tests verify the JSON selection/fallback fields.

Local integration passed 36 original-wrapper prefix hashes and 18 integrated/standalone complete-WAV
comparisons, including automatic mix and automatic combined modes at each output rate. UBSan, bounds
instrumentation and libc++ extensive hardening passed the expanded native synthetic suite.

Three full local stereo music recordings selected 0.118880, 0.690639 and 0.322487, respectively, without
fallback. Each selected a 15-19 kHz fit interval; fitted slopes were approximately -41.15, -20.13 and
-27.72 dB/octave. Reliable analysis windows numbered 1,971, 1,924 and 1,924, and measured constraint
coverage was at least 90%. All three produced 96 kHz PCM32 WAVs with unchanged processed frame counts.

For listening comparisons only, separate external gain/limiting restored original integrated loudness
within the measurement's 0.1 LU resolution. Final measured true peaks were -0.2 dBTP. These adjustments
are not part of `--mix auto`. Temporary input/intermediate WAVs were removed; the local processing
record is `build/user-batch-automix-2026-10-22.json`. The selected weights and spectral criteria have
not been validated as perceptual optima; loudness-matched comparison is still required.

## Crossover-continuity selection — 2026-10-22

The revised default automatic objective prioritizes local spectral shape; the original policy remains
available as `--auto-mix-objective envelope`. The default 5/5 CTest suite passed, including new cases
where a distant envelope ceiling would deepen a local crossover seam, excessive generated energy
must lower the selected mix, and phase cancellation still limits it. Selected fixed-weight PCM,
common-gain invariance and old-policy reproduction are checked. Local integration passed 36 native
reference hashes and 24 integrated/standalone comparisons across three rates and both objectives.
UBSan, bounds instrumentation and libc++ extensive hardening also passed the synthetic suite.

The same three local recordings selected 0.32, 0.99 and 0.95 (initial envelope-policy choices:
0.118880, 0.690639 and 0.322487). Their section-based seam scores decreased from approximately
2.839 to 1.825, 4.012 to 3.154 and 3.875 to 2.150, respectively. The original envelope coverage fell
below 90%, explicitly reported rather than treated as a satisfied upper bound. This reflects the
change in objective, not stronger evidence for the missing original high-frequency content.

New 96 kHz PCM32 listening files retained frame counts, were restored to within the 0.1 LU measurement
resolution of original loudness using separate gain/limiting, and measured -0.2 dBTP. Temporary WAVs
were removed. Local records: `build/user-batch-automix-seam-2026-10-22.json` and
`build/pray-auto-seam-comparison.json`. These measurements support the intended change in spectral
continuity; they do not establish a perceptually optimal mix across other material.

## Experimental Linux / x86-64 QEMU — 2026-10-22

An opt-in AArch64 GNU/Linux runtime was built with GCC 12.2, glibc 2.36 and
OpenSSL 3.0.20. Both native macOS and native Linux default CTest suites passed
5/5. The pinned state allocation now explicitly reserves 112 KiB, correcting
accesses beyond the wrapper's nominal 110,000-byte request that macOS allocator
rounding had masked. macOS still matches all 36 original-wrapper prefixes and
all 24 integrated/standalone postprocessing comparisons.

The Linux API stress suite passed Valgrind Memcheck with zero errors and no live
heap allocations at exit. On an Intel Core i7-11800H under WSL2 and QEMU 7.2.22,
the complete API stress suite and 36 core output-prefix comparisons against
native AArch64 Linux passed. The instrumented API runs omit the process-RSS
growth limit because it includes translator caches.

Linux matches only 18/36 original macOS reference prefixes bit for bit; the
largest observed full-output difference is about -122.13 dBFS. This difference
exists before QEMU is introduced. Full translated postprocessing was not
validated. Build commands, exact scope, measurements and local report names are
in [the QEMU validation notes](QEMU.md).

## Unicorn probe — 2026-10-22

The standalone C++ probe uses Unicorn 2.1.4 with native import bridges, a bounded
guest heap, emulated TLS and the shared resource profile. It was built with
AppleClang on macOS ARM64 and MSVC 14.51 on Windows x86-64. Both resource-free
probe self-tests passed; the default native suite passed 5/5. The signed APS2
addend overflow checks now also have explicit boundary coverage. Native macOS
integration still passed 36 original-reference and 24 postprocessing comparisons.

Mac Unicorn output matched all 36 complete native C API outputs, including
drained tails. Windows completed all 36 cases with identical input hashes and
frame counts; 21 complete PCM outputs were bit-identical to macOS. The largest
absolute difference was 1,680 PCM32 units (about -122.13 dBFS), and the largest
per-case RMS difference was about -154.61 dBFS. All five individually corrupted
resources were refused before emulator initialization on Windows.

On the same Intel Core i7-11800H used for QEMU, three repeats of 64 measured
blocks after 64 warmup blocks produced median throughputs of 0.212–0.438 times
real time. This prototype was slower than the previous QEMU implementation;
neither comparison controlled host load or power state. The probe remains
separate from the public C API and WAV CLI. Full configuration results, build
commands and scope are in [the Unicorn notes](UNICORN.md).

## Dynarmic probe — 2026-10-22

Dynarmic 6.7.0 snapshot `a41c380246d3d9f9874f0f792d234dc0cc17c180` was built for
native Windows x86-64 with the A64 frontend, safe optimizations and checked page
table mappings. The shared-harness Unicorn regression reproduced all 36 earlier
complete Mac outputs. Both engine self-tests passed, including non-executable
data and a store crossing into a protected page; the default native suite passed
5/5. The five individually corrupted resources were rejected before execution.

All 36 Dynarmic complete PCM hashes matched the prior Windows Unicorn results,
with identical inputs and frame counts. The same 21 complete outputs are
bit-identical to native macOS; the remaining errors are consequently the same
previously measured maximum of 1,680 PCM32 units, about -122.13 dBFS. No fallback
engine is linked or invoked.

On the Intel Core i7-11800H, three extended repeats with 64 warmup and 256 measured
blocks gave medians of 2.33–5.92 times real time. Matching 64-block short tests
were 4.6–6.7 times the earlier QEMU throughput and 10.4–13.6 times the earlier
Unicorn throughput. This validates average core throughput on the fixtures,
not live playback or deadline guarantees. Build details, per-configuration
results and local report names are in [the Dynarmic notes](DYNARMIC.md).

## Integrated Windows Dynarmic runtime — 2026-10-22

The Windows x86-64 static C API and WAV CLI now share the same emulation core as
the diagnostic probe. Apple Silicon retains its native execution backend and
the original public ABI. Windows was built with MSVC 19.51 and the pinned Dynarmic
6.7.0 snapshot on the Intel Core i7-11800H host. These are local validation results.

- Windows default CTest: **8/8**, including authored Dynarmic memory tests,
  synthetic ELF/WAV tests, sparse offsets beyond 2 GiB, Unicode paths, output
  races/refusal, C headers, postprocessing, injected backend failures and an installed C client.
- macOS default CTest: **7/7**. Its resource-backed regression retained all
  **36 original-wrapper prefix matches** and **24 integrated/standalone comparisons**.
  The refactored native C API and shared-core Unicorn probe each reproduced
  **36/36 complete pre-integration Mac outputs**, including drained tails.
- Windows C API and WAV CLI each reproduced **36/36 complete pre-integration
  Windows Dynarmic outputs**, with identical input hashes, PCM hashes, output
  frames and tail-block counts. API verification also compared fragmented input.
  The Windows baseline already matched Unicorn in full; no extra tolerance was introduced.
- Windows passed **24** integrated/standalone postprocessing comparisons,
  **five** corrupted-resource rejections, empty-input handling and selected-resource-only
  creation. Resource and WAV paths included Unicode and spaces.
- The full API stress suite passed fragmentation, tails in nine configurations,
  concurrent-entry rejection, 200 create/destroy cycles and 5,000 processing blocks.
  Windows private-memory growth over the lifecycle check was **598,016 bytes**
  (about **0.57 MiB**, below 16 MiB); the macOS run reported no additional peak RSS.

### C API throughput and latency

Each configuration used 64 warmup blocks and three repeats of 256 measured blocks
(5.461333 seconds of input). The timing includes public API buffering, guest
execution, imports and output copies. Initialization and first-block timings are
reported separately. File I/O and offline postprocessing are excluded. No CPU affinity,
power-state or background-load isolation was applied.

| Profile | Output | Median real-time factor | Median initialization | Median first block | Median P99 block | Largest observed block |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| PCM | 48 kHz | 5.674× | 53.43 ms | 118.61 ms | 5.67 ms | 5.91 ms |
| PCM | 96 kHz | 3.216× | 52.59 ms | 129.80 ms | 8.44 ms | 9.48 ms |
| PCM | 192 kHz | 2.989× | 54.49 ms | 144.75 ms | 10.61 ms | 11.38 ms |
| AAC128 | 48 kHz | 3.677× | 56.84 ms | 166.40 ms | 8.35 ms | 8.63 ms |
| AAC128 | 96 kHz | 2.698× | 75.21 ms | 191.50 ms | 11.64 ms | 12.66 ms |
| AAC128 | 192 kHz | 2.411× | 57.88 ms | 157.73 ms | 13.32 ms | 13.41 ms |
| MP3128 | 48 kHz | 3.733× | 58.23 ms | 175.63 ms | 8.35 ms | 8.52 ms |
| MP3128 | 96 kHz | 2.692× | 57.94 ms | 156.43 ms | 11.15 ms | 12.47 ms |
| MP3128 | 192 kHz | 2.306× | 55.71 ms | 147.25 ms | 13.20 ms | 14.13 ms |

All nine median throughputs exceeded real time (**2.31–5.67×**); all 27 individual
samples also exceeded real time (minimum **2.276×**). Each percentile column
summarizes the three per-repeat measurements rather than pooling samples.
Full P50/P95/P99/max measurements are retained in `build/windows/benchmark.jsonl`.
Cold first-block execution took much longer than a steady block, so these results
do not establish a live-playback deadline guarantee. Real music and live playback
remain outside this first integration acceptance run.

Local reports are in `build/windows`: `integration.json`, `api-verify.jsonl`,
`api-lifecycle.jsonl`, `benchmark.jsonl`, `benchmark-summary.json`, `host.json`,
`mac-integration.json`, `mac-api-comparison.json` and `mac-unicorn-verify.jsonl`.
Original resources were supplied through explicit external directories. Windows
resource staging and corrupt fixtures were outside the repository/build tree.

### Thirty-minute stream and cleanup

The Windows MP3128/192 kHz run processed **84,375 measured blocks**, representing
**1,800 seconds** of changing deterministic noise, after 64 warmup blocks. It completed
in **763.14 seconds** (about **2.36× real time**), with all measured blocks producing
nonzero PCM and the expected frame counts. The final tail drained in **eight blocks**.
Peak sampled private-memory growth after warmup was **0 bytes**, below the 16 MiB limit.
Memory was sampled every 4,096 blocks and at completion; PCM was checked and checksummed
in memory rather than retained. The recorded rolling checksum is `7111313578768470239`.

The temporary external Windows resource tree, including corruption fixtures, was removed
after all validation completed. `long-stream.jsonl` and `validation-status.json` record
the run and cleanup. The AArch64 Linux build remains experimental; it was not rerun
as part of this Windows integration.
