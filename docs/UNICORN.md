# Experimental Unicorn probe

`dsee-unicorn-probe` runs the supported AArch64 library with Unicorn 2.1.4 inside
a native host process. It was tested on Apple Silicon macOS and native Windows
x86-64. Windows does not need WSL for this probe.

This is a diagnostic program for synthetic fixtures and benchmarks, **not** a
Windows implementation of the public C API or the full WAV CLI. It does not
implement the CLI's arbitrary input streaming or postprocessing commands.

## Build and use

Supply an existing Unicorn installation's C headers and native library. The
tested dependency was the official Unicorn 2.1.4 PyPI wheel for each platform;
its SHA-256 was checked against PyPI metadata. No dependency is downloaded by
CMake, and no third-party implementation is vendored here.

For example, in a Windows developer shell with MSVC and the Windows SDK:

```powershell
cmake -S . -B build/unicorn/native -A x64 -DDSEE_UNICORN_PROBE_ONLY=ON -DUNICORN_INCLUDE_DIR=C:/path/to/unicorn/include -DUNICORN_LIBRARY=C:/path/to/unicorn/lib/unicorn.lib
cmake --build build/unicorn/native --config Release
ctest --test-dir build/unicorn/native -C Release --output-on-failure

build/unicorn/native/Release/dsee-unicorn-probe.exe C:/external/resource-root verify
build/unicorn/native/Release/dsee-unicorn-probe.exe C:/external/resource-root benchmark 64 3
```

On macOS, use the host architecture's Unicorn `.dylib`, omit `-A x64`, and set
`CMAKE_BUILD_TYPE=Release`. The target uses CommonCrypto on macOS and BCrypt on
Windows for SHA-256. The Linux build branch uses OpenSSL, but this probe's Linux
build has not been validated. The ordinary project build remains unchanged when
`DSEE_UNICORN_PROBE_ONLY` is off.

- `--self-test` needs no original resources. It exercises an authored AArch64
  arithmetic/return fixture, import dispatch, invalid guest pointers, guest code
  write protection, blocked/unknown imports, allocation tracking and SHA-256.
- `RESOURCE_ROOT verify` generates silence, impulse, 1 kHz tone and deterministic
  noise. Each case processes 48 blocks of 1,024 input frames at 48 kHz, then
  drains to four consecutive zero output blocks with the same 48-block limit.
  All three source profiles and output rates are covered. JSON lines contain
  input hashes, output-prefix hashes, complete output hashes and frame counts.
- `RESOURCE_ROOT verify NEW_DIRECTORY` additionally saves the **synthetic output
  PCM** for numerical comparison. The directory must not already exist. This
  does not save original resources or an adapted executable image.
- `RESOURCE_ROOT benchmark BLOCKS REPEATS` uses 64 warmup blocks, followed by the
  requested number of measured blocks. Defaults are 64 measured blocks and three
  repeats. It reuses the same deterministic input block as `api_test`.

The private-host tests used `--stdin-assets` instead of a Windows resource copy.
This bounded local test transport accepts `DSEEAS01`, followed by the five
resources in shared-profile order (library, DSX, PCM, AAC128, MP3128), each with a
little-endian 32-bit size. Exact sizes and SHA-256 values are checked before an
emulator is constructed; trailing bytes are rejected. The sender read an
explicit external directory and transmitted bytes over authenticated SSH. No
resource payload was written into a repository, Windows workspace or image.
The probe contains no downloader or firmware URL.

## Execution model

The probe reuses the repository's ELF parser and the exact resource profile in
`src/assets.hpp`. The normal runtime reads that same profile. The parser-only
build excludes the native `mmap` image, allowing the probe to compile with MSVC;
signed APS2 addend overflow checks use portable C++ and have boundary tests.

The common harness is now `tools/emulation_probe.cpp`, with the Unicorn binding
in `tools/cpu_unicorn.cpp`. The [Dynarmic probe](DYNARMIC.md) shares the loader,
host imports and fixtures. After this extraction, Unicorn reproduced all 36
previous complete Mac output hashes; the benchmark figures below describe the
original recorded run.

