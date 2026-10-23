# Dynarmic Windows runtime and probe

Windows x86-64 now uses Dynarmic through the public streaming C API and the WAV CLI.
The integrated runtime passed complete-output regression and a 30-minute stream, with
**2.31–5.67×** median real-time throughput across the nine tested configurations.
Apple Silicon retains native execution. Both use the same buffering, tail handling,
resource-validation and single-instance rules. The standalone probe remains available
for diagnostics and shares its execution core with production.

## Implementation and dependency

The dependency is [Dynarmic](https://github.com/lioncash/dynarmic), at commit
`a41c380246d3d9f9874f0f792d234dc0cc17c180`, reporting version 6.7.0.
Only its A64 frontend is needed. No dependency source is vendored in this repository.
Dynarmic is 0BSD; its dependencies retain their own licenses.

`src/runtime.cpp` owns the platform-independent C API state machine. The internal
backend interface separates native execution from `src/emulation.cpp`, whose loader,
bridges and guest allocator are shared by the integrated runtime and both probes.
`src/cpu_dynarmic.cpp` implements the CPU interface; Unicorn remains a probe-only backend.
Resource loading is shared, using CommonCrypto, BCrypt or OpenSSL for SHA-256.

The integrated backend retains the measured probe configuration:

- Safe optimizations; no reduced-accuracy FP or unsafe options.
- A checked 31-bit page table, with direct mappings only for readable/writable pages;
  protected pages use callbacks, crossing-page misalignment is checked, mirroring is disabled,
  and instruction fetch requires execute permission.
- Memory-abort checks, a 32 MiB code cache, 16 MiB guest heap and the pinned 112 KiB state allowance.
- Emulated `TPIDR_EL0`, the 43-TLS-read profile check, and authored SVC/RET import stubs.
- Native C++ imports, no Python audio callbacks, no fastmem arena and no fallback emulator.

Constructor, process and destructor guest failures become `DSEE_PROCESS_ERROR`.
A processing fault poisons the context; subsequent process/finish calls return `DSEE_STATE_ERROR`.
Host memory is released even if guest destruction fails. Callback faults are captured without
allocating error strings or throwing through JIT frames. This is a narrow compatibility runtime,
not an Android OS emulator or a security sandbox.

## Integrated Windows build

Use an x64 MSVC developer environment with CMake, Ninja, Windows SDK and an installed
Dynarmic package from the pinned revision. The tested compiler is MSVC 19.51.
C++20 is private to the internal emulation target; public interfaces and DSP remain C++17.
CMake does not fetch dependencies or original resources.

```powershell
cmake -S . -B build/windows/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DDSEE_BACKEND=AUTO -DCMAKE_PREFIX_PATH=C:/deps/dynarmic
cmake --build build/windows/native -j 4
ctest --test-dir build/windows/native --output-on-failure
cmake --install build/windows/native --prefix C:/apps/dsee-compat

build/windows/native/dsee-compat.exe inspect --assets C:/external/resource-root
build/windows/native/dsee-compat.exe process --assets C:/external/resource-root --input input.wav --output output.wav --source-profile mp3128 --output-rate 192000
```

`AUTO` chooses Dynarmic on Windows x86-64 and native execution on Apple Silicon.
`DSEE_BACKEND=DYNARMIC` selects it explicitly; unsupported platform/backend combinations fail configuration.
`BUILD_TESTING=OFF` removes the Python development-test requirement.
The CLI accepts Unicode paths. C API resource-root strings must be UTF-8.
The runtime validates selected resource hashes before constructing the engine; `inspect` checks all five.

Installation exports `dsee::dsee_compat` and `dsee::dsee_postprocess` via
`find_package(dsee_compat CONFIG REQUIRED)`, including internal archive and system link dependencies.
Windows consumers must also make the Dynarmic package and its Boost headers discoverable.
Enable C++ for linking a C client, as shown in the README. The installed-client CTest
builds and runs a separate C translation unit without original resources.

### Preparing the dependency

Build the pinned source using the existing toolchain and a directory under `build/`:

```text
BUILD_SHARED_LIBS=OFF
DYNARMIC_FRONTENDS=A64
DYNARMIC_TESTS=OFF
DYNARMIC_TESTS_USE_UNICORN=OFF
DYNARMIC_USE_BUNDLED_EXTERNALS=ON
DYNARMIC_USE_PRECOMPILED_HEADERS=OFF
DYNARMIC_USE_LLVM=OFF
DYNARMIC_WARNINGS_AS_ERRORS=OFF
```

Keep runtime CPU-feature detection enabled. Explicit `BUILD_SHARED_LIBS=OFF` is needed
for this snapshot's installed static-dependency discovery. Its bundled Zycore installation
also requires:

```sh
cmake --install build/windows/engine/externals/zydis/zycore --prefix C:/deps/dynarmic
```

For CMake 4.3, use `CMAKE_POLICY_VERSION_MINIMUM=3.5` when configuring the dependency.
The tested build reused Boost 1.90.0 headers with `Boost_NO_BOOST_CMAKE=ON`,
`Boost_INCLUDE_DIR` and `CMAKE_POLICY_DEFAULT_CMP0167=OLD`; supply those settings to
consumer configuration as needed. No extra compiler or full Boost installation is required.

## Local validation commands

The default CTest suite uses only authored fixtures. Resource-backed checks run locally,
with explicit external resources and a temporary directory outside the source/build tree:

```powershell
build/windows/native/api_test.exe C:/external/resource-root verify
build/windows/native/api_test.exe C:/external/resource-root
build/windows/native/api_test.exe C:/external/resource-root benchmark 256 3
build/windows/native/api_test.exe C:/external/resource-root long-stream 1800
python tests/integration.py --exe build/windows/native/dsee-compat.exe --assets C:/external/resource-root --reference-results C:/external/windows-probe.jsonl --reference-label windows-dynarmic-probe --report build/windows/integration.json
```

`verify` emits complete PCM and input hashes and compares fragmented/full-block processing.
The integration script accepts complete probe JSONL or legacy prefix-reference JSON.
Benchmarks record initialization and first-block time separately from 64 warmup blocks and
steady-state P50/P95/P99/max block latencies. The long stream checks MP3128/192 kHz for
30 minutes of generated audio while discarding output after block validation and checksumming.
Windows memory checks use private committed bytes, with a 16 MiB growth limit after warmup.

The separate `DSEE_DYNARMIC_PROBE_ONLY=ON` and `DSEE_UNICORN_PROBE_ONLY=ON` builds are
still available. Probe-only `--stdin-assets` is not exposed through the public C API or WAV CLI.

## Historical probe measurements

The following records predate C API/CLI integration and remain the independent baseline.
Integrated-runtime results are recorded separately in [VALIDATION.md](VALIDATION.md).

## Correctness — 2026-10-22

- The default native macOS suite passed 5/5. The shared-harness Unicorn regression
  reproduced all 36 earlier complete Mac output hashes, including drained tails.
- Both engine self-tests passed. Coverage includes guest calls and returns,
  import dispatch, blocked/unknown imports, invalid pointers, non-executable
  data, writes to code pages, a 64-bit store crossing into a read-only page,
  allocator tracking/coalescing and SHA-256.
- Windows Dynarmic completed all 36 synthetic cases and drained each tail.
  **36/36 complete PCM hashes matched Windows Unicorn**, with identical input
  hashes and output frame counts. No fallback engine is linked or used.
- Consequently, **21/36 complete outputs match native macOS bit for bit**.
  The other outputs are the same bytes already measured in the Windows Unicorn
  comparison: maximum absolute difference 1,680 PCM32 integer units
  (approximately -122.13 dBFS), and largest per-case RMS difference about
  -154.61 dBFS. These metrics are inherited from the verified identical output
  set, not a new sample-by-sample comparison. Dynarmic introduced no additional
  differences in these fixtures. This is not a perceptual-quality evaluation.
- Each of the five resources was individually corrupted in memory and rejected
  before engine initialization. No resource or corrupted fixture was saved in
  the repository or Windows workspace.

The host received verified-input bytes over authenticated SSH directly into
process memory, using the bounded `--stdin-assets` test transport. Output PCM was
synthetic comparison data only. The profile still rejects unknown resources.

## Performance

Host: Intel Core i7-11800H, native Windows x86-64; no WSL or additional CPU
translator. Each configuration used 64 warmup blocks and three repeats, reusing
the same deterministic noise block as the QEMU and Unicorn tests. Times include
emulation, native bridges, input copies and per-block state checks. They exclude
resource validation, initialization/JIT warmup, file I/O and postprocessing.

The extended run measured 256 blocks per repeat (5.46133 seconds of input).
Median multiples of real time, with the minimum–maximum range:

| Profile | 48 kHz output | 96 kHz output | 192 kHz output |
| --- | ---: | ---: | ---: |
| PCM | 5.916× (5.590–6.647) | 3.470× (3.460–3.828) | 2.962× (2.900–3.082) |
| AAC128 | 3.592× (3.592–3.836) | 2.716× (2.696–2.848) | 2.414× (2.123–2.437) |
| MP3128 | 3.649× (3.481–3.779) | 2.652× (2.378–2.660) | 2.331× (2.204–2.341) |

The matching 64-measured-block short run gave these medians:

| Profile | 48 kHz output | 96 kHz output | 192 kHz output |
| --- | ---: | ---: | ---: |
| PCM | 5.563× | 3.314× | 2.898× |
| AAC128 | 3.568× | 2.686× | 2.487× |
| MP3128 | 3.912× | 2.497× | 2.355× |

Under the matching short-test conditions, the medians were **4.6–6.7× the earlier
QEMU throughput** and **10.4–13.6× the earlier Unicorn throughput** for the same
profile/rate. These are comparisons of tested implementations, not an isolated
engine benchmark: versions, OS/runtime, memory mappings and import mechanisms
differ. Host load, CPU affinity and power state were not controlled.

All measured configurations exceeded real time, including the extended run.
This is average core throughput on the synthetic workload. Live playback,
deadline jitter, startup latency, arbitrary audio, the complete WAV pipeline
and postprocessing under Windows remain unvalidated by this probe.

Local records in `build/dynarmic` include `dependency-metadata.json`,
`host-metadata.json`, `verify.jsonl`, `verification-summary.json`,
`numerical-equivalence.json`, `benchmark-short.jsonl`, `benchmark-long.jsonl`,
`benchmark-summary.json`, `status.json` and `probe-ctest.log`.
