#include "emulation_cpu.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unicorn/unicorn.h>
#include <utility>

namespace dsee::emulation {
namespace {
void checked(uc_err error) {
    if (error != UC_ERR_OK)
        throw std::runtime_error(std::string("Unicorn: ") + uc_strerror(error));
}
int gpr(unsigned index) {
    if (index < 29)
        return UC_ARM64_REG_X0 + int(index);
    if (index == 29)
        return UC_ARM64_REG_X29;
    if (index == 30)
        return UC_ARM64_REG_X30;
    throw std::runtime_error("Invalid guest register");
}
} // namespace
struct Cpu::Impl {
    uc_engine *uc = nullptr;
    Import import;
    uint64_t stubs, stop;
    char fault[512]{};

    Impl(Import callback, uint64_t stub_address, uint64_t stop_address, uint64_t tls)
        : import(std::move(callback)), stubs(stub_address), stop(stop_address) {
        checked(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc));
        try {
            checked(uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_MAX));
            checked(uc_reg_write(uc, UC_ARM64_REG_TPIDR_EL0, &tls));
            uint64_t fp_enable = 3ULL << 20;
            checked(uc_reg_write(uc, UC_ARM64_REG_CPACR_EL1, &fp_enable));
            uc_hook h;
            checked(uc_hook_add(uc, &h, UC_HOOK_CODE, reinterpret_cast<void *>(hook), this, stubs,
                                stop - 1));
        } catch (...) {
            uc_close(uc);
            uc = nullptr;
            throw;
        }
    }
    ~Impl() {
        if (uc)
            uc_close(uc);
    }
    static void hook(uc_engine *uc, uint64_t pc, uint32_t, void *data) noexcept {
        auto &self = *static_cast<Impl *>(data);
        try {
            if (pc < self.stubs || (pc - self.stubs) % 4)
                throw std::runtime_error("Invalid guest import entry");
            self.import(size_t((pc - self.stubs) / 4));
        } catch (const std::exception &e) {
            std::snprintf(self.fault, sizeof(self.fault), "%s", e.what());
            uc_emu_stop(uc);
        } catch (...) {
            std::snprintf(self.fault, sizeof(self.fault), "%s", "Unknown import callback failure");
            uc_emu_stop(uc);
        }
    }
};
Cpu::Cpu(Import import, Access, uint64_t stubs, uint64_t stop, uint64_t tls)
    : impl(std::make_unique<Impl>(std::move(import), stubs, stop, tls)) {}
Cpu::~Cpu() = default;
void Cpu::map(uint64_t base, size_t size, uint8_t *host, unsigned permissions) {
    checked(uc_mem_map_ptr(impl->uc, base, size, permissions, host));
}
void Cpu::protect(uint64_t base, size_t size, unsigned permissions) {
    checked(uc_mem_protect(impl->uc, base, size, permissions));
}
uint64_t Cpu::reg(unsigned index) const {
    uint64_t result = 0;
    checked(uc_reg_read(impl->uc, gpr(index), &result));
    return result;
}
void Cpu::reg(unsigned index, uint64_t value) {
    checked(uc_reg_write(impl->uc, gpr(index), &value));
}
Vector Cpu::vector(unsigned index) const {
    if (index >= 32)
        throw std::runtime_error("Invalid guest vector register");
    Vector result{};
    checked(uc_reg_read(impl->uc, UC_ARM64_REG_Q0 + int(index), result.data()));
    return result;
}
void Cpu::vector(unsigned index, Vector value) {
    if (index >= 32)
        throw std::runtime_error("Invalid guest vector register");
    checked(uc_reg_write(impl->uc, UC_ARM64_REG_Q0 + int(index), value.data()));
}
size_t Cpu::stub_size() const {
    return 4;
}
void Cpu::import_stub(uint8_t *out, uint64_t, size_t) {
    uint32_t ret = 0xd65f03c0;
    std::memcpy(out, &ret, sizeof(ret));
}
void Cpu::stop_stub(uint8_t *out) {
    import_stub(out, 0, 0);
}
uint64_t Cpu::run(uint64_t entry, uint64_t sp, const std::array<uint64_t, 8> &args) {
    for (unsigned i = 0; i < args.size(); ++i)
        reg(i, args[i]);
    reg(30, impl->stop);
    checked(uc_reg_write(impl->uc, UC_ARM64_REG_SP, &sp));
    impl->fault[0] = 0;
    auto error = uc_emu_start(impl->uc, entry, impl->stop, 0, 0);
    if (impl->fault[0])
        throw std::runtime_error(impl->fault);
    checked(error);
    uint64_t pc = 0;
    checked(uc_reg_read(impl->uc, UC_ARM64_REG_PC, &pc));
    if (pc != impl->stop)
        throw std::runtime_error("Guest did not reach the stop address");
    return reg(0);
}
const char *engine_name() {
    return "Unicorn";
}
const char *engine_version() {
    static const std::string version = std::to_string(UC_VERSION_MAJOR) + "." +
                                       std::to_string(UC_VERSION_MINOR) + "." +
                                       std::to_string(UC_VERSION_PATCH);
    return version.c_str();
}
const char *engine_configuration() {
    return "ARM64 max; native C++ imports; bounded code hooks";
}
} // namespace dsee::emulation