Only the external library's ARM64 code is translated. Memory allocation, file
reads, PCM conversion and imported mathematical functions are implemented in
native C++ callbacks. There are no Python callbacks in the audio path. A code
hook is restricted to the import-stub address range; algorithm instructions do
not invoke a per-instruction host callback.

Guest memory uses `uc_mem_map_ptr` with checked host bridges, separate stack and
heap arenas, and guest permissions. The heap is bounded to 16 MiB for this pinned
profile. ELF permissions reject writable/executable pages. The 112 KiB state
allocation allowance discovered during the Linux port is preserved. The 43 TLS
reads are counted, while an emulated `TPIDR_EL0` supplies the private TLS page.
Unexpected imports fail at load or invocation. This remains a local experiment,
not a general Android runtime or a claimed security sandbox.

## Validation — 2026-10-22

- Both macOS and Windows passed the resource-free Unicorn self-test.
- The default native macOS CTest suite passed 5/5, including the added signed
  APS2 overflow/underflow cases. Native integration still matched all 36 original
  wrapper references and all 24 integrated/standalone postprocessing comparisons.
- On macOS, Unicorn matched **36/36 original prefix hashes**. A separate check
  confirmed **36/36 complete PCM outputs matched the native C API**, including
  drained tails and frame counts.
- On Windows, all 36 cases initialized, consumed complete blocks and drained
  successfully. Their input hashes and output frame counts matched macOS.
  **21/36 output prefixes matched the macOS original references bit for bit**;
  19/36 matched the earlier native ARM64 Linux reference. These counts must not
  be presented as 36 original-reference matches.
- Complete PCM comparison, including drained tails, also gave **21/36 exact
  matches** between Windows and the native macOS C API. The largest absolute
  difference was 1,680 PCM32 integer units (about -122.13 dBFS); the largest
  per-case RMS difference was about -154.61 dBFS. Input hashes and complete
  output frame counts agreed in every case. A second Windows verification run
  with synthetic PCM export reproduced all 36 initial output hashes exactly.
- One-byte corruption of each of the five resources was rejected on Windows
  before emulator initialization. An in-memory corruption test also passed on
  macOS. No corrupt resource was saved to the repository.

The cross-platform difference has not been isolated to a particular operation.
Imported mathematical functions run through the host C++/C runtime, which is a
candidate explanation, not a demonstrated cause. Numerical equality with the
reference and perceptual quality are separate questions.

## Windows x86-64 performance

Host: Intel Core i7-11800H; native Windows x64 executable (PE machine `0x8664`),
MSVC 14.51, `/O2 /fp:strict`, Unicorn 2.1.4. No WSL or additional CPU translator
was used. Each of three repeats used 64 warmup and 64 measured blocks per
configuration, representing 1.36533 seconds of input for the timed portion.
The timer includes emulation, host bridges, copying input into the guest and
per-block state checks. It excludes resource validation, initialization, hashes,
file I/O and postprocessing.

Median multiples of real time (1× is real time):

| Profile | 48 kHz output | 96 kHz output | 192 kHz output |
| --- | ---: | ---: | ---: |
| PCM | 0.438× | 0.293× | 0.254× |
| AAC128 | 0.291× | 0.247× | 0.213× |
| MP3128 | 0.288× | 0.241× | 0.212× |

All measurements were below real time. These medians are approximately
42–53% of the [previous QEMU medians](QEMU.md) for the same configuration on the
same physical host. This is a comparison of two tested implementations, not an
isolated emulator-engine benchmark: the engine versions, host OS/runtime, import
handling and execution paths differ. Neither run controlled CPU affinity,
background load or power conditions. It does not establish a speed ceiling for
a different or optimized Unicorn integration.

Local records are in `build/unicorn/`: `benchmark.jsonl`,
`benchmark-summary.json`, `windows-verification-summary.json`,
`mac-full-native-comparison.json`, `numerical-comparison.json`,
`native-regression.json` and `pcm-status.json`.
The generated PCM is temporary comparison data; it is removed after recording
the measurements. Original library and parameter bytes remain external.
