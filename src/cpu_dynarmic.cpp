#include "emulation_cpu.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dynarmic/interface/A64/a64.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef DSEE_DYNARMIC_VERSION
#define DSEE_DYNARMIC_VERSION "unknown"
#endif
namespace dsee::emulation {
namespace {
constexpr size_t page_size = 4096, address_bits = 31;
constexpr uint32_t stop_svc = 0xffff;
constexpr auto abort_reason = Dynarmic::HaltReason::MemoryAbort;
} // namespace
struct Cpu::Impl final : Dynarmic::A64::UserCallbacks {
    Import import;
    Access access;
    uint64_t stubs, stop, tls;
    std::vector<void *> pages;
    std::vector<std::pair<uint64_t, uint8_t *>> mappings;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    char fault[512]{};
    bool returned = false;
    size_t import_count = 0;

    Impl(Import callback, Access memory, uint64_t stub_address, uint64_t stop_address,
         uint64_t tls_address)
        : import(std::move(callback)), access(std::move(memory)), stubs(stub_address),
          stop(stop_address), tls(tls_address), pages(size_t(1) << (address_bits - 12), nullptr) {
        Dynarmic::A64::UserConfig config{};
        config.callbacks = this;
        config.tpidr_el0 = &tls;
        config.page_table = pages.data();
        config.page_table_address_space_bits = address_bits;
        config.silently_mirror_page_table = false;
        config.detect_misaligned_access_via_page_table = 8 | 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;
        config.check_halt_on_memory_access = true;
        config.enable_cycle_counting = false;
        config.code_cache_size = 32 * 1024 * 1024;
        // Keep all_safe_optimizations and unsafe_optimizations=false. No fastmem
        // signal-handler arena or fallback emulator is used by this probe.
        jit = std::make_unique<Dynarmic::A64::Jit>(config);
    }
    void fail(const char *why) noexcept {
        if (!fault[0])
            std::snprintf(fault, sizeof(fault), "%s", why);
        if (jit)
            jit->HaltExecution(abort_reason);
    }
    template <class T> T read(uint64_t address) noexcept {
        T value{};
        try {
            std::memcpy(&value, access(address, sizeof(T), Read), sizeof(T));
        } catch (const std::exception &e) {
            fail(e.what());
        } catch (...) {
            fail("Unknown guest memory read failure");
        }
        return value;
    }
    template <class T> void write(uint64_t address, T value) noexcept {
        try {
            std::memcpy(access(address, sizeof(T), Write), &value, sizeof(T));
        } catch (const std::exception &e) {
            fail(e.what());
        } catch (...) {
            fail("Unknown guest memory write failure");
        }
    }
    std::optional<uint32_t> MemoryReadCode(uint64_t address) override {
        try {
            uint32_t value;
            std::memcpy(&value, access(address, 4, Read | Execute), 4);
            return value;
        } catch (...) {
            return std::nullopt;
        }
    }
    uint8_t MemoryRead8(uint64_t p) override {
        return read<uint8_t>(p);
    }
    uint16_t MemoryRead16(uint64_t p) override {
        return read<uint16_t>(p);
    }
    uint32_t MemoryRead32(uint64_t p) override {
        return read<uint32_t>(p);
    }
    uint64_t MemoryRead64(uint64_t p) override {
        return read<uint64_t>(p);
    }
    Vector MemoryRead128(uint64_t p) override {
        return read<Vector>(p);
    }
    void MemoryWrite8(uint64_t p, uint8_t v) override {
        write(p, v);
    }
    void MemoryWrite16(uint64_t p, uint16_t v) override {
        write(p, v);
    }
    void MemoryWrite32(uint64_t p, uint32_t v) override {
        write(p, v);
    }
    void MemoryWrite64(uint64_t p, uint64_t v) override {
        write(p, v);
    }
    void MemoryWrite128(uint64_t p, Vector v) override {
        write(p, v);
    }
    void InterpreterFallback(uint64_t, size_t) override {
        fail("Dynarmic requested an unsupported instruction fallback");
    }
    void ExceptionRaised(uint64_t, Dynarmic::A64::Exception) override {
        fail("Dynarmic guest exception");
    }
    void CallSVC(uint32_t number) override {
        try {
            if (number == stop_svc) {
                if (jit->GetPC() != stop + 4)
                    throw std::runtime_error("Invalid guest stop entry");
                returned = true;
                jit->HaltExecution();
                return;
            }
            if (!number || number > import_count ||
                jit->GetPC() != stubs + uint64_t(number - 1) * 8 + 4)
                throw std::runtime_error("Invalid guest import entry");
            import(number - 1);
        } catch (const std::exception &e) {
            fail(e.what());
        } catch (...) {
            fail("Unknown import callback failure");
        }
    }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override {
        return UINT64_MAX;
    }
    uint64_t GetCNTPCT() override {
        return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    }
};
Cpu::Cpu(Import import, Access access, uint64_t stubs, uint64_t stop, uint64_t tls)
    : impl(std::make_unique<Impl>(std::move(import), std::move(access), stubs, stop, tls)) {}
Cpu::~Cpu() = default;
void Cpu::map(uint64_t base, size_t size, uint8_t *host, unsigned permissions) {
    if (base % page_size || size % page_size || base >= (uint64_t(1) << address_bits) ||
        size > (uint64_t(1) << address_bits) - base)
        throw std::runtime_error("Invalid Dynarmic map range");
    for (size_t p = 0; p < size; p += page_size) {
        auto index = size_t(base + p) / page_size;
        impl->mappings.emplace_back(base + p, host + p);
        impl->pages[index] = (permissions & (Read | Write)) == (Read | Write) ? host + p : nullptr;
    }
}
void Cpu::protect(uint64_t base, size_t size, unsigned permissions) {
    if (base % page_size || size % page_size || base >= (uint64_t(1) << address_bits) ||
        size > (uint64_t(1) << address_bits) - base)
        throw std::runtime_error("Invalid Dynarmic protection range");
    for (auto [address, pointer] : impl->mappings)
        if (address >= base && address - base < size)
            impl->pages[size_t(address) / page_size] =
                (permissions & (Read | Write)) == (Read | Write) ? pointer : nullptr;
    impl->jit->InvalidateCacheRange(base, size);
}
uint64_t Cpu::reg(unsigned index) const {
    return impl->jit->GetRegister(index);
}
void Cpu::reg(unsigned index, uint64_t value) {
    impl->jit->SetRegister(index, value);
}
Vector Cpu::vector(unsigned index) const {
    return impl->jit->GetVector(index);
}
void Cpu::vector(unsigned index, Vector value) {
    impl->jit->SetVector(index, value);
}
size_t Cpu::stub_size() const {
    return 8;
}
void Cpu::import_stub(uint8_t *out, uint64_t address, size_t index) {
    if (index >= stop_svc - 1 || address != impl->stubs + index * 8)
        throw std::runtime_error("Invalid Dynarmic import stub");
    uint32_t code[] = {0xd4000001U | (uint32_t(index + 1) << 5), 0xd65f03c0U};
    std::memcpy(out, code, sizeof(code));
    impl->import_count = index + 1;
}
void Cpu::stop_stub(uint8_t *out) {
    uint32_t svc = 0xd4000001U | (stop_svc << 5);
    std::memcpy(out, &svc, sizeof(svc));
}
uint64_t Cpu::run(uint64_t entry, uint64_t sp, const std::array<uint64_t, 8> &args) {
    impl->fault[0] = 0;
    impl->returned = false;
    // The public API serializes entry; no other thread can raise a halt here.
    impl->jit->ClearHalt(static_cast<Dynarmic::HaltReason>(UINT32_MAX));
    for (unsigned i = 0; i < args.size(); ++i)
        reg(i, args[i]);
    reg(30, impl->stop);
    impl->jit->SetSP(sp);
    impl->jit->SetPC(entry);
    impl->jit->Run();
    if (impl->fault[0])
        throw std::runtime_error(impl->fault);
    if (!impl->returned || impl->jit->GetPC() != impl->stop + 4)
        throw std::runtime_error("Dynarmic guest did not reach the stop address");
    return reg(0);
}
const char *engine_name() {
    return "Dynarmic";
}
const char *engine_version() {
    return DSEE_DYNARMIC_VERSION;
}
const char *engine_configuration() {
    return "safe optimizations; checked 31-bit page table; no fastmem or fallback";
}
} // namespace dsee::emulation
