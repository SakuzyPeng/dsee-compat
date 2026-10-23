# Experimental AArch64 Linux and x86-64 QEMU

The runtime can be built as an **AArch64 GNU/Linux executable** and executed on an
x86-64 Linux host with `qemu-aarch64`. Windows uses the same route inside WSL2.
This does not produce a native x86 library or Windows executable. The supported
Sony resources and their exact size/SHA-256 checks remain unchanged.

This path is opt-in and experimental. Its output is not universally bit-identical
to the macOS reference; see the numerical comparison below. The default macOS
build and its original-reference tests remain supported.

## Build and run

In an AArch64 Linux environment, use GCC/C++17, CMake 3.20+, OpenSSL development
headers/libraries, and Python 3 for the tests. The verified environment is Debian
12 with glibc 2.36, GCC 12.2 and OpenSSL 3.0.20. Reuse existing toolchains and keep
this target's outputs beneath the existing build directory:

```sh
cmake -S . -B build/linux-arm64 -DCMAKE_BUILD_TYPE=Release \
  -DDSEE_EXPERIMENTAL_LINUX=ON
cmake --build build/linux-arm64 -j 4
ctest --test-dir build/linux-arm64 --output-on-failure
```

On x86-64 Linux, install the distribution's QEMU user-mode package. Supply an
AArch64 sysroot containing the matching dynamic loader, libc, libm, libstdc++,
libgcc and libcrypto. `-L` points to this sysroot, **not** the Sony resource root:

```sh
qemu-aarch64 -L /path/to/arm64-sysroot build/linux-arm64/dsee-compat \
  inspect --assets /path/to/external-resources

qemu-aarch64 -L /path/to/arm64-sysroot build/linux-arm64/dsee-compat \
  process --assets /path/to/external-resources \
  --input input.wav --output output.wav --output-rate 96000
```

Cross-compilation is also possible with an existing AArch64 Linux toolchain and
its matching OpenSSL/sysroot. Simply passing the opt-in flag to an x86 compiler
does not make a working runtime. QEMU executes the AArch64 loader, compatibility
shims and external library together in one guest process.

An AArch64 Linux C API consumer also needs OpenSSL Crypto and the platform's
dynamic-loader libraries, in addition to the C++ runtime. The CMake target links
these dependencies automatically.

Resources stay in an explicitly supplied external directory. Do not include them
in a container image, sysroot, binary bundle, repository or hosted CI job.

## Compatibility changes

- Linux uses OpenSSL SHA-256 and `fopencookie`; macOS keeps CommonCrypto and
  `funopen`. Both memory readers operate on already verified bytes.
- The pinned wrapper's nominal 110,000-byte state allocation is too short for
  some of its accesses. The verified macOS allocator actually supplies 114,688
  bytes. The shim now explicitly reserves this 112 KiB state arena on both
  platforms; other allocation sizes keep their requested bounds. This fixes
  Linux heap corruption without modifying the original library or its hash.
- API stress tests normalize Linux's `ru_maxrss` from KiB to bytes. The optional
  `api_test RESOURCE_ROOT instrumented` mode runs all behavioral checks while
  omitting the process-RSS limit, because emulator and memory-checker translation
  caches contribute to that measurement. It is not a memory-leak test substitute.

## Reference comparisons

On 2026-10-22, the macOS regression still matched **36/36 original-wrapper
prefix hashes**, and all 24 integrated/standalone postprocessing comparisons
passed. Native AArch64 Linux passed the default 5/5 CTest suite, the API stress
suite, and 24 integrated/standalone postprocessing comparisons.

Identical synthetic input bytes were processed on macOS and native AArch64 Linux
across silence, impulse, tone and noise, three source profiles and three output
rates. Frame counts matched in all 36 cases. Linux matched **18/36** original
prefix hashes exactly. Across complete outputs, the largest absolute difference
was 1,680 PCM32 integer units (about -122.13 dBFS); the largest per-case RMS
difference was about -154.61 dBFS. These are numerical measurements, not a
perceptual equivalence claim. The precise source of the platform difference has
not been isolated; it already exists without QEMU.

