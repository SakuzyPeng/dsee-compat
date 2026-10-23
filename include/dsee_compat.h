#ifndef DSEE_COMPAT_H
#define DSEE_COMPAT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct dsee_context dsee_context;
typedef enum {
    DSEE_OK = 0,
    DSEE_INVALID_ARGUMENT,
    DSEE_ASSET_ERROR,
    DSEE_UNSUPPORTED,
    DSEE_BUSY,
    DSEE_STATE_ERROR,
    DSEE_PROCESS_ERROR,
    DSEE_TAIL_LIMIT,
    DSEE_INTERNAL_ERROR
} dsee_status;
typedef enum { DSEE_PCM = 0, DSEE_AAC128 = 1, DSEE_MP3128 = 2 } dsee_profile;
/* root contains vendor/lib64 and vendor/etc. No file is copied or modified.
 * On Windows, root is UTF-8.
 */
dsee_status dsee_inspect(const char *root);
dsee_status dsee_create(const char *root, unsigned output_rate, dsee_profile profile,
                        dsee_context **result);
/* Interleaved stereo PCM16 -> PCM32. Capacities/counts are frames, not samples.
 * Output capacity must be >= 1024 * output_rate/48000. Buffers must not overlap.
 * May consume only part of input; callers must honor both returned counts.
 * Calls are serialized by the caller; concurrent entry returns DSEE_BUSY.
 */
dsee_status dsee_process(dsee_context *, const int16_t *input, size_t input_frames, int32_t *output,
                         size_t capacity_frames, size_t *consumed, size_t *produced);
/* Call repeatedly until done=1. Returns one padded/zero block at most.
 * Four consecutive silent zero-fed blocks end draining; 48 is the limit.
 * Algorithm latency and emitted padding are retained. Empty input emits nothing.
 */
dsee_status dsee_finish(dsee_context *, int32_t *output, size_t capacity_frames, size_t *produced,
                        size_t *padding_frames, int *done);
dsee_status dsee_destroy(dsee_context *);
/* Thread-local error text, valid until the next API call on this thread. */
const char *dsee_last_error(void);
const char *dsee_version(void);
#ifdef __cplusplus
}
#endif
#endif
