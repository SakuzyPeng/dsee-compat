#include "dsee_compat.h"
#include "elf.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <setjmp.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
thread_local std::string error_text;
std::atomic_flag gate = ATOMIC_FLAG_INIT;
struct Lock {
    bool owns = !gate.test_and_set(std::memory_order_acquire);
    ~Lock() {
        if (owns)
            gate.clear(std::memory_order_release);
    }
};
struct Allocation {
    void *ptr;
    Allocation *next;
};
struct OpenFile {
    const uint8_t *data;
    size_t length, pos;
    FILE *file;
    OpenFile *next;
};
struct AssetSpec {
    const char *name;
    size_t size;
    const char *sha;
};
const AssetSpec library = {"vendor/lib64/libsonydseehxwrapper.so", 517392,
                           "511f7efcdf99cb00ca9553c23364e89c4eab91bae631b68d1df4dc116fead806"};
const AssetSpec dsx = {"vendor/etc/dsx_param_file.bin", 60000,
                       "dfc87d4b071c8b6a295690daefc2df889e827c393ae0411d54bcd14d5d3956f4"};
const AssetSpec parameters[] = {
    {"vendor/etc/DrangeparaPCM48.bin", 426576,
     "a915e3899841af760e40a8f4325c3522356f178a68e2a462dde39680947cfe45"},
    {"vendor/etc/DrangeparaAAC128.bin", 426576,
     "6094fc762f13976c6c1816e4c73dd3a4ef7e6bdec1011971f4a331639f25e48d"},
    {"vendor/etc/DrangeparaMP3_128.bin", 426576,
     "ebd94f7abcd86ecd5d97810b114890c177ce02b8e2bb2c3c92415665d7323aaf"}};