The full Linux API stress suite also passed under Valgrind Memcheck with **zero
errors and zero live heap blocks at exit**, using the explicit `instrumented`
mode. It covered all nine profile/rate combinations, tail draining, fragmented
input, concurrent-call rejection, 200 lifecycles and the 5,000-block stream.

To test the DSEE core against a deliberately selected platform baseline, the
integration script supports `--core-only` and `--reference-label`. The former
skips only the postprocessing comparisons; output collision and corrupted-resource
rejection checks still run. Use an executable wrapper around `qemu-aarch64` as
`--exe`, and identify the baseline accurately. A Linux-baseline match is not an
original-wrapper match.

`api_test RESOURCE_ROOT benchmark [BLOCKS]` measures all nine profile/rate
combinations after 64 warmup blocks. The default remains 1,440 measured blocks;
a smaller explicit count is useful for slow emulation. Report that count and
repeat count alongside results; each block represents 1,024 frames at 48 kHz.

## x86-64 validation — 2026-10-22

Host: Intel Core i7-11800H, Windows with WSL2 3.0.3, Linux
6.18.54.1-microsoft-standard-WSL2, Debian 12 x86-64 and QEMU user-mode 7.2.22.
The same AArch64 executable and system libraries used for the native Linux
comparison were executed by `qemu-aarch64`.

- The ELF/WAV unit tests, no-assets API test and C-header linkage test passed.
- The complete API stress suite passed in `instrumented` mode in about 330 s,
  including all nine tail cases, input fragmentation, concurrent-call rejection,
  200 lifecycles and the 5,000-block stream. Process peak-RSS growth was about
  46.4 MiB; its limit was deliberately skipped because it includes QEMU caches.
- **36/36 output-prefix hashes matched the native AArch64 Linux baseline**.
  Existing-output refusal and corrupt-resource rejection also passed. This run
  used `--core-only --reference-label native-ARM64-Linux`; it does not establish
  bit identity with the macOS/original-wrapper baseline.
- The translated postprocessor test was stopped after about 266 CPU seconds
  without a test assertion result, to prioritize DSEE execution. The complete
  default suite and integrated postprocessing were validated on native Linux and
  macOS, not under QEMU. The standard-C++ postprocessor can be ported to run
  natively on x86 instead of translating it with the external library.

Raw local records are `build/qemu-x86/functional.json`,
`build/qemu-x86/integration.json`, `build/qemu-x86/platform-comparison.json` and
`build/linux-arm64/memcheck.log`. Original resources were read from an external
read-only directory and supplied to the private Windows test host's temporary
RAM filesystem for execution; they were never included in a repository, runtime
bundle or container image.

## QEMU performance

Each configuration used 64 warmup blocks and 64 measured blocks (1.36533 seconds
of 48 kHz input), with three complete repeats. The deterministic 1,024-frame
noise block was reused throughout. Timing includes emulated core processing but
excludes initialization, file I/O and postprocessing. These short measurements
are a feasibility check, not a controlled comparison with the longer macOS
benchmark. The host was not isolated or pinned to a CPU, and variation between
runs was substantial. An earlier SSH-interrupted run was excluded.

Median multiples of real time, followed by the minimum–maximum range:

| Profile | 48 kHz output | 96 kHz output | 192 kHz output |
| --- | ---: | ---: | ---: |
| PCM | 0.870× (0.431–0.875) | 0.584× (0.282–0.614) | 0.525× (0.265–0.628) |
| AAC128 | 0.550× (0.342–0.581) | 0.519× (0.280–0.531) | 0.489× (0.250–0.507) |
| MP3128 | 0.584× (0.501–0.631) | 0.485× (0.477–0.540) | 0.509× (0.448–0.522) |

Every measurement was below real time on this host with this QEMU version.
At 96 kHz, the medians correspond to approximately 1.7–2.1 times the input
duration, before file I/O or postprocessing. Other CPUs, QEMU versions, workloads
and power/load conditions are unmeasured.

Raw measurements and aggregate ranges are in `build/qemu-x86/benchmark.json`
and `build/qemu-x86/benchmark-summary.json`. The temporary WSL distribution and
build containers are removed after collecting the results; the source changes,
small local build artifacts and reports remain available.
