# dsee-compat

[简体中文](README.md) · **English**

An independent compatibility runtime, with a WAV command-line tool, for **running one supported Sony Android
audio library on Apple Silicon macOS**. It is written in C++17, relocates the ELF in memory, applies narrowly
scoped ABI adaptation, and then executes the original ARM64 instructions.

> **No Sony code, binaries, models, parameter files, firmware downloads, or adapted images are distributed here.**
> Build the tool without those resources; provide your own resources locally when running it. This is not a Sony
> product or an officially supported port. MIT covers this repository's own code only and does not grant rights
> to Sony software. See [distribution boundaries](docs/DISTRIBUTION.md).

This page covers getting started and building. **For the full C API contract see the [API reference](docs/API.md);
for test results and performance numbers see the [validation record](docs/VALIDATION.md).**

## Contents

- [What you can do with it](#what-you-can-do-with-it)
- [Supported scope](#supported-scope)
- [Building from source](#building-from-source)
- [Preparing the external resource directory](#preparing-the-external-resource-directory)
- [Three steps to your first conversion](#three-steps-to-your-first-conversion)
- [How processing works](#how-processing-works)
- [C API](#c-api)
- [FAQ](#faq)
- [Known limitations](#known-limitations)
- [Testing](#testing)
- [Documentation](#documentation)
- [License](#license)

## What you can do with it

- **Process WAV files on a Mac**: feed 48 kHz stereo PCM16 into the original algorithm and get stereo PCM32 at
  48 / 96 / 192 kHz.
- **Check whether your resources are supported**: `inspect` verifies the size and SHA-256 of the library and all
  three profiles without executing any firmware code.
- **Embed it in your own program**: a static library `libdsee_compat.a` plus a plain C header, with a streaming
  block-by-block interface.
- **Leave the originals alone**: resources are read into memory, verified and used there — never copied, cached
  or modified, and no patched binary is written.
- **Write output safely**: existing files are never overwritten; output goes to a temporary file beside the
  destination and is published atomically when complete, and is cleaned up on failure.

## Supported scope

| Item | Supported |
| --- | --- |
| Host | Apple Silicon macOS (tested on Apple M4 Pro / macOS 27.0.1; other systems not yet validated) |
| Firmware | Xperia 5 V `67.1.A.2.229`, identified by exact resource hashes |
| Input | RIFF/WAVE, format tag 1, 48 kHz, stereo, signed PCM16 |
| Output | Stereo signed PCM32 at 48, 96 (default) or 192 kHz |
| Source metadata profile | `pcm` (default), `aac128`, `mp3128` |

Source metadata profiles only select algorithm parameters; the CLI does **not** decode AAC/MP3 or infer the
original codec of a WAV file.

| | macOS (Apple Silicon) | Intel Mac | Linux / Windows |
| --- | --- | --- | --- |
| Build | ✅ | ❌ | ❌ |
| Run | ✅ | ❌ | ❌ |

CMake stops with an error on anything other than Apple Silicon macOS.

## Building from source

### Requirements

- **Apple Silicon macOS** and **Xcode Command Line Tools**.
- **CMake 3.20+**.
- **Python 3** — only for tests and repository checks.

The build downloads no packages and needs no Sony resources.

### Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Outputs are `build/dsee-compat` (CLI) and `build/libdsee_compat.a` (static library).
Add `-DBUILD_TESTING=OFF` to build only the CLI and static library, without Python.

### Install

```sh
cmake --install build --prefix <destination>
```

This installs the CLI, static library, and C header.

## Preparing the external resource directory

Pass a directory with this layout; **do not place the resources in this repository**:

```text
<resource-root>/
  vendor/lib64/libsonydseehxwrapper.so
  vendor/etc/dsx_param_file.bin
  vendor/etc/DrangeparaPCM48.bin
  vendor/etc/DrangeparaAAC128.bin
  vendor/etc/DrangeparaMP3_128.bin
```

- `inspect` checks the library and all three profiles; `process` needs only the library, DSX parameters and the
  selected Drange file.
- Every selected file must match its supported size and SHA-256; unknown resources are rejected before any native
  code runs.
- Symlinks escaping the resource root are rejected.
- The verified bytes are used directly in memory, so files are not reopened during processing. The original files
  are not changed.

## Three steps to your first conversion

1. Prepare the resource directory as described above.
2. Check that the resources are supported:

   ```sh
   build/dsee-compat inspect --assets /path/to/resource-root
   ```

   On success it prints one line of JSON listing the firmware version and available profiles.

3. Process a WAV:

   ```sh
   build/dsee-compat process --assets /path/to/resource-root \
     --input input.wav --output output.wav \
     --output-rate 96000 --source-profile pcm
   ```

   When it finishes it prints a one-line JSON summary: input, padding and output frames, rates, profile, and
   elapsed time including output I/O.

Run `build/dsee-compat --help` for usage and `--version` for the version.

## How processing works

- Input is streamed in small buffers.
- The final partial block is zero-filled to 1024 input frames.
- Zero blocks are then fed until four consecutive blocks produce all-zero output. Draining is limited to 48 extra
  zero blocks; **failure to settle is an error**, not a silently truncated result.
- Algorithm latency and the zero-fed tail are preserved, so **output can be longer than input**. No automatic
  alignment, normalization, or resampling of unsupported input is performed.
- Empty input produces an empty WAV.

## C API

Include `dsee_compat.h`, link `libdsee_compat.a` and the C++ standard library. The header is valid C.

```text
dsee_inspect → dsee_create → repeated dsee_process → repeated dsee_finish → dsee_destroy
```

- There is one active instance per process. Calls must be serialized; overlapping calls and second instances
  return `DSEE_BUSY`.
- Counts and capacities are **stereo frames**, not individual samples.
- Normal validation and native-shim failures are returned as statuses, with text from `dsee_last_error()`.
- This library executes a hash-pinned foreign binary in-process; **it is not a sandbox** and cannot promise
  recovery from arbitrary native faults or ABI changes.

The full lifecycle, buffer requirements and error semantics are in the [API reference](docs/API.md).

## FAQ

**Why are my resources reported as unsupported?**
Every file is compared by exact size and SHA-256, and only the `67.1.A.2.229` set is accepted. Other firmware
versions or modified files — even by a single byte — are rejected before any code runs. Use `inspect` first to
confirm the directory layout and files.

**Why won't my WAV open?**
Input must be format tag 1, 48 kHz, stereo, 16-bit integer PCM. 44.1 kHz, 24-bit, float, RF64 and WAVE
extensible are not supported and are not converted automatically. Convert to 48 kHz stereo PCM16 with your own
tools first.

**Why is the output longer than the input?**
The algorithm has latency, and zero blocks are fed at the end to drain the tail. All of that output is kept;
nothing is trimmed or aligned. `padding_frames` in the JSON summary counts the zero input frames appended.

**Do `aac128` / `mp3128` decode AAC/MP3?**
No. They tell the algorithm what codec the source material originally used, so the matching Drange parameters are
selected. Input must still be a PCM16 WAV.

**Why won't it overwrite the output file?**
By design. The CLI refuses existing outputs, identical input/output paths, and results that may exceed the RIFF
size limit. It writes a temporary file beside the destination and publishes it atomically without replacing an
existing destination; a failed conversion removes its temporary output.

**Can I run several instances or process on multiple threads?**
Not in this version. One active instance is allowed per process, and concurrent calls return `DSEE_BUSY`, which
can be retried after the other call completes.

## Known limitations

- **Tested only on Apple M4 Pro / macOS 27.0.1.** Other Apple Silicon systems and other firmware versions are not
  validated.
- **Xperia hardware output has not been captured for comparison**; current matches are against the local
  original-wrapper research reference.
- No RF64, WAVE extensible, 24-bit, float or 44.1 kHz input.
- No GUI, no Audio Unit, no concurrent instances or multi-threaded processing.
- Not a sandbox: arbitrary native segmentation faults and host out-of-memory conditions are outside the
  error-handling guarantee.

## Testing

### Default tests (no Sony resources)

`ctest` uses self-generated ELF/WAV fixtures covering ELF range/overflow rejection, RELA/APS2/RELR decoding, WAV
parsing, atomic output, CLI errors, C header linkage and API errors.

### Optional local integration tests

Run these locally when external resources are available:

```sh
build/api_test /path/to/resource-root
build/api_test /path/to/resource-root benchmark
python3 tests/integration.py --exe build/dsee-compat \
  --assets /path/to/resource-root \
  --reference-results /path/to/external/validation.json \
  --report build/integration.json
```

`--reference-results` is an external JSON record of original-wrapper synthetic fixtures; it is not distributed
here. The script checks existing fixture PCM hashes against the output prefix before the new CLI's tail. Test
audio and corrupted-resource fixtures are generated temporarily and removed.
**Never upload these resources to hosted CI.**

Current local results: 36 original-wrapper reference matches, nine profile/rate tail comparisons, fragmented input
equivalence, concurrent-call rejection, 200 instance lifecycles, and a 5,000-block stream. See the
[validation record](docs/VALIDATION.md).

### Repository checks

```sh
python3 tools/check_distribution.py
```

This checks staged content and all reachable commit trees against the source-only path policy and binary
restrictions. Review the staged diff too. A passing check is **not** a legal determination that cross-device use
is permitted.

## Documentation

[C API contract](docs/API.md) ·
[Validation record](docs/VALIDATION.md) ·
[Distribution boundaries and provenance](docs/DISTRIBUTION.md)

## License

This repository's own code and documentation are under the [MIT License](LICENSE). It grants no rights to Sony
or other third-party programs, model weights, parameters or trademarks. You supply your own lawfully obtained
files and remain responsible for the applicable software license and local interoperability/copyright rules; see
[distribution boundaries](docs/DISTRIBUTION.md).
