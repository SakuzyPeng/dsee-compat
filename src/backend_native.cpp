#include "backend.hpp"
#include "dsee_compat.h"
#include "elf.hpp"
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
struct NativeState {
    std::map<std::string, std::vector<uint8_t>> assets;
    std::unique_ptr<dsee::Image> image;
    void *object = nullptr;
    using Process = int (*)(void *, const int16_t *, int32_t *, size_t, size_t *, int);
    Process process = nullptr;
    void (*destruct)(void *) = nullptr;
    bool poisoned = false, guarded = false;
    Allocation *allocations = nullptr;
    OpenFile *files = nullptr;
    sigjmp_buf jump;
    char fault[256]{};
};
thread_local NativeState *current = nullptr;
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
    size_t bytes = n * s;
    // The hash-pinned wrapper accesses beyond its nominal 110000-byte state.
    // Reserve the 112 KiB arena supplied by the validated macOS allocator
    // explicitly, instead of depending on unspecified heap slack. Other
    // allocation sizes retain their requested bounds for memory checking.
    if (n == 1 && s == 110000)
        bytes = 112 * 1024;
    return track(std::calloc(1, bytes));
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
size_t read_memory_file(void *cookie, char *out, size_t n) {
    auto *c = static_cast<OpenFile *>(cookie);
    size_t count = std::min(size_t(n), c->length - c->pos);
    std::memcpy(out, c->data + c->pos, count);
    c->pos += count;
    return count;
}
int64_t seek_memory_file(void *cookie, int64_t offset, int whence) {
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
    return end;
}
#if defined(__APPLE__)
int file_read(void *cookie, char *out, int n) {
    return int(read_memory_file(cookie, out, size_t(n)));
}
fpos_t file_seek(void *cookie, fpos_t offset, int whence) {
    return fpos_t(seek_memory_file(cookie, int64_t(offset), whence));
}
#else
ssize_t file_read(void *cookie, char *out, size_t n) {
    return ssize_t(
        read_memory_file(cookie, out, std::min(n, size_t(std::numeric_limits<ssize_t>::max()))));
}
int file_seek(void *cookie, off64_t *offset, int whence) {
    int64_t end = seek_memory_file(cookie, *offset, whence);
    if (end < 0)
        return -1;
    *offset = end;
    return 0;
}
#endif
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
#if defined(__APPLE__)
    f->file = funopen(f, file_read, nullptr, file_seek, file_close);
#else
    cookie_io_functions_t io{};
    io.read = file_read;
    io.seek = file_seek;
    io.close = file_close;
    f->file = fopencookie(f, mode, io);
#endif
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
bool invoke(NativeState *c, Call op, const int16_t *in = nullptr, int32_t *out = nullptr,
            unsigned rate = 0, dsee_profile profile = DSEE_PCM) {
    // No automatic object with a destructor is created between setjmp and the foreign call.
    current = c;
    c->guarded = true;
    c->fault[0] = 0;
    if (sigsetjmp(c->jump, 1)) {
        c->guarded = false;
        current = nullptr;
        c->poisoned = true;
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
void cleanup(NativeState *c) {
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
class NativeBackend final : public dsee::Backend {
    NativeState state;

  public:
    ~NativeBackend() override {
        cleanup(&state);
    }
    void initialize(dsee::Resources resources, unsigned rate, unsigned profile) {
        state.assets.emplace("/" + std::string(dsee::dsx.name), std::move(resources.bytes[1]));
        state.assets.emplace("/" + std::string(dsee::parameters[profile].name),
                             std::move(resources.bytes[profile + 2]));
        dsee::Elf elf(std::move(resources.bytes[0]));
        state.image = std::make_unique<dsee::Image>(elf, resolve);
        if (state.image->tls_patches != 43)
            throw std::runtime_error("Unexpected TLS patch count");
        state.process = reinterpret_cast<NativeState::Process>(
            state.image->symbol("_ZN7android10SonyDseeHx7processEPKsPimPmb"));
        state.destruct =
            reinterpret_cast<void (*)(void *)>(state.image->symbol("_ZN7android10SonyDseeHxD2Ev"));
        state.image->symbol("_ZN7android10SonyDseeHxC2Eiiii");
        state.object = std::calloc(1, 120);
        if (!state.object)
            throw std::bad_alloc();
        if (!invoke(&state, Call::Construct, nullptr, nullptr, rate, dsee_profile(profile)))
            throw dsee::ProcessError(state.fault);
        auto *s = field<void *>(state.object, 8);
        if (!s || !field<void *>(state.object, 16) || !field<void *>(state.object, 40) ||
            !field<void *>(state.object, 48) || field<int32_t>(s, 23488))
            throw dsee::ProcessError("Native initialization failed");
    }
    void process_block(const int16_t *input, int32_t *output) override {
        if (!invoke(&state, Call::Process, input, output))
            throw dsee::ProcessError(state.fault);
        auto *s = field<void *>(state.object, 8);
        if (!s || field<int32_t>(s, 23488) || field<int32_t>(s, 26056) || field<int32_t>(s, 27952))
            throw dsee::ProcessError("Native processing state failed");
    }
    void close() override {
        if (!invoke(&state, Call::Destruct))
            throw dsee::ProcessError(state.fault);
    }
};
} // namespace
namespace dsee {
std::unique_ptr<Backend> make_backend(Resources resources, unsigned rate, unsigned profile) {
    auto backend = std::make_unique<NativeBackend>();
    backend->initialize(std::move(resources), rate, profile);
    return backend;
}
} // namespace dsee
