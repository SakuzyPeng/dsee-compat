#pragma once
#include "resources.hpp"
#include <memory>
#include <stdexcept>

namespace dsee {
class ProcessError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class Backend {
  public:
    virtual ~Backend() = default;
    virtual void process_block(const int16_t *input, int32_t *output) = 0;
    // Explicit close may fail. The destructor only releases host resources.
    virtual void close() = 0;
};
std::unique_ptr<Backend> make_backend(Resources, unsigned rate, unsigned profile);
} // namespace dsee
