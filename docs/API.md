# C API contract (0.1)

The installed header is the authoritative signature reference. ABI values of public statuses and source profiles are fixed for the 0.1 series. Processing input/output is interleaved stereo; a frame contains two samples. Input is signed 16-bit, output signed 32-bit. These representations match little-endian Apple Silicon, Windows x86-64 and the experimental AArch64 Linux target. Windows uses the integrated Dynarmic backend; resource-root strings are UTF-8. Under QEMU, the C API belongs to the AArch64 guest process; an x86 application cannot directly link that static library. See [QEMU notes](QEMU.md).

Optional envelope/crossover controls are exposed by the separate C++ API in `dsee_postprocess.hpp`
and both WAV CLI entry points, documented in [POSTPROCESS.md](POSTPROCESS.md). Link the installed CMake target
`dsee::dsee_postprocess` to use them directly with seekable `AudioSource` instances and a PCM32 sink.
They do not alter this streaming API's ABI or pinned resource profile.

## Lifecycle

- `dsee_inspect(root)` validates all supported profile files without executing firmware. It does not reserve an instance.
- `dsee_create(root, rate, profile, &context)` validates only the selected resources and constructs a fresh runtime image. It sets the result to null on failure. A second active context is rejected.
- `dsee_process(context, input, frames, output, capacity, &consumed, &produced)` buffers partial input and emits complete algorithm blocks. Supply output capacity of at least `1024 * rate / 48000` frames. With larger input, the function may return after consuming only a prefix; advance by `consumed`. Non-overlapping valid buffers are required. Output and count pointers must be non-null; input may be null only when its count is zero.
- `dsee_finish(context, output, capacity, &produced, &padding, &done)` must be repeated until done is 1. Each successful call returns at most one block. Padding counts refer to zero-valued **input** frames appended in that call. The final partial block is padded, followed by at most 48 zero-fed blocks. Four consecutive all-zero zero-fed output blocks finish the stream. Retain all produced output; this API does not trim latency. Empty streams and subsequent finish calls after completion produce no additional output.
- Once finish begins, additional process calls fail. Processing failure poisons the instance; destroy it instead of retrying.
- `dsee_destroy(context)` releases the original wrapper's state, all remaining tracked foreign allocations/readers, and its memory image. A null context is accepted. Do not use a handle after destruction. Failed destruction still releases the context.

## Errors and concurrency

All API calls other than error/version access are guarded against concurrent entry. Caller-owned buffers and handles must also obey normal lifetime rules. `DSEE_BUSY` is retryable after the other call completes. The current implementation is deliberately single-instance.

`dsee_last_error()` returns thread-local text valid until the next API call on the same thread. Check the status first. Process output counts describe the successfully completed prefix even if a later block fails; callers creating an atomic output file should discard the entire failed conversion.

Normal errors do not terminate the process. Guest construction, processing and destruction failures return `DSEE_PROCESS_ERROR`; processing failure poisons the context, and subsequent process/finish calls return `DSEE_STATE_ERROR`. Destruction still releases all host resources. Host allocation failure during inspect/create returns `DSEE_INTERNAL_ERROR`. Exceptions are caught at the C boundary. Unexpected imported interfaces, Sony error logs/assertions and compatibility-shim failures are caught at the guarded foreign-call boundary and reported as processing errors. Arbitrary native segmentation faults and host out-of-memory conditions are outside that guarantee. No process-wide signal handlers are installed.

## Loading and permissions

Only the pinned library is executed. The loader checks ELF64/AArch64 structure and bounds, overlapping segments, supported RELA/APS2/RELR forms, symbol references and relocation targets. Import resolution is an explicit allowlist; unused unsupported imports fail if called. The native backend redirects the 43 expected TLS reads to a private stack-canary area; Dynarmic validates the same instruction count and supplies an emulated `TPIDR_EL0`. Code pages become read/execute, never write/execute. Parameters and the emulated CPU feature text are read from in-memory streams; no firmware cache or patched binary is written.

## Backend and installation

`DSEE_BACKEND=AUTO` chooses native execution on Apple Silicon and Dynarmic on Windows x86-64.
There is no runtime backend switch or fallback emulator. The public ABI and single-instance rules are identical.
The Dynarmic backend uses the tested safe optimization set, checked page-table mappings and a 32 MiB code cache.
It is not a general-purpose Android runtime or a security sandbox.

Install with `cmake --install`, then link `dsee::dsee_compat` from `find_package(dsee_compat CONFIG REQUIRED)`.
The exported target supplies the internal static archives and system dependencies. On Windows the consumer
also supplies the Dynarmic package prefix and its Boost header root when needed. A C translation unit can
use the header; enable the C++ language for its linker. The `installed_c_client` CTest exercises this path.
