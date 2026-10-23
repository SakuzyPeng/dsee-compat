#pragma once
#include "backend.hpp"

namespace dsee {
class EmulatedProfile final : public Backend {
  public:
    EmulatedProfile(Resources, unsigned rate, unsigned profile);
    ~EmulatedProfile() override;
    const int32_t *block(const int16_t *);
    void process_block(const int16_t *, int32_t *) override;
    void close() override;
    size_t output_samples() const;
    uint64_t calls() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
void emulation_self_test();
} // namespace dsee
