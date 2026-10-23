#include "emulation.hpp"
#include "elf.hpp"
#include "emulation_cpu.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
namespace dsee {
namespace {
using namespace emulation;
void require(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}
void require(bool ok, const std::string &text) {
    if (!ok)
        throw std::runtime_error(text);
}
constexpr uint64_t image_base = 0x10000000, stub_base = 0x20000000, tls_base = 0x21000000,
                   stack_base = 0x30000000, heap_base = 0x40000000, halt = stub_base + 0xff0;
constexpr size_t page_size = 4096, stack_size = 1024 * 1024, heap_size = 16 * 1024 * 1024;
uint64_t round_up(uint64_t value, uint64_t alignment) {
    require(alignment && !(alignment & (alignment - 1)) && value <= UINT64_MAX - alignment + 1,
            "Invalid alignment");
    return (value + alignment - 1) & ~(alignment - 1);
}
uint64_t plus_signed(uint64_t value, int64_t offset) {
    if (offset >= 0) {
        require(value <= UINT64_MAX - uint64_t(offset), "Relocation overflow");
        return value + uint64_t(offset);
    }
    uint64_t magnitude = uint64_t(-(offset + 1)) + 1;
    require(value >= magnitude, "Relocation underflow");
    return value - magnitude;
}
int32_t to_i32(float x) {
    if (!(x > -1.f))
        return INT32_MIN;
    if (x >= 1.f)
        return INT32_MAX;
    float v = x * 2147483648.f;
    return int32_t(double(v) + (v > 0.f ? 0.5 : -0.5));
}
class Machine {
    struct Region {
        uint64_t base;
        Bytes bytes;
        std::vector<unsigned> permissions;
    };
    struct File {
        const Bytes *bytes;
        size_t pos = 0;
    };
    std::unique_ptr<dsee::emulation::Cpu> cpu;
    std::vector<Region> regions;
    std::map<uint64_t, size_t> free_ranges, allocations;
    std::map<uint64_t, File> files;
    std::map<std::string, const Bytes *> assets;
    Bytes cpuinfo;
    std::vector<std::string> imports;
    std::map<std::string, uint64_t> symbols;
    uint64_t next_file = 0x70000000;
    uint64_t import_calls = 0;

