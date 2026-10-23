#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace dsee::emulation {
enum Permission : unsigned { Read = 1, Write = 2, Execute = 4, All = 7 };
using Vector = std::array<uint64_t, 2>;
using Import = std::function<void(size_t)>;
using Access = std::function<uint8_t *(uint64_t, size_t, unsigned)>;

// One implementation is linked per target; the resource loader, host bridges,
// allocator, fixtures and benchmark loop are shared between the two engines.
class Cpu {
  public:
    Cpu(Import import, Access access, uint64_t stubs, uint64_t stop, uint64_t tls);
    ~Cpu();
    Cpu(const Cpu &) = delete;
    Cpu &operator=(const Cpu &) = delete;
    void map(uint64_t base, size_t size, uint8_t *host, unsigned permissions);
    void protect(uint64_t base, size_t size, unsigned permissions);
    uint64_t reg(unsigned index) const;
    void reg(unsigned index, uint64_t value);
    Vector vector(unsigned index) const;
    void vector(unsigned index, Vector value);
    size_t stub_size() const;
    void import_stub(uint8_t *out, uint64_t address, size_t index);
    void stop_stub(uint8_t *out);
    uint64_t run(uint64_t entry, uint64_t sp, const std::array<uint64_t, 8> &args);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
const char *engine_name();
const char *engine_version();
const char *engine_configuration();
} // namespace dsee::emulation
