# C API contract (0.1)

The installed header is the authoritative signature reference. ABI values of public statuses and source profiles are fixed for the 0.1 series. Processing input/output is interleaved stereo; a frame contains two samples. Input is signed 16-bit, output signed 32-bit. These representations match little-endian Apple Silicon.

Optional envelope/crossover controls are exposed by the separate C++ API in `dsee_postprocess.hpp`
and both WAV CLI entry points, documented in [POSTPROCESS.md](POSTPROCESS.md). Link
`libdsee_postprocess.a` to use them directly with seekable `AudioSource` instances and a PCM32 sink.
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

Normal errors do not terminate the process. Unexpected imported interfaces, Sony error logs/assertions and compatibility-shim failures are caught at the guarded foreign-call boundary and reported as processing errors. Arbitrary native segmentation faults and host out-of-memory conditions are outside that guarantee. No process-wide signal handlers are installed.

## Loading and permissions

Only the pinned library is executed. The loader checks ELF64/AArch64 structure and bounds, overlapping segments, supported RELA/APS2/RELR forms, symbol references and relocation targets. Import resolution is an explicit allowlist; unused unsupported imports fail if called. The 43 expected TLS reads are redirected to a private stack-canary area. Code pages become read/execute, never write/execute. Parameters and the emulated CPU feature text are read from in-memory streams; no firmware cache or patched binary is written.