    uint64_t x(unsigned i) const {
        return cpu->reg(i);
    }
    void result(uint64_t v) {
        cpu->reg(0, v);
    }
    template <class T> T fp(unsigned i = 0) const {
        T value{};
        auto vector = cpu->vector(i);
        std::memcpy(&value, vector.data(), sizeof(T));
        return value;
    }
    template <class T> void fp_result(T value) {
        dsee::emulation::Vector vector{};
        std::memcpy(vector.data(), &value, sizeof(T));
        cpu->vector(0, vector);
    }
    size_t product(uint64_t a, uint64_t b) {
        require(!b || a <= heap_size / b, "Import byte count exceeds guest arena");
        return size_t(a * b);
    }
    void dispatch(const std::string &s) {
        uint64_t a = x(0), b = x(1), c = x(2), d = x(3), e = x(4);
        if (s == "malloc" || s == "_Znwm" || s == "_Znam")
            result(allocate(size_t(a)));
        else if (s == "calloc") {
            size_t n = product(a, b);
            if (a == 1 && b == 110000)
                n = 112 * 1024;
            uint64_t p = allocate(n);
            std::memset(pointer(p, std::max(n, size_t(1)), true), 0, std::max(n, size_t(1)));
            result(p);
        } else if (s == "free" || s == "_ZdlPv" || s == "_ZdaPv") {
            release(a);
            result(0);
        } else if (s == "posix_memalign") {
            require(b >= 8 && !(b & (b - 1)), "Unsupported allocation alignment");
            set<uint64_t>(a, allocate(size_t(c), size_t(b)));
            result(0);
        } else if (s == "memalign") {
            require(a >= 8 && !(a & (a - 1)), "Unsupported allocation alignment");
            result(allocate(size_t(b), size_t(a)));
        } else if (s == "memcpy" || s == "memmove") {
            std::memmove(pointer(a, size_t(c), true), pointer(b, size_t(c)), size_t(c));
            result(a);
        } else if (s == "memset") {
            std::memset(pointer(a, size_t(c), true), int(b), size_t(c));
            result(a);
        } else if (s == "strstr") {
            auto first = string(a), second = string(b);
            auto pos = first.find(second);
            result(pos == std::string::npos ? 0 : a + pos);
        } else if (s == "strtoul") {
            auto text = string(a);
            char *end = nullptr;
            auto n = std::strtoull(text.c_str(), &end, int(c));
            if (b)
                set<uint64_t>(b, a + size_t(end - text.c_str()));
            result(n);
        } else if (s == "fopen") {
            auto path = string(a), mode = string(b);
            require(mode == "r" || mode == "rb", "Guest file write denied");
            auto it = assets.find(path);
            require(it != assets.end(), "Guest file not in verified resource set");
            auto handle = next_file++;
            files.emplace(handle, File{it->second});
            result(handle);
        } else if (s == "fread") {
            auto it = files.find(d);
            require(it != files.end(), "Invalid guest file");
            auto &f = it->second;
            size_t n = std::min(product(b, c), f.bytes->size() - f.pos);
            if (n)
                std::memcpy(pointer(a, n, true), f.bytes->data() + f.pos, n);
            f.pos += n;
            result(b ? n / b : 0);
        } else if (s == "fclose") {
            require(files.erase(a) == 1, "Invalid guest close");
            result(0);
        } else if (s == "property_get") {
            auto value = c ? string(c) : std::string();
            require(value.size() < 92, "Property too long");
            std::memcpy(pointer(b, value.size() + 1, true), value.c_str(), value.size() + 1);
            result(value.size());
        } else if (s == "property_get_int32")
            result(uint32_t(b));
        else if (s == "__vsnprintf_chk") {
            require(string(e) == "/vendor/etc/%s" && b <= d, "Unsupported guest format");
            auto va = x(5);
            int32_t offset = get<int32_t>(va + 24);
            uint64_t arg =
                offset < 0 ? plus_signed(get<uint64_t>(va + 8), offset) : get<uint64_t>(va);
            auto path = std::string("/vendor/etc/") + string(get<uint64_t>(arg));
            if (b) {
                size_t n = std::min(size_t(b - 1), path.size());
                std::memcpy(pointer(a, n + 1, true), path.data(), n);
                set<uint8_t>(a + n, 0);
            }
            result(path.size());
        } else if (s == "memcpy_by_audio_format") {
            require((b == 5 && d == 1) || (b == 3 && d == 5), "Unsupported PCM conversion");
            size_t n = product(e, 1);
            auto *out = pointer(a, product(n, 4), true);
            auto *in = pointer(c, product(n, d == 1 ? 2 : 4));
            for (size_t i = 0; i < n; ++i) {
                if (d == 1) {
                    int16_t sample;
                    std::memcpy(&sample, in + 2 * i, 2);
                    float value = sample / 32768.f;
                    std::memcpy(out + 4 * i, &value, 4);
                } else {
                    float value;
                    std::memcpy(&value, in + 4 * i, 4);
                    int32_t sample = to_i32(value);
                    std::memcpy(out + 4 * i, &sample, 4);
                }
            }
            result(0);
        } else if (s == "sincos") {
            auto value = fp<double>();
            set<double>(a, std::sin(value));
            set<double>(b, std::cos(value));
        } else if (s == "pow")
            fp_result(std::pow(fp<double>(), fp<double>(1)));
        else if (s == "cos")
            fp_result(std::cos(fp<double>()));
        else if (s == "sin")
            fp_result(std::sin(fp<double>()));
        else if (s == "log10")
            fp_result(std::log10(fp<double>()));
        else if (s == "log10f")
            fp_result(std::log10(fp<float>()));
        else if (s == "powf")
            fp_result(std::pow(fp<float>(), fp<float>(1)));
        else if (s == "exp2f")
            fp_result(std::exp2(fp<float>()));
        else if (s == "__android_log_print") {
            require(int32_t(a) < 6, "Guest reported an error");
            result(0);
        } else if (s == "__cxa_finalize" || s == "atrace_get_enabled_tags" ||
                   s == "atrace_begin_body" || s == "atrace_end_body")
            result(0);
        else
            throw std::runtime_error("Unsupported guest interface invoked: " + s);
    }

