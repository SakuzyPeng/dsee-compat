#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace dsee {
struct Segment {
    uint64_t offset, address, filesz, memsz;
    uint32_t flags;
};
struct Symbol {
    std::string name;
    uint64_t value, size;
    bool defined;
};
struct Relocation {
    uint64_t address;
    uint32_t type;
    size_t symbol;
    int64_t addend;
};
class Elf {
  public:
    explicit Elf(std::vector<uint8_t> bytes);
    std::vector<uint8_t> bytes;
    std::vector<Segment> segments;
    std::vector<Symbol> symbols;
    std::vector<Relocation> relocs;
    std::vector<uint64_t> relative;
    uint64_t image_size = 0;
};
class Image {
  public:
    Image(const Elf &, const std::function<void *(const std::string &)> &resolve);
    ~Image();
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;
    void *symbol(const std::string &) const;
    size_t tls_patches = 0;

  private:
    uint8_t *base = nullptr;
    size_t size = 0;
    std::vector<Symbol> symbols;
};
} // namespace dsee
