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
