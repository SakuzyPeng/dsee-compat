#pragma once
#include "dsee_postprocess.hpp"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
namespace dsee {
class WavReader : public AudioSource {
  public:
    // Default retains the native runtime's strict 48 kHz PCM16 input contract.
    explicit WavReader(const std::filesystem::path &, bool native_input = true);
    size_t read(int16_t *interleaved, size_t max_frames);
    size_t read_at(uint64_t first, StereoFrame *, size_t count) override;
    unsigned sample_rate() const override {
        return rate;
    }
    uint64_t frame_count() const override {
        return frames;
    }
    uint64_t frames = 0;

  private:
    std::ifstream file;
    uint64_t position = 0, data_offset = 0;
    unsigned rate = 0, bits = 0, frame_bytes = 0;
    void seek(uint64_t first);
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
