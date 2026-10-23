# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A compatibility runtime for one hash-pinned Sony Android ARM64 library (`libsonydseehxwrapper.so`, Xperia 5 V firmware `67.1.A.2.229`). Apple Silicon executes it natively; Windows x86-64 uses Dynarmic. Both expose the same C API and WAV CLI. Public interfaces and postprocessing use C++17; the internal Dynarmic target uses C++20. Users provide an explicit external resource root.

## Hard rules (from AGENTS.md — follow exactly)

- Never add proprietary libraries, parameter/model data, adapted images, disassembly/decompilation, IDA databases, or firmware download code/URLs. Do not copy, cache, symlink, upload, or commit external resources into this repository, including build directories. Local resource-backed test fixtures must be in external temporary directories and cleaned afterward. Corrupt-input fixtures are generated only in temporary test directories.
- Keep the hash-pinned profile (`AssetSpec` size + SHA-256 in `src/assets.hpp`, shared by runtime and probe). Unknown resources must fail before any native execution.
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

`DSEE_BACKEND=AUTO` selects NATIVE on Apple Silicon and DYNARMIC on Windows x86-64. The integrated Dynarmic target requires the installed 6.7.0 package built from revision `a41c380246d3d9f9874f0f792d234dc0cc17c180`; CMake never fetches it. Reuse installed toolchains and the existing build tree. `DSEE_EXPERIMENTAL_LINUX=ON` retains native AArch64 GNU/Linux with OpenSSL. Linux x86-64 and Intel Mac integration are outside this version. Formatting follows `.clang-format`; MSVC uses `/W4 /fp:strict /utf-8`, other compilers retain their existing warning options, with `-ffp-contract=off` on emulation/probe targets.

Optional local native tests (need an external resource root):

```sh
build/api_test /path/to/resource-root
build/api_test /path/to/resource-root verify
build/api_test /path/to/resource-root benchmark 256 3
build/api_test /path/to/resource-root long-stream 1800
python3 tests/integration.py --exe build/dsee-compat --assets /path/to/resource-root \
  --reference-results /path/to/external/validation.json --report build/integration.json
```

`api_test` with no argument is the no-assets CTest case; with a root it runs the native suite. Adding `instrumented` runs the same behavioral checks but skips the RSS-growth limit for Valgrind/QEMU translation caches. The `verify` mode emits complete PCM hashes and checks fragmented processing; `benchmark` records initialization, first-block and steady-state percentiles; `long-stream` tests MP3128 at 192 kHz without retaining PCM. `tests/integration.py` accepts legacy prefix-reference JSON or complete probe JSONL. Its temporary directory must be outside the repository. `tests/integration.py --core-only` skips postprocessing comparisons; `--reference-label` identifies a non-default baseline without implying original-wrapper equivalence.

## Architecture

