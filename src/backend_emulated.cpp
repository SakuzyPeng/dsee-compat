#include "emulation.hpp"
namespace dsee {
std::unique_ptr<Backend> make_backend(Resources resources, unsigned rate, unsigned profile) {
    return std::make_unique<EmulatedProfile>(std::move(resources), rate, profile);
}
} // namespace dsee