std::vector<uint8_t> read_asset(const char *root, const AssetSpec &s) {
    namespace fs = std::filesystem;
    auto dir = fs::canonical(root), path = fs::canonical(dir / s.name);
    auto rel = path.lexically_relative(dir);
    if (rel.empty() || *rel.begin() == ".." || !fs::is_regular_file(path) ||
        fs::file_size(path) != s.size)
        throw std::runtime_error(std::string("Invalid resource path/size: ") + s.name);
    std::vector<uint8_t> b(s.size);
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(b.data()), b.size()) || in.peek() != EOF)
        throw std::runtime_error(std::string("Cannot read resource: ") + s.name);
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(b.data(), CC_LONG(b.size()), hash);
    char text[65];
    for (size_t i = 0; i < 32; ++i)
        std::snprintf(text + 2 * i, 3, "%02x", hash[i]);
    if (std::strcmp(text, s.sha))
        throw std::runtime_error(std::string("Unsupported resource SHA-256: ") + s.name);
    return b;
}
} // namespace
struct dsee_context {
    std::map<std::string, std::vector<uint8_t>> assets;
    std::unique_ptr<dsee::Image> image;
    void *object = nullptr;
    using Process = int (*)(void *, const int16_t *, int32_t *, size_t, size_t *, int);
    Process process = nullptr;
    void (*destruct)(void *) = nullptr;
    unsigned ratio = 0;
    std::array<int16_t, 2048> pending{};
    size_t pending_frames = 0;
    uint64_t total_input = 0;
    bool finishing = false, finished = false, poisoned = false, guarded = false;
    unsigned zero_blocks = 0, quiet_blocks = 0;
    Allocation *allocations = nullptr;
    OpenFile *files = nullptr;
    sigjmp_buf jump;
    char fault[256]{};
};
namespace {
dsee_context *active = nullptr;
thread_local dsee_context *current = nullptr;
template <class T> T field(void *p, size_t offset) {
    T v;
    std::memcpy(&v, static_cast<char *>(p) + offset, sizeof(T));
    return v;
}
[[noreturn]] void native_fault(const char *why) {
    // Only used within guarded foreign calls. No C++ destructors cross this jump.
    if (!current || !current->guarded)
        std::abort();
    std::snprintf(current->fault, sizeof(current->fault), "%s", why);
    siglongjmp(current->jump, 1);
}
void *track(void *p) {
    if (!p)
        native_fault("Native allocation failed");
    auto *n = static_cast<Allocation *>(std::malloc(sizeof(Allocation)));
    if (!n) {
        std::free(p);
        native_fault("Allocation bookkeeping failed");
    }
    n->ptr = p;
    n->next = current->allocations;
    current->allocations = n;
    return p;
}
void *native_malloc(size_t n) {
    if (n > 128 * 1024 * 1024)
        native_fault("Native allocation exceeds limit");
    return track(std::malloc(n ? n : 1));
}
void *native_calloc(size_t n, size_t s) {
    if (!n || !s)
        return native_malloc(1);
    if (s && n > 128 * 1024 * 1024 / s)
        native_fault("Native allocation exceeds limit");
    return track(std::calloc(n, s));
}
void native_free(void *p) {
    if (!p)
        return;
    Allocation **n = &current->allocations;
    while (*n && (*n)->ptr != p)
        n = &(*n)->next;
    if (!*n)
        native_fault("Native free outside allocation set");
    auto *old = *n;
    *n = old->next;
    std::free(p);
    std::free(old);
}
int native_posix_memalign(void **p, size_t alignment, size_t n) {
    if (!p || !alignment || (alignment & (alignment - 1)) || alignment < sizeof(void *))
        return EINVAL;
    if (n > 128 * 1024 * 1024)
        return ENOMEM;
    int e = posix_memalign(p, alignment, n ? n : 1);
    if (!e)
        track(*p);
    return e;
}
void *native_memalign(size_t a, size_t n) {
    void *p = nullptr;
    if (native_posix_memalign(&p, a, n))
        native_fault("Native aligned allocation failed");
    return p;
}
uintptr_t noop() {
    return 0;
}
uintptr_t unsupported() {
    native_fault("Unimplemented Android interface invoked");
}
int native_log(int priority, const char *, const char *) {
    if (priority >= 6)
        native_fault("Sony library reported an error");
    return 0;
}
void native_assert(const char *, int, const char *, const char *) {
    native_fault("Sony library assertion failed");
}
int property_get(const char *, char *v, const char *fallback) {
    const char *s = fallback ? fallback : "";
    size_t n = std::strlen(s);
    if (n >= 92)
        native_fault("Property default too long");
    std::memcpy(v, s, n + 1);
    return int(n);
}
int property_int(const char *, int fallback) {
    return fallback;
}
void native_sincos(double x, double *s, double *c) {
    *s = std::sin(x);
    *c = std::cos(x);
}
int32_t to_i32(float x) {
    if (!(x > -1.f))
        return INT32_MIN;
    if (x >= 1.f)
        return INT32_MAX;
    float v = x * 2147483648.f;
    return int32_t(double(v) + (v > 0.f ? 0.5 : -0.5));
}
void audio_copy(void *dest, int df, const void *src, int sf, size_t n) {
    if (df == 5 && sf == 1) {
        auto *d = static_cast<float *>(dest);
        auto *s = static_cast<const int16_t *>(src);
        for (size_t i = 0; i < n; ++i)
            d[i] = s[i] / 32768.f;
    } else if (df == 3 && sf == 5) {
        auto *d = static_cast<int32_t *>(dest);
        auto *s = static_cast<const float *>(src);
        for (size_t i = 0; i < n; ++i)
            d[i] = to_i32(s[i]);
    } else
        native_fault("Unsupported native PCM conversion");
}
struct BionicVa {
    void *stack, *gr_top, *vr_top;
    int gr_offs, vr_offs;
};
int format_path(char *dst, size_t n, int, size_t bound, const char *fmt, BionicVa *ap) {
    if (std::strcmp(fmt, "/vendor/etc/%s") || n > bound)
        native_fault("Unsupported native format call");
    auto arg = *reinterpret_cast<const char **>(
        ap->gr_offs < 0 ? static_cast<char *>(ap->gr_top) + ap->gr_offs : ap->stack);
    return std::snprintf(dst, n, "/vendor/etc/%s", arg);
}
int file_read(void *cookie, char *out, int n) {
    auto *c = static_cast<OpenFile *>(cookie);
    size_t count = std::min(size_t(n), c->length - c->pos);
    std::memcpy(out, c->data + c->pos, count);
    c->pos += count;
    return int(count);
}
fpos_t file_seek(void *cookie, fpos_t offset, int whence) {
    auto *c = static_cast<OpenFile *>(cookie);
    int64_t start = whence == SEEK_SET   ? 0
                    : whence == SEEK_CUR ? int64_t(c->pos)
                    : whence == SEEK_END ? int64_t(c->length)
                                         : -1,
            end;
    if (start < 0 || __builtin_add_overflow(start, int64_t(offset), &end) || end < 0 ||
        uint64_t(end) > c->length) {
        errno = EINVAL;
        return -1;
    }
    c->pos = size_t(end);
    return fpos_t(end);
}
int file_close(void *cookie) {
    auto *c = static_cast<OpenFile *>(cookie);
    OpenFile **p = &current->files;
    while (*p && *p != c)
        p = &(*p)->next;
    if (*p)
        *p = c->next;
    std::free(c);
    return 0;
}
FILE *native_fopen(const char *path, const char *mode) {
    if (std::strcmp(mode, "r") && std::strcmp(mode, "rb"))
        native_fault("Native write access denied");
    static const char cpu[] = "Features : fp asimd neon\n";
    const uint8_t *data = nullptr;
    size_t size = 0;
    if (!std::strcmp(path, "/proc/cpuinfo")) {
        data = reinterpret_cast<const uint8_t *>(cpu);
        size = sizeof(cpu) - 1;
    } else {
        auto it = current->assets.find(path);
        if (it == current->assets.end())
            native_fault("Native resource not in verified set");
        data = it->second.data();
        size = it->second.size();
    }
    auto *f = static_cast<OpenFile *>(std::calloc(1, sizeof(OpenFile)));
    if (!f)
        native_fault("Resource reader allocation failed");
    f->data = data;
    f->length = size;
    f->next = current->files;
    f->file = funopen(f, file_read, nullptr, file_seek, file_close);
    if (!f->file) {
        std::free(f);
        native_fault("Cannot open memory resource");
    }
    current->files = f;
    return f->file;
}
void *resolve(const std::string &s) {
#define MAP(name, fn)                                                                              \
    if (s == name)                                                                                 \
    return reinterpret_cast<void *>(fn)
    MAP("malloc", native_malloc);
    MAP("calloc", native_calloc);
    MAP("free", native_free);
    MAP("_Znwm", native_malloc);
    MAP("_Znam", native_malloc);
    MAP("_ZdlPv", native_free);
    MAP("_ZdaPv", native_free);
    MAP("posix_memalign", native_posix_memalign);
    MAP("memalign", native_memalign);
    MAP("fopen", native_fopen);
    MAP("__android_log_print", native_log);
    MAP("__assert2", native_assert);
    MAP("property_get", property_get);
    MAP("property_get_int32", property_int);
    MAP("sincos", native_sincos);
    MAP("memcpy_by_audio_format", audio_copy);
    MAP("__vsnprintf_chk", format_path);
    MAP("__cxa_finalize", noop);
    MAP("atrace_get_enabled_tags", noop);
    MAP("atrace_begin_body", noop);
    MAP("atrace_end_body", noop);
#undef MAP
    if (s == "stderr")
        return &stderr;
    static const char *blocked[] = {"__stack_chk_fail",
                                    "__cxa_pure_virtual",
                                    "__android_log_assert",
                                    "__write_chk",
                                    "ditherAndClamp",
                                    "pthread_once",
                                    "pthread_mutex_lock",
                                    "pthread_mutex_unlock",
                                    "dprintf",
                                    "fprintf",
                                    "open",
                                    "close",
                                    "dlopen",
                                    "dlsym",
                                    "dlclose",
                                    "dlerror"};
    for (auto n : blocked)
        if (s == n)
            return reinterpret_cast<void *>(unsupported);
    static const char *allowed[] = {"fread",   "fclose",  "fwrite", "memcpy", "memset",
                                    "memmove", "strtoul", "strstr", "pow",    "cos",
                                    "sin",     "log10",   "log10f", "powf",   "exp2f"};
    for (auto n : allowed)
        if (s == n) {
            auto p = dlsym(RTLD_DEFAULT, n);
            if (p)
                return p;
        }
    throw std::runtime_error("Unsupported native import: " + s);
}
enum class Call { Construct, Process, Destruct };
bool invoke(dsee_context *c, Call op, const int16_t *in = nullptr, int32_t *out = nullptr,
            unsigned rate = 0, dsee_profile profile = DSEE_PCM) {
    // No automatic object with a destructor is created between setjmp and the foreign call.
    current = c;
    c->guarded = true;
    c->fault[0] = 0;
    if (sigsetjmp(c->jump, 1)) {
        c->guarded = false;
        current = nullptr;
        c->poisoned = true;
        error_text = c->fault;
        return false;
    }
    if (op == Call::Construct) {
        auto ctor = reinterpret_cast<void (*)(void *, int, int, int, int)>(
            c->image->symbol("_ZN7android10SonyDseeHxC2Eiiii"));
        ctor(c->object, 48000, int(rate),
             profile == DSEE_PCM      ? 1
             : profile == DSEE_AAC128 ? 0x4000000
                                      : 0x1000000,
             profile == DSEE_PCM ? -1 : 128000);
    } else if (op == Call::Process) {
        size_t done = 0;
        if (c->process(c->object, in, out, 1024, &done, 0) || done != 1024)
            native_fault("Sony processing did not consume one complete block");
    } else
        c->destruct(c->object);
    c->guarded = false;
    current = nullptr;
    return true;
}
void cleanup(dsee_context *c) {
    if (!c)
        return;
    current = c;
    while (c->files)
        std::fclose(c->files->file);
    while (c->allocations) {
        auto *n = c->allocations;
        c->allocations = n->next;
        std::free(n->ptr);
        std::free(n);
    }
    std::free(c->object);
    c->object = nullptr;
    current = nullptr;
}
dsee_status fail(dsee_status code, const std::string &s) {
    error_text = s;
    return code;
}
bool valid(dsee_context *c) {
    return c && c == active;
}
bool block(dsee_context *c, int32_t *out) {
    if (!invoke(c, Call::Process, c->pending.data(), out))
        return false;
    void *state = field<void *>(c->object, 8);
    if (!state || field<int32_t>(state, 23488) || field<int32_t>(state, 26056) ||
        field<int32_t>(state, 27952)) {
        c->poisoned = true;
        error_text = "Native processing state failed";
        return false;
    }
    c->pending_frames = 0;
    return true;
}
} // namespace
extern "C" {
const char *dsee_last_error() {
    return error_text.c_str();
}
const char *dsee_version() {
    return "0.1.0";
}
dsee_status dsee_inspect(const char *root) {
    Lock lock;
    if (!lock.owns)
        return fail(DSEE_BUSY, "Concurrent API call");
    error_text.clear();
    if (!root || !*root)
        return fail(DSEE_INVALID_ARGUMENT, "Resource directory required");
    try {
        auto b = read_asset(root, library);
        dsee::Elf elf(std::move(b));
        read_asset(root, dsx);
        for (auto &p : parameters)
            read_asset(root, p);
        return DSEE_OK;
    } catch (const std::exception &e) {
        return fail(DSEE_ASSET_ERROR, e.what());
    }
}
dsee_status dsee_create(const char *root, unsigned rate, dsee_profile profile,
                        dsee_context **result) {
    if (result)
        *result = nullptr;
    Lock lock;
    if (!lock.owns)
        return fail(DSEE_BUSY, "Concurrent API call");
    error_text.clear();
    if (!result || !root || !*root || (rate != 48000 && rate != 96000 && rate != 192000) ||
        profile < DSEE_PCM || profile > DSEE_MP3128)
        return fail(DSEE_INVALID_ARGUMENT, "Invalid creation arguments");
    if (active)
        return fail(DSEE_BUSY, "Only one active instance is supported");
    std::unique_ptr<dsee_context> c;
    try {
        c = std::make_unique<dsee_context>();
        auto b = read_asset(root, library);
        c->assets.emplace("/" + std::string(dsx.name), read_asset(root, dsx));
        auto &p = parameters[profile];
        c->assets.emplace("/" + std::string(p.name), read_asset(root, p));
        dsee::Elf elf(std::move(b));
        c->image = std::make_unique<dsee::Image>(elf, resolve);
        if (c->image->tls_patches != 43)
            throw std::runtime_error("Unexpected TLS patch count");
        c->process = reinterpret_cast<dsee_context::Process>(
            c->image->symbol("_ZN7android10SonyDseeHx7processEPKsPimPmb"));
        c->destruct =
            reinterpret_cast<void (*)(void *)>(c->image->symbol("_ZN7android10SonyDseeHxD2Ev"));
        c->image->symbol("_ZN7android10SonyDseeHxC2Eiiii");
        c->object = std::calloc(1, 120);
        if (!c->object)
            throw std::bad_alloc();
        c->ratio = rate / 48000;
        if (!invoke(c.get(), Call::Construct, nullptr, nullptr, rate, profile)) {
            cleanup(c.get());
            return DSEE_PROCESS_ERROR;
        }
        auto *state = field<void *>(c->object, 8);
        if (!state || !field<void *>(c->object, 16) || !field<void *>(c->object, 40) ||
            !field<void *>(c->object, 48) || field<int32_t>(state, 23488))
            throw std::runtime_error("Native initialization failed");
        active = c.release();
        *result = active;
        return DSEE_OK;
    } catch (const std::exception &e) {
        cleanup(c.get());
        return fail(DSEE_ASSET_ERROR, e.what());
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
    error_text.clear();
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
    error_text.clear();
    if (!valid(c) || !out || !produced || !padding || !done)
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
        std::fill(c->pending.begin() + 2 * c->pending_frames, c->pending.end(), 0);
        if (!block(c, out))
            return DSEE_PROCESS_ERROR;
        *produced = n;
        return DSEE_OK;
    }
    std::fill(c->pending.begin(), c->pending.end(), 0);
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
    error_text.clear();
    if (!c)
        return DSEE_OK;
    if (!valid(c))
        return fail(DSEE_INVALID_ARGUMENT, "Invalid instance");
    bool success = true;
    if (!c->poisoned && c->object)
        success = invoke(c, Call::Destruct);
    cleanup(c);
    active = nullptr;
    delete c;
    return success ? DSEE_OK : DSEE_PROCESS_ERROR;
}
}
