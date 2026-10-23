#include "backend.hpp"
#include "dsee_compat.h"
#include "elf.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {
thread_local char error_text[512]{};
std::atomic_flag gate = ATOMIC_FLAG_INIT;
struct Lock {
    bool owns = !gate.test_and_set(std::memory_order_acquire);
    ~Lock() {
        if (owns)
            gate.clear(std::memory_order_release);
    }
};
dsee_status fail(dsee_status code, const char *text) noexcept {
    std::snprintf(error_text, sizeof(error_text), "%s", text);
    return code;
}
} // namespace
struct dsee_context {
    std::unique_ptr<dsee::Backend> backend;
    unsigned ratio = 0;
    std::array<int16_t, 2048> pending{};
    size_t pending_frames = 0;
    uint64_t total_input = 0;
    bool finishing = false, finished = false, poisoned = false;
    unsigned zero_blocks = 0, quiet_blocks = 0;
};
namespace {
dsee_context *active = nullptr;
bool valid(dsee_context *c) {
    return c && c == active;
}
bool block(dsee_context *c, int32_t *out) noexcept {
    try {
        c->backend->process_block(c->pending.data(), out);
        c->pending_frames = 0;
        return true;
    } catch (const std::exception &e) {
        fail(DSEE_PROCESS_ERROR, e.what());
    } catch (...) {
        fail(DSEE_PROCESS_ERROR, "Unknown processing failure");
    }
    c->poisoned = true;
    return false;
}
} // namespace
extern "C" {
const char *dsee_last_error() {
    return error_text;
}
const char *dsee_version() {
    return "0.1.0";
}
dsee_status dsee_inspect(const char *root) {
    Lock lock;
    if (!lock.owns)
        return fail(DSEE_BUSY, "Concurrent API call");
    error_text[0] = 0;
    if (!root || !*root)
        return fail(DSEE_INVALID_ARGUMENT, "Resource directory required");
    try {
        auto resources = dsee::load_resources(root);
        dsee::Elf elf(std::move(resources.bytes[0]));
        return DSEE_OK;
    } catch (const std::bad_alloc &) {
        return fail(DSEE_INTERNAL_ERROR, "Resource allocation failed");
    } catch (const std::exception &e) {
        return fail(DSEE_ASSET_ERROR, e.what());
    } catch (...) {
        return fail(DSEE_INTERNAL_ERROR, "Unknown inspection failure");
    }
}
dsee_status dsee_create(const char *root, unsigned rate, dsee_profile profile,
                        dsee_context **result) {
    if (result)
        *result = nullptr;
    Lock lock;
    if (!lock.owns)
        return fail(DSEE_BUSY, "Concurrent API call");
    error_text[0] = 0;
    if (!result || !root || !*root || (rate != 48000 && rate != 96000 && rate != 192000) ||
        profile < DSEE_PCM || profile > DSEE_MP3128)
        return fail(DSEE_INVALID_ARGUMENT, "Invalid creation arguments");
    if (active)
        return fail(DSEE_BUSY, "Only one active instance is supported");
    try {
        auto resources = dsee::load_resources(root, int(profile));
        auto c = std::make_unique<dsee_context>();
        c->backend = dsee::make_backend(std::move(resources), rate, unsigned(profile));
        c->ratio = rate / 48000;
        active = c.release();
        *result = active;
        return DSEE_OK;
    } catch (const dsee::ProcessError &e) {
        return fail(DSEE_PROCESS_ERROR, e.what());
    } catch (const std::bad_alloc &) {
        return fail(DSEE_INTERNAL_ERROR, "Runtime allocation failed");
    } catch (const std::exception &e) {
        return fail(DSEE_ASSET_ERROR, e.what());
    } catch (...) {
        return fail(DSEE_INTERNAL_ERROR, "Unknown initialization failure");
    }
}
dsee_status dsee_process(dsee_context *c, const int16_t *input, size_t frames, int32_t *out,
                         size_t capacity, size_t *consumed, size_t *produced) {
    if (consumed)
        *consumed = 0;
    if (produced)
        *produced = 0;
    Lock lock;
    if (!lock.owns)
        return fail(DSEE_BUSY, "Concurrent API call");
    error_text[0] = 0;
    if (!valid(c) || !consumed || !produced || !out || (!input && frames) ||
        frames > SIZE_MAX / sizeof(int16_t) / 2 || capacity > SIZE_MAX / sizeof(int32_t) / 2)
        return fail(DSEE_INVALID_ARGUMENT, "Invalid process arguments");
    if (c->poisoned || c->finishing)
        return fail(DSEE_STATE_ERROR, "Instance cannot accept more input");
    size_t n = 1024 * c->ratio;
    if (capacity < n)
        return fail(DSEE_INVALID_ARGUMENT, "Output capacity smaller than one block");
    if (frames > UINT64_MAX - c->total_input)
        return fail(DSEE_INVALID_ARGUMENT, "Input count overflow");
    while (*consumed < frames && capacity - *produced >= n) {
        size_t take = std::min(frames - *consumed, 1024 - c->pending_frames);
        std::memcpy(c->pending.data() + 2 * c->pending_frames, input + 2 * (*consumed), take * 4);
        c->pending_frames += take;
        *consumed += take;
        c->total_input += take;
        if (c->pending_frames == 1024) {
            if (!block(c, out + 2 * (*produced)))
                return DSEE_PROCESS_ERROR;
            *produced += n;
        }
    }
    return DSEE_OK;
}
dsee_status dsee_finish(dsee_context *c, int32_t *out, size_t capacity, size_t *produced,
                        size_t *padding, int *done) {
    if (produced)
        *produced = 0;
    if (padding)
        *padding = 0;
    if (done)
        *done = 0;
    Lock lock;
    if (!lock.owns)
        return fail(DSEE_BUSY, "Concurrent API call");
    error_text[0] = 0;
    if (!valid(c) || !out || !produced || !padding || !done ||
        capacity > SIZE_MAX / sizeof(int32_t) / 2)
        return fail(DSEE_INVALID_ARGUMENT, "Invalid finish arguments");
    if (c->poisoned)
        return fail(DSEE_STATE_ERROR, "Instance is failed");
    size_t n = 1024 * c->ratio;
    if (capacity < n)
        return fail(DSEE_INVALID_ARGUMENT, "Output capacity smaller than one block");
    c->finishing = true;
    if (c->finished || !c->total_input) {
        c->finished = true;
        *done = 1;
        return DSEE_OK;
    }
    if (c->pending_frames) {
        *padding = 1024 - c->pending_frames;
        std::fill(c->pending.begin() + 2 * c->pending_frames, c->pending.end(), int16_t(0));
        if (!block(c, out))
            return DSEE_PROCESS_ERROR;
        *produced = n;
        return DSEE_OK;
    }
    std::fill(c->pending.begin(), c->pending.end(), int16_t(0));
    if (!block(c, out))
        return DSEE_PROCESS_ERROR;
    *produced = n;
    *padding = 1024;
    ++c->zero_blocks;
    c->quiet_blocks =
        std::all_of(out, out + 2 * n, [](int32_t x) { return x == 0; }) ? c->quiet_blocks + 1 : 0;
    if (c->quiet_blocks >= 4) {
        c->finished = true;
        *done = 1;
        return DSEE_OK;
    }
    if (c->zero_blocks >= 48) {
        c->poisoned = true;
        return fail(DSEE_TAIL_LIMIT, "Output did not become silent within 48 zero blocks");
    }
    return DSEE_OK;
}
dsee_status dsee_destroy(dsee_context *c) {
    Lock lock;
    if (!lock.owns)
        return fail(DSEE_BUSY, "Concurrent API call");
    error_text[0] = 0;
    if (!c)
        return DSEE_OK;
    if (!valid(c))
        return fail(DSEE_INVALID_ARGUMENT, "Invalid instance");
    dsee_status status = DSEE_OK;
    if (!c->poisoned) {
        try {
            c->backend->close();
        } catch (const std::exception &e) {
            status = fail(DSEE_PROCESS_ERROR, e.what());
        } catch (...) {
            status = fail(DSEE_PROCESS_ERROR, "Unknown destruction failure");
        }
    }
    active = nullptr;
    delete c;
    return status;
}
} // extern "C"
