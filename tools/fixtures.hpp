#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
namespace dsee::test {
inline std::vector<int16_t> fixture(const std::string &kind) {
    std::vector<int16_t> data(2 * 49152);
    uint32_t rng = 12345;
    constexpr double pi = 3.14159265358979323846;
    for (size_t i = 0; i < 49152; ++i) {
        double x = 0;
        if (kind == "tone")
            x = 0.2 * std::sin(2 * pi * 1000 * double(i) / 48000.0);
        else if (kind == "impulse")
            x = i == 0 ? 0.5 : 0;
        else if (kind == "noise") {
            rng = rng * 1664525 + 1013904223;
            x = ((rng >> 8) / 16777216.0 - 0.5) * 0.3;
        }
        data[2 * i] = int16_t(x * 32768);
        data[2 * i + 1] = int16_t(x * 0.7 * 32768);
    }
    return data;
}
} // namespace dsee::test
