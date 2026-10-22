# dsee-compat

Independent C++17 compatibility runtime and WAV CLI for running one supported Sony Android audio library on Apple Silicon macOS. The runtime executes the original ARM64 instructions after in-memory ELF relocation and narrowly scoped ABI adaptation.

**No Sony code, binaries, models, parameter files, firmware downloads, or adapted images are distributed here.** Build the tool without those resources; provide your own resources locally when running it. This is not a Sony product or an officially supported port. MIT covers this repository's own code only and does not grant rights to Sony software. See [distribution boundaries](docs/DISTRIBUTION.md).

## Status and supported input

- Tested on Apple M4 Pro / macOS 27.0.1. Other Apple Silicon systems have not yet been validated.
- Supports the inspected Xperia 5 V firmware `67.1.A.2.229` by exact resource hashes.
- Input: RIFF/WAVE format tag 1, 48 kHz, stereo, signed PCM16.
- Output: stereo signed PCM32 at 48, 96 (default), or 192 kHz.
- Source metadata profiles: `pcm` (default), `aac128`, `mp3128`. These select algorithm parameters; the CLI does not decode AAC/MP3 or infer the original codec of a WAV file.
- Processing preserves algorithm latency and the zero-fed tail. Output can be longer than input. No automatic alignment, normalization, or resampling of unsupported input is performed.
- No RF64, WAVE extensible, 24-bit, float, 44.1 kHz, GUI, Audio Unit, concurrent instances, or multi-threaded processing in this version.

## Build without Sony resources

Requires Apple Silicon macOS, Xcode Command Line Tools, and CMake. Python 3 is needed only for tests and repository checks. No packages are downloaded by the build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Use `-DBUILD_TESTING=OFF` to build the CLI and static library without Python. Outputs are `build/dsee-compat` and `build/libdsee_compat.a`. `cmake --install build --prefix <destination>` installs the CLI, static library, and C header.

## External resource directory

Pass a directory containing this layout; do not place the resources in this repository:

```text
<resource-root>/
  vendor/lib64/libsonydseehxwrapper.so
  vendor/etc/dsx_param_file.bin
  vendor/etc/DrangeparaPCM48.bin
  vendor/etc/DrangeparaAAC128.bin
  vendor/etc/DrangeparaMP3_128.bin
```

`inspect` checks the library and all three profiles. `process` needs only the library, DSX parameters and the selected Drange file. Every selected file must match its supported size and SHA-256. Symlinks escaping the resource root are rejected. The verified bytes are used directly in memory, so files are not reopened during processing. The original files are not changed.

```sh
build/dsee-compat inspect --assets /path/to/resource-root
build/dsee-compat process --assets /path/to/resource-root \
  --input input.wav --output output.wav \
  --output-rate 96000 --source-profile pcm
```

Input is streamed in small buffers. The final partial block is zero-filled to 1024 input frames, then zero blocks are fed until four consecutive blocks produce all-zero output. Draining is limited to 48 extra zero blocks; failure to settle is an error, not a silently truncated result. Empty input produces an empty WAV. The JSON summary reports input, padding, output frames, and elapsed time including output I/O.

The CLI refuses existing outputs, same input/output paths and results that may exceed the RIFF size limit. It creates a temporary file beside the destination, then publishes the completed file atomically without replacing an existing destination. Failed conversion removes its temporary output.

## C API

Include `dsee_compat.h`, link `libdsee_compat.a` and the C++ standard library. The header is valid C. See [API contract](docs/API.md).

`dsee_inspect` → `dsee_create` → repeated `dsee_process` → repeated `dsee_finish` → `dsee_destroy`.

There is one active instance per process. Calls must be serialized; overlapping calls and second instances return `DSEE_BUSY`. Counts and capacities are stereo frames, not individual samples. Normal validation and native-shim failures are returned as statuses. This library executes a hash-pinned foreign binary in-process; it is not a sandbox and cannot promise recovery from arbitrary native faults or ABI changes.

## Optional local integration tests

The default tests use self-generated ELF/WAV fixtures and need no Sony resources. For local native tests:

```sh
build/api_test /path/to/resource-root
build/api_test /path/to/resource-root benchmark
python3 tests/integration.py --exe build/dsee-compat \
  --assets /path/to/resource-root \
  --reference-results /path/to/external/validation.json \
  --report build/integration.json
```

The reference-results argument is an external JSON record of original-wrapper synthetic fixtures; it is not distributed here. The script checks existing fixture PCM hashes against the output prefix before the new CLI's tail. Test audio and corrupted-resource fixtures are generated temporarily and removed.

Current local results: 36 original-wrapper reference matches, nine profile/rate tail comparisons with longer zero input, fragmented input equivalence, concurrent-call rejection, 200 instance lifecycles, and a 5,000-block stream. **Xperia hardware output has not been captured for comparison.** See [validation record](docs/VALIDATION.md).

## Repository checks

```sh
python3 tools/check_distribution.py
```

This checks staged content and all reachable commit trees against the source-only path policy and binary restrictions. Review the staged diff too. The private repository is an implementation workspace, not a legal determination that cross-device use is permitted.
