#include "elf.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#if !defined(DSEE_ELF_PARSER_ONLY)
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <unordered_set>
namespace dsee {
namespace {
[[noreturn]] void bad(const char *s) {
    throw std::runtime_error(s);
}
void bounds(uint64_t at, uint64_t n, uint64_t size) {
    if (at > size || n > size - at)
        bad("ELF range outside bounds");
}
uint64_t add(uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a)
        bad("ELF integer overflow");
    return a + b;
}
uint64_t plus_signed(uint64_t a, int64_t b) {
    if (b >= 0)
        return add(a, uint64_t(b));
    uint64_t n = uint64_t(-(b + 1)) + 1;
    if (n > a)
        bad("ELF negative offset");
    return a - n;
}
int64_t signed_add(int64_t a, int64_t b) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        bad("ELF addend overflow");
    return a + b;
}
struct Reader {
    const std::vector<uint8_t> &b;
    uint64_t u(uint64_t p, size_t n) const {
        bounds(p, n, b.size());
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i)
            v |= uint64_t(b[p + i]) << (8 * i);
        return v;
    }
    std::string str(uint64_t p, uint64_t end) const {
        bounds(p, 0, end);
        bounds(0, end, b.size());
        auto e = std::find(b.begin() + p, b.begin() + end, 0);
        if (e == b.begin() + end)
            bad("ELF unterminated string");
        return std::string(b.begin() + p, e);
    }
};
struct Section {
    uint32_t type, link;
    uint64_t off, size, entry;
};
bool loaded(const std::vector<Segment> &ss, uint64_t p, uint64_t n) {
    for (auto &s : ss)
        if (p >= s.address && p - s.address <= s.memsz && n <= s.memsz - (p - s.address))
            return true;
    return false;
}
} // namespace
Elf::Elf(std::vector<uint8_t> input) : bytes(std::move(input)) {
    Reader r{bytes};
    bounds(0, 64, bytes.size());
    if (r.u(0, 4) != 0x464c457f || r.u(4, 1) != 2 || r.u(5, 1) != 1 || r.u(6, 1) != 1 ||
        r.u(16, 2) != 3 || r.u(18, 2) != 183 || r.u(20, 4) != 1)
        bad("Expected AArch64 little-endian ELF64 DYN");
    uint64_t ph = r.u(32, 8), sh = r.u(40, 8), pn = r.u(56, 2), sn = r.u(60, 2);
    if (r.u(52, 2) != 64 || r.u(54, 2) != 56 || r.u(58, 2) != 64 || !pn || !sn || pn > 128 ||
        sn > 4096)
        bad("Unsupported ELF header layout");
    bounds(ph, pn * 56, bytes.size());
    bounds(sh, sn * 64, bytes.size());
    for (uint64_t i = 0; i < pn; ++i) {
        auto p = ph + i * 56;
        if (r.u(p, 4) != 1)
            continue;
        Segment s{r.u(p + 8, 8), r.u(p + 16, 8), r.u(p + 32, 8), r.u(p + 40, 8),
                  uint32_t(r.u(p + 4, 4))};
        if (s.filesz > s.memsz || (s.flags & 3) == 3 || s.flags > 7)
            bad("Invalid LOAD segment");
        bounds(s.offset, s.filesz, bytes.size());
        auto end = add(s.address, s.memsz);
        if (end > 16 * 1024 * 1024)
            bad("ELF image exceeds 16 MiB limit");
        for (auto &other : segments)
            if (s.memsz && other.memsz && s.address < other.address + other.memsz &&
                other.address < end)
                bad("Overlapping LOAD segments");
        segments.push_back(s);
        image_size = std::max(image_size, end);
    }
    if (!image_size)
        bad("ELF has no image");
    std::vector<Section> sections;
    size_t dyn = SIZE_MAX;
    for (uint64_t i = 0; i < sn; ++i) {
        auto p = sh + i * 64;
        Section s{uint32_t(r.u(p + 4, 4)), uint32_t(r.u(p + 40, 4)), r.u(p + 24, 8), r.u(p + 32, 8),
                  r.u(p + 56, 8)};
        if (s.type != 8)
            bounds(s.off, s.size, bytes.size());
        if (s.type == 11) {
            if (dyn != SIZE_MAX)
                bad("Multiple dynamic symbol tables");
            dyn = i;
        }
        sections.push_back(s);
    }
    if (dyn == SIZE_MAX)
        bad("Missing dynamic symbol table");
    auto &ds = sections[dyn];
    if (ds.entry != 24 || ds.size % 24 || ds.link >= sn)
        bad("Invalid dynamic symbol table");
    auto &st = sections[ds.link];
    if (st.type != 3)
        bad("Invalid dynamic string table");
    for (uint64_t p = ds.off; p < ds.off + ds.size; p += 24) {
        auto name = r.u(p, 4);
        if (name >= st.size)
            bad("Invalid symbol string offset");
        auto index = r.u(p + 6, 2);
        Symbol s{r.str(st.off + name, st.off + st.size), r.u(p + 8, 8), r.u(p + 16, 8), index != 0};
        if (s.defined && (index >= sn || !loaded(segments, s.value, s.size ? s.size : 1)))
            bad("Invalid defined symbol");
        symbols.push_back(std::move(s));
    }
    std::unordered_set<uint64_t> destinations;
    auto target = [&](uint64_t at) {
        if (at % 8 || !loaded(segments, at, 8) || !destinations.insert(at).second)
            bad("Invalid or duplicate relocation target");
    };
    auto relocation = [&](uint64_t at, uint64_t info, int64_t a) {
        uint32_t type = uint32_t(info);
        size_t sym = info >> 32;
        if (type != 257 && type != 1025 && type != 1026 && type != 1027)
            bad("Unsupported relocation type");
        if (sym >= symbols.size())
            bad("Invalid relocation symbol");
        target(at);
        relocs.push_back({at, type, sym, a});
    };
    for (auto &s : sections) {
        if (s.type == 4) {
            if (s.entry != 24 || s.size % 24)
                bad("Invalid RELA table");
            for (uint64_t p = s.off; p < s.off + s.size; p += 24)
                relocation(r.u(p, 8), r.u(p + 8, 8), int64_t(r.u(p + 16, 8)));
        } else if (s.type == 0x60000002) {
            uint64_t p = s.off, end = s.off + s.size;
            if (s.size < 4 || r.u(p, 4) != 0x32535041)
                bad("Missing APS2 header");
            p += 4;
            auto sleb = [&]() -> int64_t {
                uint64_t v = 0;
                unsigned shift = 0;
                for (unsigned i = 0; i < 10; ++i) {
                    if (p >= end)
                        bad("Truncated APS2 number");
                    auto c = r.u(p++, 1);
                    if (i == 9 && c != 0 && c != 127)
                        bad("APS2 number overflow");
                    v |= (c & 127) << shift;
                    shift += 7;
                    if (!(c & 128)) {
                        if (shift < 64 && (c & 64))
                            v |= ~uint64_t(0) << shift;
                        return int64_t(v);
                    }
                }
                bad("APS2 number too long");
            };
            int64_t count = sleb(), start = sleb();
            if (count < 0 || count > 100000 || start < 0)
                bad("Invalid APS2 count");
            uint64_t dest = start;
            int64_t a = 0, done = 0;
            while (done < count) {
                int64_t n = sleb(), flags = sleb();
                if (n <= 0 || n > count - done || flags < 0 || flags > 15 ||
                    (flags & 4 && !(flags & 8)))
                    bad("Invalid APS2 group");
                int64_t delta = flags & 2 ? sleb() : 0;
                uint64_t info = flags & 1 ? uint64_t(sleb()) : 0;
                if ((flags & 12) == 12)
                    a = signed_add(a, sleb());
                else if (!(flags & 8))
                    a = 0;
                for (int64_t i = 0; i < n; ++i) {
                    dest = plus_signed(dest, flags & 2 ? delta : sleb());
                    if (!(flags & 1))
                        info = uint64_t(sleb());
                    if ((flags & 12) == 8)
                        a = signed_add(a, sleb());
                    relocation(dest, info, a);
                    ++done;
                }
            }
            if (p != end)
                bad("Trailing APS2 bytes");
        } else if (s.type == 19 || s.type == 0x6fffff00) {
            if (s.entry != 8 || s.size % 8)
                bad("Invalid RELR table");
            uint64_t addr = 0;
            bool seen = false;
            for (uint64_t p = s.off; p < s.off + s.size; p += 8) {
                uint64_t v = r.u(p, 8);
                if (!(v & 1)) {
                    target(v);
                    relative.push_back(v);
                    addr = add(v, 8);
                    seen = true;
                } else {
                    if (!seen)
                        bad("RELR bitmap before base");
                    for (unsigned i = 0; i < 63; ++i)
                        if ((v >> (i + 1)) & 1) {
                            auto at = add(addr, 8 * i);
                            target(at);
                            relative.push_back(at);
                        }
                    addr = add(addr, 63 * 8);
                }
            }
        }
    }
}
#if !defined(DSEE_ELF_PARSER_ONLY)
Image::Image(const Elf &elf, const std::function<void *(const std::string &)> &resolve)
    : symbols(elf.symbols) {
    size_t page = size_t(sysconf(_SC_PAGESIZE));
    if (!page || (page & (page - 1)))
        bad("Invalid host page size");
    size_t tls = (elf.image_size + page - 1) & ~(page - 1);
    size = tls + page;
    base = static_cast<uint8_t *>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (base == MAP_FAILED) {
        base = nullptr;
        bad("Cannot allocate ELF image");
    }
    try {
        for (auto &s : elf.segments)
            std::memcpy(base + s.address, elf.bytes.data() + s.offset, s.filesz);
        auto read64 = [&](uint64_t p) {
            uint64_t v;
            std::memcpy(&v, base + p, 8);
            return v;
        };
        auto write64 = [&](uint64_t p, uint64_t v) { std::memcpy(base + p, &v, 8); };
        for (auto p : elf.relative) {
            auto v = read64(p);
            if (!loaded(elf.segments, v, 1))
                bad("Invalid RELR address");
            write64(p, add(uintptr_t(base), v));
        }
        for (auto &r : elf.relocs) {
            auto &s = elf.symbols[r.symbol];
            uint64_t v;
            if (r.type == 1027) {
                auto offset = plus_signed(0, r.addend);
                if (!loaded(elf.segments, offset, 1))
                    bad("Invalid relative addend");
                v = add(uintptr_t(base), offset);
            } else {
                v = s.defined ? uintptr_t(base + s.value) : uintptr_t(resolve(s.name));
                if (!v)
                    bad("Unresolved symbol");
                v = plus_signed(v, r.addend);
            }
            write64(r.address, v);
        }
        for (auto &s : elf.segments)
            if (s.flags & 1) {
                if (s.address % 4 || s.filesz % 4)
                    bad("Unaligned executable section");
                for (uint64_t p = s.address; p < s.address + s.filesz; p += 4) {
                    uint32_t op;
                    std::memcpy(&op, base + p, 4);
                    if ((op & ~31U) == 0xd53bd040U) {
                        int64_t delta = int64_t(tls) - int64_t(p);
                        if (delta < -(1 << 20) || delta >= (1 << 20))
                            bad("TLS outside ADR range");
                        uint32_t replacement = 0x10000000U | ((uint32_t(delta) & 3) << 29) |
                                               (((uint32_t(delta) >> 2) & 0x7ffff) << 5) |
                                               (op & 31);
                        std::memcpy(base + p, &replacement, 4);
                        ++tls_patches;
                    }
                }
            }
        uint64_t canary = 0;
        arc4random_buf(&canary, sizeof(canary));
        write64(tls + 40, canary);
        __builtin___clear_cache(reinterpret_cast<char *>(base),
                                reinterpret_cast<char *>(base + size));
        std::vector<int> perms(size / page, PROT_NONE);
        perms[tls / page] = PROT_READ | PROT_WRITE;
        for (auto &s : elf.segments)
            if (s.memsz) {
                int prot = (s.flags & 4 ? PROT_READ : 0) | (s.flags & 2 ? PROT_WRITE : 0) |
                           (s.flags & 1 ? PROT_EXEC : 0);
                for (size_t i = s.address / page; i <= (s.address + s.memsz - 1) / page; ++i)
                    perms[i] |= prot;
            }
        for (size_t i = 0; i < perms.size(); ++i) {
            if ((perms[i] & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
                bad("Page would be writable and executable");
            if (mprotect(base + i * page, page, perms[i]))
                bad("Cannot set image protection");
        }
    } catch (...) {
        munmap(base, size);
        base = nullptr;
        throw;
    }
}
Image::~Image() {
    if (base)
        munmap(base, size);
}
void *Image::symbol(const std::string &name) const {
    for (auto &s : symbols)
        if (s.defined && s.name == name)
            return base + s.value;
    throw std::runtime_error("Missing symbol: " + name);
}
#endif
} // namespace dsee
