# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A C++17 compatibility runtime that loads one hash-pinned Sony Android ARM64 library (`libsonydseehxwrapper.so`, Xperia 5 V firmware `67.1.A.2.229`) into memory on Apple Silicon macOS, relocates it, adapts its ABI, and calls it directly as native ARM64 code. It ships a C API (`libdsee_compat.a`, `include/dsee_compat.h`) and a WAV CLI (`dsee-compat`). The Sony resources are never in this repo; users pass an external `--assets` root.

## Hard rules (from AGENTS.md — follow exactly)

- Never add proprietary libraries, parameter/model data, adapted images, disassembly/decompilation, IDA databases, or firmware download code/URLs. Do not copy, cache, symlink, upload, or commit external resources. Corrupt-input fixtures are generated only in temporary test directories.
- Keep the hash-pinned profile (`AssetSpec` size + SHA-256 in `src/runtime.cpp`). Unknown resources must fail before any native execution.
- The default CTest suite must build and pass without proprietary resources. Native integration tests run only locally; never upload resources to hosted CI.
- Build into the existing `build/` directory; no duplicate toolchains, dependency caches, or copied research workspaces.
- Before committing or pushing, run `python3 tools/check_distribution.py` and review the actual staged diff.
- Do not make the repository public or create a release unless explicitly asked.

## Commands

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # add -DBUILD_TESTING=OFF to skip tests/Python
cmake --build build -j 4
ctest --test-dir build --output-on-failure
ctest --test-dir build -R unit --output-on-failure  # single test: unit | api_without_assets | c_header | cli_without_assets
python3 tools/check_distribution.py                 # required before commit/push
```

CMake hard-fails on anything other than Apple Silicon macOS (`FATAL_ERROR` in `CMakeLists.txt`), so on Linux containers the CMake project cannot be built there. The postprocessing library itself uses standard C++17 only. Formatting follows `.clang-format` (LLVM base, 4-space indent, 100 columns). There is no separate lint step; targets compile with `-Wall -Wextra -Wpedantic`.

Optional local native tests (need an external resource root):

```sh
build/api_test /path/to/resource-root [benchmark]
python3 tests/integration.py --exe build/dsee-compat --assets /path/to/resource-root \
  --reference-results /path/to/external/validation.json --report build/integration.json
```

`api_test` with no argument is the no-assets CTest case; with a root it runs the native suite.

## Architecture

- **`src/elf.cpp` / `elf.hpp`** — `Elf` parses and bounds-checks ELF64/AArch64 (segments, dynamic symbols, RELA, Android APS2 packed relocations, RELR). `Image` maps segments into one allocation, applies relocations through a caller-supplied `resolve` callback, rewrites each `mrs xN, tpidr_el0` into an `adr` pointing at a private page after the image that holds a random stack canary at offset 40 (`tls_patches` must be exactly 43), then `mprotect`s pages to R/X or R/W — never W+X.
- **`src/runtime.cpp`** — implements the whole C API.
  - `read_asset` canonicalizes paths (rejects symlinks escaping the root), checks exact size and SHA-256 via CommonCrypto, and returns bytes kept in memory; firmware files are never reopened.
  - `resolve` is the import allowlist: `MAP(...)` entries are host shims (tracked `malloc`/`free`, in-memory `fopen` via `funopen` over the verified asset bytes, `property_get`, logging/assert → fault, etc.); `blocked[]` imports resolve to a stub that faults if called; `allowed[]` pass through to `dlsym`; anything else throws at load time. Adding firmware-facing behavior means extending this list.
  - Foreign calls go only through `invoke()`, which sets `sigsetjmp`; shims call `native_fault()` to `siglongjmp` back and poison the instance. No C++ objects with destructors may live between the `sigsetjmp` and the foreign call. All foreign allocations and open files are tracked on the `dsee_context` linked lists and freed in `cleanup()`.
  - The Sony object is a 120-byte `calloc` driven by mangled symbols (`SonyDseeHx` ctor/`process`/dtor). Health checks read fixed offsets in its state via `field<T>()`; these offsets are part of the pinned profile.
  - Streaming: input is buffered into 1024-frame blocks; each block yields `1024 * rate/48000` output frames. `dsee_finish` pads the final partial block, then feeds zero blocks until 4 consecutive all-zero outputs, failing with `DSEE_TAIL_LIMIT` after 48.
  - Concurrency: a global `atomic_flag` `Lock` makes every API call return `DSEE_BUSY` on overlap, and a single `active` context is allowed per process. Error text is thread-local.
- **`src/cli.cpp`, `src/wav.cpp`** — WAV CLI with `process` and `postprocess` paths. Native input stays 48 kHz PCM16; offline readers also accept stereo PCM32 and the documented reference rates (format tag 1). `WavWriter` writes PCM32 to a temp file beside the destination and publishes atomically, never replacing an existing file and refusing output beyond the RIFF 4 GiB limit.
- **Tests** — `tests/unit.cpp` builds synthetic ELF/WAV byte fixtures in memory; `tests/cli.py` exercises CLI error paths with a nonexistent assets root; `tests/c_header.c` checks the header compiles as C. None need Sony resources.
- **`tools/check_distribution.py`** — scans the index and every reachable commit tree against a path allowlist (top-level files in `TOP`, plus `include/ src/ tests/ tools/ docs/` with source/text extensions), a 256 KiB blob limit, text-only content, and local-path/download-URL patterns. A new top-level file or directory must be added to its allowlist or the check fails.
- **`src/postprocess.cpp` / `include/dsee_postprocess.hpp`** — independent C++17 offline DSP: radix-2 FFT/STFT, rational reference resampling, automatic signed alignment, stereo-linked envelope attenuation, complementary crossover and optional per-track automatic mixing with seam/envelope objectives and diagnostics. Installed as `libdsee_postprocess.a`; used by `postprocess` and optional `process --postprocess`. No external runtime, codecs, metadata or Sony resources. Native synthetic checks are in `tests/postprocess.cpp`; standard-library CLI tests in `tests/cli.py`; local integrated/standalone equivalence in `tests/integration.py`. Keep `docs/POSTPROCESS.md` synchronized.

## Docs to keep in sync

`docs/API.md` (C API contract; status/profile enum values are ABI-fixed for 0.1), `docs/POSTPROCESS.md` (optional offline controls), `docs/VALIDATION.md` (local test record), `docs/DISTRIBUTION.md` (provenance and what stays out of the repo), and the README's supported-input list. `dsee_version()` and `project(VERSION)` in `CMakeLists.txt` are both `0.1.0`.
