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
