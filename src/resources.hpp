#pragma once
#include "assets.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dsee {
using Bytes = std::vector<uint8_t>;
struct Resources {
    // Library, DSX, PCM, AAC, MP3. Unselected parameters may be empty.
    std::array<Bytes, 5> bytes;
};
std::string sha256(const void *, size_t);
void validate_asset(const Bytes &, const AssetSpec &);
Bytes read_asset(const char *root, const AssetSpec &);
Resources load_resources(const char *root, int profile = -1);
} // namespace dsee