  public:
    Machine() {
        cpu = std::make_unique<dsee::emulation::Cpu>(
            [this](size_t index) {
                require(index < imports.size(), "Unknown import entry");
                ++import_calls;
                dispatch(imports[index]);
            },
            [this](uint64_t address, size_t size, unsigned permissions) {
                return access(address, size, permissions);
            },
            stub_base, halt, tls_base);
        try {
            map(stub_base, page_size);
            map(tls_base, page_size);
            map(stack_base, stack_size);
            map(heap_base, heap_size);
            free_ranges.emplace(heap_base, heap_size);
            set<uint64_t>(tls_base + 40, 0x9172536485a6b7c8ULL);
            cpu->stop_stub(pointer(halt, 4, true));
            const char value[] = "Features : fp asimd neon\n";
            cpuinfo.assign(value, value + sizeof(value) - 1);
            assets.emplace("/proc/cpuinfo", &cpuinfo);
        } catch (...) {
            cpu.reset();
            throw;
        }
    }
    ~Machine() {
        cpu.reset();
    }
    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;
    void map(uint64_t base, size_t size) {
        require(base % page_size == 0 && size % page_size == 0, "Unaligned guest map");
        regions.push_back({base, Bytes(size), std::vector<unsigned>(size / page_size, All)});
        auto &r = regions.back();
        cpu->map(base, size, r.bytes.data(), Read | Write);
        std::fill(r.permissions.begin(), r.permissions.end(), Read | Write);
    }
    uint8_t *pointer(uint64_t address, size_t size, bool write = false) {
        return access(address, size, write ? Write : Read);
    }
    uint8_t *access(uint64_t address, size_t size, unsigned permissions) {
        for (auto &r : regions) {
            if (address < r.base || address - r.base > r.bytes.size())
                continue;
            size_t offset = size_t(address - r.base);
            if (size > r.bytes.size() - offset)
                continue;
            if (size)
                for (size_t p = offset / page_size; p <= (offset + size - 1) / page_size; ++p)
                    require((r.permissions[p] & permissions) == permissions,
                            "Host bridge guest-memory protection violation");
            return r.bytes.data() + offset;
        }
        throw std::runtime_error("Host bridge guest pointer outside mapped memory");
    }
    void protect(uint64_t base, size_t size, unsigned permissions) {
        require(base % page_size == 0 && size % page_size == 0, "Unaligned guest protection");
        for (auto &r : regions)
            if (base >= r.base && base - r.base <= r.bytes.size() &&
                size <= r.bytes.size() - (base - r.base)) {
                cpu->protect(base, size, permissions);
                size_t first = size_t(base - r.base) / page_size;
                std::fill_n(r.permissions.begin() + first, size / page_size, permissions);
                return;
            }
        throw std::runtime_error("Invalid guest protection region");
    }
    template <class T> T get(uint64_t address) {
        T value;
        std::memcpy(&value, pointer(address, sizeof(T)), sizeof(T));
        return value;
    }
    template <class T> void set(uint64_t address, T value) {
        std::memcpy(pointer(address, sizeof(T), true), &value, sizeof(T));
    }
    std::string string(uint64_t address) {
        std::string value;
        for (size_t i = 0; i < 4096; ++i) {
            auto c = get<char>(address + i);
            if (!c)
                return value;
            value += c;
        }
        throw std::runtime_error("Unterminated guest string");
    }
    uint64_t allocate(size_t n, size_t alignment = 16) {
        require(n <= 128 * 1024 * 1024 && alignment <= heap_size, "Guest allocation limit");
        n = std::max(n, size_t(1));
        for (auto it = free_ranges.begin(); it != free_ranges.end(); ++it) {
            uint64_t start = it->first, end = start + it->second;
            uint64_t p = round_up(start, alignment);
            if (p > end || n > end - p)
                continue;
            free_ranges.erase(it);
            if (p > start)
                free_ranges.emplace(start, size_t(p - start));
            if (p + n < end)
                free_ranges.emplace(p + n, size_t(end - p - n));
            allocations.emplace(p, n);
            return p;
        }
        throw std::runtime_error("Guest heap exhausted");
    }
    void release(uint64_t p) {
        if (!p)
            return;
        auto it = allocations.find(p);
        require(it != allocations.end(), "Guest free outside allocation set");
        auto range = free_ranges.emplace(p, it->second).first;
        allocations.erase(it);
        auto next = std::next(range);
        if (next != free_ranges.end() && range->first + range->second == next->first) {
            range->second += next->second;
            free_ranges.erase(next);
        }
        if (range != free_ranges.begin()) {
            auto prev = std::prev(range);
            if (prev->first + prev->second == range->first) {
                prev->second += range->second;
                free_ranges.erase(range);
            }
        }
    }
    uint64_t stub(const std::string &name) {
        static const std::vector<std::string> allowed = {"malloc",
                                                         "calloc",
                                                         "free",
                                                         "_Znwm",
                                                         "_Znam",
                                                         "_ZdlPv",
                                                         "_ZdaPv",
                                                         "posix_memalign",
                                                         "memalign",
                                                         "fopen",
                                                         "fread",
                                                         "fclose",
                                                         "fwrite",
                                                         "memcpy",
                                                         "memmove",
                                                         "memset",
                                                         "strstr",
                                                         "strtoul",
                                                         "pow",
                                                         "cos",
                                                         "sin",
                                                         "log10",
                                                         "log10f",
                                                         "powf",
                                                         "exp2f",
                                                         "sincos",
                                                         "property_get",
                                                         "property_get_int32",
                                                         "__vsnprintf_chk",
                                                         "memcpy_by_audio_format",
                                                         "__android_log_print",
                                                         "__assert2",
                                                         "__cxa_finalize",
                                                         "atrace_get_enabled_tags",
                                                         "atrace_begin_body",
                                                         "atrace_end_body",
                                                         "__stack_chk_fail",
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
        require(std::find(allowed.begin(), allowed.end(), name) != allowed.end(),
                "Unknown guest import: " + name);
        auto it = std::find(imports.begin(), imports.end(), name);
        if (it != imports.end())
            return stub_base + size_t(it - imports.begin()) * cpu->stub_size();
        uint64_t address = stub_base + imports.size() * cpu->stub_size();
        require(address + cpu->stub_size() <= halt, "Too many imports");
        cpu->import_stub(pointer(address, cpu->stub_size(), true), address, imports.size());
        imports.push_back(name);
        return address;
    }
    uint64_t symbol(const std::string &name) {
        return symbols.at(name);
    }
    void load(const Resources &resources, unsigned profile) {
        require(profile < 3, "Invalid profile");
        assets.emplace("/" + std::string(dsee::dsx.name), &resources.bytes[1]);
        assets.emplace("/" + std::string(dsee::parameters[profile].name),
                       &resources.bytes[profile + 2]);
        dsee::Elf elf(resources.bytes[0]);
        map(image_base, size_t(round_up(elf.image_size, page_size)));
        auto inside = [&](uint64_t offset) {
            return std::any_of(elf.segments.begin(), elf.segments.end(), [&](auto &s) {
                return offset >= s.address && offset - s.address < s.memsz;
            });
        };
        std::vector<unsigned> perms(size_t(round_up(elf.image_size, page_size)) / page_size, 0);
        size_t tls_reads = 0;
        for (auto &s : elf.segments) {
            std::memcpy(pointer(image_base + s.address, size_t(s.filesz), true),
                        elf.bytes.data() + s.offset, size_t(s.filesz));
            if (s.memsz) {
                unsigned prot = (s.flags & 4 ? Read : 0) | (s.flags & 2 ? Write : 0) |
                                (s.flags & 1 ? Execute : 0);
                for (size_t p = size_t(s.address / page_size);
                     p <= (s.address + s.memsz - 1) / page_size; ++p)
                    perms[p] |= prot;
            }
            if (s.flags & 1) {
                require(s.address % 4 == 0 && s.filesz % 4 == 0, "Unaligned guest code");
                for (uint64_t at = s.address; at < s.address + s.filesz; at += 4)
                    if ((get<uint32_t>(image_base + at) & ~31U) == 0xd53bd040U)
                        ++tls_reads;
            }
        }
        require(tls_reads == 43, "Unexpected TLS instruction count");
        for (auto at : elf.relative) {
            auto offset = get<uint64_t>(image_base + at);
            require(inside(offset), "Invalid RELR destination");
            set<uint64_t>(image_base + at, image_base + offset);
        }
        for (auto &r : elf.relocs) {
            auto &s = elf.symbols[r.symbol];
            uint64_t value;
            if (r.type == 1027) {
                auto offset = plus_signed(0, r.addend);
                require(inside(offset), "Invalid relative addend");
                value = image_base + offset;
            } else {
                if (s.defined)
                    value = image_base + s.value;
                else if (s.name == "stderr") {
                    value = allocate(8);
                    set<uint64_t>(value, 0);
                } else
                    value = stub(s.name);
                value = plus_signed(value, r.addend);
            }
            set<uint64_t>(image_base + r.address, value);
        }
        for (auto &s : elf.symbols)
            if (s.defined)
                symbols.emplace(s.name, image_base + s.value);
        for (size_t p = 0; p < perms.size();) {
            auto prot = perms[p];
            require((prot & (Write | Execute)) != (Write | Execute),
                    "Guest page would be writable and executable");
            size_t end = p + 1;
            while (end < perms.size() && perms[end] == prot)
                ++end;
            protect(image_base + p * page_size, (end - p) * page_size, prot);
            p = end;
        }
        protect(stub_base, page_size, Read | Execute);
    }
    uint64_t run(uint64_t entry, std::initializer_list<uint64_t> args) {
        require(args.size() <= 8, "Too many guest arguments");
        std::array<uint64_t, 8> values{};
        std::copy(args.begin(), args.end(), values.begin());
        return cpu->run(entry, stack_base + stack_size - 16, values);
    }
    uint64_t calls() const {
        return import_calls;
    }
};
} // namespace
struct EmulatedProfile::Impl {
    Resources resources;
    Machine m;
    uint64_t object, input, output, done, process, destruct;
    unsigned ratio;

