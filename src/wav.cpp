#include "wav.hpp"
#include <array>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
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
WavReader::WavReader(const std::filesystem::path &path) : file(path, std::ios::binary) {
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
            require(le(f, 2) == 1 && le(f + 2, 2) == 2 && le(f + 4, 4) == 48000 &&
                        le(f + 8, 4) == 192000 && le(f + 12, 2) == 4 && le(f + 14, 2) == 16,
                    "Input must be 48 kHz stereo PCM16 (format tag 1)");
            fmt = true;
        } else if (!std::memcmp(h, "data", 4)) {
            require(!data && n % 4 == 0, "Invalid or duplicate data chunk");
            data = true;
            offset = pos;
            bytes = n;
        }
        pos += n + (n & 1);
        file.seekg(std::streamoff(pos));
        require(bool(file), "Cannot seek WAV chunk");
    }
    require(fmt && data, "Missing WAV fmt/data chunk");
    frames = remaining = bytes / 4;
    file.seekg(std::streamoff(offset));
}
size_t WavReader::read(int16_t *out, size_t max) {
    size_t n = size_t(std::min(uint64_t(max), remaining));
    if (n) {
        require(n <= SIZE_MAX / 4, "WAV read size overflow");
        exact(file, reinterpret_cast<unsigned char *>(out), n * 4);
        remaining -= n;
    }
    return n;
}
WavWriter::WavWriter(const std::filesystem::path &path, unsigned sample_rate)
    : destination(path.string()), rate(sample_rate) {
    require(rate == 48000 || rate == 96000 || rate == 192000, "Unsupported output rate");
    require(!std::filesystem::exists(path) && !std::filesystem::is_symlink(path),
            "Output already exists");
    std::string pattern = destination + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    require(fd >= 0, "Cannot create temporary output beside destination");
    temporary = name.data();
    file = fdopen(fd, "w+b");
    if (!file) {
        close(fd);
        unlink(temporary.c_str());
        throw std::runtime_error("Cannot open temporary output");
    }
    try {
        header();
    } catch (...) {
        std::fclose(file);
        file = nullptr;
        unlink(temporary.c_str());
        throw;
    }
}
WavWriter::~WavWriter() {
    if (file)
        std::fclose(file);
    if (!temporary.empty())
        unlink(temporary.c_str());
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
    require(std::fflush(file) == 0, "Cannot flush output");
    require(fsync(fileno(file)) == 0, "Cannot sync output");
    int result = std::fclose(file);
    file = nullptr;
    require(result == 0, "Cannot close output");
    // Same-directory hard link publishes atomically without replacing a raced-in file.
    require(link(temporary.c_str(), destination.c_str()) == 0,
            "Cannot publish output (destination may already exist)");
    unlink(temporary.c_str());
    temporary.clear();
}
} // namespace dsee
