#include "wav.hpp"
#include "platform.hpp"
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace dsee {
namespace {
uint32_t le(const unsigned char *p, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= uint32_t(p[i]) << (8 * i);
    return v;
}
void put(unsigned char *p, uint32_t v, size_t n) {
    for (size_t i = 0; i < n; ++i)
        p[i] = uint8_t(v >> (8 * i));
}
void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
void exact(std::ifstream &f, unsigned char *p, size_t n) {
    require(bool(f.read(reinterpret_cast<char *>(p), n)), "Truncated WAV");
}
} // namespace
WavReader::WavReader(const std::filesystem::path &path, bool native_input)
    : file(path, std::ios::binary) {
    require(bool(file), "Cannot open input WAV");
    file.seekg(0, std::ios::end);
    auto length = file.tellg();
    require(length >= 12, "WAV too short");
    file.seekg(0);
    unsigned char head[12];
    exact(file, head, 12);
    require(!std::memcmp(head, "RIFF", 4) && !std::memcmp(head + 8, "WAVE", 4),
            "Only RIFF/WAVE is supported (not RF64)");
    uint64_t end = uint64_t(le(head + 4, 4)) + 8;
    require(end == uint64_t(length), "RIFF length does not match file");
    bool fmt = false, data = false;
    uint64_t offset = 0, bytes = 0, pos = 12;
    while (pos < end) {
        require(end - pos >= 8, "Truncated chunk header");
        unsigned char h[8];
        exact(file, h, 8);
        pos += 8;
        uint64_t n = le(h + 4, 4);
        require(n <= end - pos && (n + (n & 1)) <= end - pos, "Chunk exceeds RIFF bounds");
        if (!std::memcmp(h, "fmt ", 4)) {
            require(!fmt && n >= 16, "Invalid or duplicate format chunk");
            unsigned char f[16];
            exact(file, f, 16);
            rate = le(f + 4, 4);
            bits = le(f + 14, 2);
            frame_bytes = bits / 4;
            bool supported_rate = rate == 44100 || rate == 48000 || rate == 88200 ||
                                  rate == 96000 || rate == 176400 || rate == 192000;
            require(le(f, 2) == 1 && le(f + 2, 2) == 2 && (bits == 16 || bits == 32) &&
                        supported_rate && le(f + 8, 4) == rate * frame_bytes &&
                        le(f + 12, 2) == frame_bytes,
                    "WAV must be stereo PCM16/PCM32 (format tag 1) at a supported rate");
            require(!native_input || (rate == 48000 && bits == 16),
                    "Input must be 48 kHz stereo PCM16 (format tag 1)");
            fmt = true;
        } else if (!std::memcmp(h, "data", 4)) {
            require(!data, "Duplicate data chunk");
            data = true;
            offset = pos;
            bytes = n;
        }
        pos += n + (n & 1);
        file.seekg(std::streamoff(pos));
        require(bool(file), "Cannot seek WAV chunk");
    }
    require(fmt && data, "Missing WAV fmt/data chunk");
    require(bytes % frame_bytes == 0, "Partial PCM frame in WAV data");
    frames = bytes / frame_bytes;
    data_offset = offset;
    seek(0);
}
size_t WavReader::read(int16_t *out, size_t max) {
    require(bits == 16, "PCM16 read requested from another format");
    size_t n = size_t(std::min(uint64_t(max), frames - position));
    if (n) {
        require(n <= SIZE_MAX / 4, "WAV read size overflow");
        exact(file, reinterpret_cast<unsigned char *>(out), n * 4);
        position += n;
    }
    return n;
}
void WavReader::seek(uint64_t first) {
    require(first <= frames, "WAV seek outside audio");
    file.clear();
    file.seekg(std::streamoff(data_offset + first * frame_bytes));
    require(bool(file), "Cannot seek WAV audio");
    position = first;
}
size_t WavReader::read_at(uint64_t first, StereoFrame *out, size_t count) {
    if (first >= frames || !count)
        return 0;
    size_t n = size_t(std::min(uint64_t(count), frames - first));
    require(n <= SIZE_MAX / frame_bytes, "WAV read size overflow");
    seek(first);
    std::vector<unsigned char> bytes(n * frame_bytes);
    exact(file, bytes.data(), bytes.size());
    for (size_t i = 0; i < n; ++i) {
        if (bits == 16) {
            int16_t values[2];
            std::memcpy(values, bytes.data() + 4 * i, 4);
            out[i] = {values[0] / 32768.0, values[1] / 32768.0};
        } else {
            int32_t values[2];
            std::memcpy(values, bytes.data() + 8 * i, 8);
            out[i] = {values[0] / 2147483648.0, values[1] / 2147483648.0};
        }
    }
    position += n;
    return n;
}
WavWriter::WavWriter(const std::filesystem::path &path, unsigned sample_rate)
    : destination(path), rate(sample_rate) {
    require(rate == 48000 || rate == 96000 || rate == 192000, "Unsupported output rate");
    require(!std::filesystem::exists(path) && !std::filesystem::is_symlink(path),
            "Output already exists");
    file = temporary_file(destination, temporary);
    try {
        header();
    } catch (...) {
        std::fclose(file);
        file = nullptr;
        remove_file(temporary);
        throw;
    }
}
WavWriter::~WavWriter() {
    if (file)
        std::fclose(file);
    if (!temporary.empty())
        remove_file(temporary);
}
void WavWriter::header() {
    unsigned char h[44]{};
    std::memcpy(h, "RIFF", 4);
    put(h + 4, uint32_t(36 + frames * 8), 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put(h + 16, 16, 4);
    put(h + 20, 1, 2);
    put(h + 22, 2, 2);
    put(h + 24, rate, 4);
    put(h + 28, rate * 8, 4);
    put(h + 32, 8, 2);
    put(h + 34, 32, 2);
    std::memcpy(h + 36, "data", 4);
    put(h + 40, uint32_t(frames * 8), 4);
    require(std::fseek(file, 0, SEEK_SET) == 0 && std::fwrite(h, 1, 44, file) == 44,
            "Cannot write WAV header");
}
void WavWriter::write(const int32_t *data, size_t n) {
    require(n <= max_frames - frames, "Output exceeds RIFF size limit");
    require(std::fwrite(data, 8, n, file) == n, "Cannot write output PCM");
    frames += n;
}
void WavWriter::commit() {
    header();
    sync_file(file);
    int result = std::fclose(file);
    file = nullptr;
    require(result == 0, "Cannot close output");
    publish_file(temporary, destination);
    temporary.clear();
}
} // namespace dsee