  public:
    Impl(Resources r, unsigned rate, unsigned profile)
        : resources(std::move(r)), ratio(rate / 48000) {
        require(rate == 48000 || rate == 96000 || rate == 192000, "Invalid output rate");
        m.load(resources, profile);
        try {
            object = m.allocate(120);
            std::memset(m.pointer(object, 120, true), 0, 120);
            input = m.allocate(4096);
            output = m.allocate(32768);
            done = m.allocate(8);
            process = m.symbol("_ZN7android10SonyDseeHx7processEPKsPimPmb");
            destruct = m.symbol("_ZN7android10SonyDseeHxD2Ev");
            m.run(m.symbol("_ZN7android10SonyDseeHxC2Eiiii"),
                  {object, 48000, rate,
                   profile == 0   ? 1U
                   : profile == 1 ? 0x4000000U
                                  : 0x1000000U,
                   profile == 0 ? uint64_t(UINT32_MAX) : 128000});
            auto state = m.get<uint64_t>(object + 8);
            require(state && m.get<uint64_t>(object + 16) && m.get<uint64_t>(object + 40) &&
                        m.get<uint64_t>(object + 48) && m.get<int32_t>(state + 23488) == 0,
                    "Guest initialization failed");
        } catch (const std::exception &e) {
            throw ProcessError(e.what());
        }
    }
    const int32_t *block(const int16_t *samples) {
        std::memcpy(m.pointer(input, 4096, true), samples, 4096);
        m.set<uint64_t>(done, 0);
        require(m.run(process, {object, input, output, 1024, done, 0}) == 0 &&
                    m.get<uint64_t>(done) == 1024,
                "Guest processing did not consume one block");
        auto state = m.get<uint64_t>(object + 8);
        require(state && !m.get<int32_t>(state + 23488) && !m.get<int32_t>(state + 26056) &&
                    !m.get<int32_t>(state + 27952),
                "Guest processing state failed");
        return reinterpret_cast<const int32_t *>(m.pointer(output, output_samples() * 4));
    }
    size_t output_samples() const {
        return 2048 * ratio;
    }
    uint64_t calls() const {
        return m.calls();
    }
    void close() {
        m.run(destruct, {object});
    }
};
EmulatedProfile::EmulatedProfile(Resources resources, unsigned rate, unsigned profile)
    : impl(std::make_unique<Impl>(std::move(resources), rate, profile)) {}
EmulatedProfile::~EmulatedProfile() = default;
const int32_t *EmulatedProfile::block(const int16_t *input) {
    return impl->block(input);
}
void EmulatedProfile::process_block(const int16_t *input, int32_t *output) {
    std::memcpy(output, block(input), output_samples() * sizeof(int32_t));
}
void EmulatedProfile::close() {
    impl->close();
}
size_t EmulatedProfile::output_samples() const {
    return impl->output_samples();
}
uint64_t EmulatedProfile::calls() const {
    return impl->calls();
}
template <class F> void rejects(F fn) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception &) {
        threw = true;
    }
    require(threw, "Expected rejection");
}
void emulation_self_test() {
    Machine m;
    m.map(image_base, page_size);
    m.set<uint32_t>(image_base, 0x8b010000);     // ADD X0, X0, X1, authored fixture.
    m.set<uint32_t>(image_base + 4, 0xd65f03c0); // RET.
    m.set<uint32_t>(image_base + 8, 0xf9000020); // STR X0, [X1].
    m.set<uint32_t>(image_base + 12, 0xd65f03c0);
    auto copy = m.stub("memcpy"), blocked = m.stub("dlopen");
    m.protect(stub_base, page_size, Read | Execute);
    m.protect(image_base, page_size, Read | Execute);
    require(m.run(image_base, {7, 9}) == 16, "Guest call ABI failed");
    auto a = m.allocate(64, 64), b = m.allocate(64);
    m.set<uint64_t>(a, 0x123456789abcdef0ULL);
    require(m.run(copy, {b, a, 8}) == b && m.get<uint64_t>(b) == m.get<uint64_t>(a),
            "Guest import bridge failed");
    rejects([&] { m.run(copy, {b, 0xdeadbeef, 8}); });
    rejects([&] { m.run(blocked, {}); });
    rejects([&] { m.run(image_base + 8, {42, image_base}); });
    rejects([&] { m.run(a, {}); }); // Data pages must not be executable.
    constexpr uint64_t boundary = 0x50000000;
    m.map(boundary, 2 * page_size);
    m.protect(boundary + page_size, page_size, Read);
    // An unaligned 64-bit store must not bypass the next page's protection.
    rejects([&] { m.run(image_base + 8, {0x1122334455667788ULL, boundary + page_size - 4}); });
    require(m.get<uint32_t>(boundary + page_size) == 0, "Protected page was modified");
    rejects([&] { m.stub("unknown_import"); });
    m.release(a);
    rejects([&] { m.release(a); });
    m.release(b);
    require(m.allocate(heap_size) == heap_base, "Guest heap coalescing failed");
    require(sha256("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 backend failed");
}
} // namespace dsee