- **`src/assets.hpp`** — the shared, unchanged size/SHA-256 profile; no original bytes are included.
- **`src/elf.cpp` / `elf.hpp`** — `Elf` parses and bounds-checks ELF64/AArch64 (segments, dynamic symbols, RELA, Android APS2 packed relocations, RELR). `Image` maps segments into one allocation, applies relocations through a caller-supplied `resolve` callback, rewrites each `mrs xN, tpidr_el0` into an `adr` pointing at a private page after the image that holds a random stack canary at offset 40 (`tls_patches` must be exactly 43), then `mprotect`s pages to R/X or R/W — never W+X.
- **`src/resources.cpp` / `resources.hpp`** — shared canonical-path, size and SHA-256 validation: CommonCrypto on macOS, BCrypt on Windows and OpenSSL on Linux. `inspect` validates all resources; `create` validates only library, DSX and the selected parameters before constructing a backend. Original files are never reopened during processing.
- **`src/runtime.cpp` / `backend.hpp`** — shared C API state machine and internal block/close interface. Input buffers into 1024-frame blocks; finish pads a partial block and emits zero blocks until four consecutive silent outputs, with a limit of 48. A process-wide gate rejects concurrent entry and permits one live instance. Fixed-size thread-local error text and exception boundaries preserve the C ABI. Failed processing poisons the instance; destruction releases it even after a fault.
- **`src/backend_native.cpp`** — native import allowlist, tracked allocations, memory readers, guarded `sigsetjmp` calls and state checks. No C++ object with a destructor may live between the jump boundary and a foreign call. The 120-byte wrapper object and its health offsets belong to the pinned profile.
- **`src/emulation.cpp` / `cpu_dynarmic.cpp`** — guest ELF loading, bounded arenas, allocation tracking, imports, emulated TLS and Dynarmic execution. `emulation_cpu.hpp` is the CPU interface. Keep safe optimizations, checked page-table mappings, memory-abort checks and the 32 MiB cache. Callback faults are copied into fixed buffers and propagated after JIT execution, never unwound through generated code. Unsupported instructions fail without fallback. Destroy JIT state before its backing memory.
- Both backends explicitly pad the pinned wrapper's `calloc(1, 110000)` state to 112 KiB. The original wrapper uses the extra allocator slack; do not remove this allowance without resource-backed memory checks.
- **`src/platform.cpp`, `src/cli.cpp`, `src/wav.cpp`** — UTF-8 API paths and wide Windows CLI entry, binary temporary files, 64-bit spool offsets, cleanup and refusal to replace outputs. Windows uses same-directory `MoveFileExW` without replacement; POSIX uses a hard-link publication. The CLI retains the RIFF 4 GiB limit.
- **Tests** — default cases need no original resources. They cover ELF/WAV boundaries, sparse offsets beyond 2 GiB, Unicode paths, output races, postprocessing, C headers, test-only backend faults and an installed C client. Dynarmic builds also run authored instruction/memory self-tests. The fault backend is linked only into its own test executable; no production resource-validation bypass exists.
- **`tools/check_distribution.py`** — scans the index and reachable commit trees against a narrow text-file allowlist, size limits and private/download material patterns. The CMake package template is explicitly allowed. The checker does not inspect unstaged or untracked contents; review those separately before staging.
- **`tools/emulation_probe.cpp`** — optional diagnostic transport and synthetic workloads, using the same `EmulatedProfile` as production. `DSEE_UNICORN_PROBE_ONLY=ON` selects the separate `tools/cpu_unicorn.cpp`; `DSEE_DYNARMIC_PROBE_ONLY=ON` selects Dynarmic. The bounded stdin asset transport is probe-only. `tools/fixtures.hpp` is shared with API verification. Never package original bytes with any artifact.
- **`src/postprocess.cpp` / `include/dsee_postprocess.hpp`** — independent C++17 offline DSP: radix-2 FFT/STFT, rational reference resampling, automatic signed alignment, stereo-linked envelope attenuation, complementary crossover and optional per-track automatic mixing with seam/envelope objectives and diagnostics. Installed as `libdsee_postprocess.a`; used by `postprocess` and optional `process --postprocess`. No external runtime, codecs, metadata or Sony resources. Native synthetic checks are in `tests/postprocess.cpp`; standard-library CLI tests in `tests/cli.py`; local integrated/standalone equivalence in `tests/integration.py`. Keep `docs/POSTPROCESS.md` synchronized.

## Docs to keep in sync

`docs/API.md` (C API contract; status/profile enum values are ABI-fixed for 0.1), `docs/DSEE.md` (pinned-firmware mechanism summary and evidence limits), `docs/POSTPROCESS.md` (optional offline controls), `docs/VALIDATION.md` (local test record), `docs/DISTRIBUTION.md` (provenance and what stays out of the repo), and the README's supported-input list. `dsee_version()` and `project(VERSION)` in `CMakeLists.txt` are both `0.1.0`.
