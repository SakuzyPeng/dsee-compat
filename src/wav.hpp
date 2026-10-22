#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
namespace dsee {
class WavReader {
  public:
    explicit WavReader(const std::filesystem::path &);
    size_t read(int16_t *interleaved, size_t max_frames);
    uint64_t frames = 0;

  private:
    std::ifstream file;
    uint64_t remaining = 0;
};
class WavWriter {
  public:
    WavWriter(const std::filesystem::path &, unsigned rate);
    ~WavWriter();
    WavWriter(const WavWriter &) = delete;
    WavWriter &operator=(const WavWriter &) = delete;
    void write(const int32_t *, size_t frames);
    void commit();
    uint64_t frames = 0;
    static constexpr uint64_t max_frames = (UINT32_MAX - 36ULL) / 8;

  private:
    FILE *file = nullptr;
    std::string destination, temporary;
    unsigned rate;
    void header();
};
} // namespace dsee
