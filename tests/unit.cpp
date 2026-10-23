#include "platform.hpp"
#ifdef _WIN32
#include <windows.h>

#include <io.h>
#include <winioctl.h>
#endif
#include "elf.hpp"
#include "wav.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using Bytes = std::vector<uint8_t>;
void check(bool v) {
    if (!v)
        throw std::runtime_error("unit check failed");
}
void put(Bytes &b, size_t p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i)
        b.at(p + i) = uint8_t(v >> (8 * i));
}
Bytes elf() {
    Bytes b(512);
    put(b, 0, 0x464c457f, 4);
    b[4] = 2;
    b[5] = 1;
    b[6] = 1;
    put(b, 16, 3, 2);
    put(b, 18, 183, 2);
    put(b, 20, 1, 4);
    put(b, 32, 64, 8);
    put(b, 40, 256, 8);
    put(b, 52, 64, 2);
    put(b, 54, 56, 2);
    put(b, 56, 1, 2);
    put(b, 58, 64, 2);
    put(b, 60, 3, 2);
    put(b, 64, 1, 4);
    put(b, 68, 4, 4);
    put(b, 96, 512, 8);
    put(b, 104, 512, 8);
    put(b, 112, 4096, 8);
    put(b, 324, 11, 4);
    put(b, 344, 128, 8);
    put(b, 352, 24, 8);
    put(b, 360, 2, 4);
    put(b, 376, 24, 8);
    put(b, 388, 3, 4);
    put(b, 408, 152, 8);
    put(b, 416, 1, 8);
    return b;
}
void section(Bytes &b, uint32_t type, size_t size, size_t entry) {
    put(b, 60, 4, 2);
    put(b, 452, type, 4);
    put(b, 472, 160, 8);
    put(b, 480, size, 8);
    put(b, 504, entry, 8);
}
void sleb(Bytes &b, int64_t v) {
    bool more = true;
    while (more) {
        uint8_t c = v & 127;
        v >>= 7;
        more = !((v == 0 && !(c & 64)) || (v == -1 && (c & 64)));
        b.push_back(c | (more ? 128 : 0));
    }
}
template <class F> void rejects(F fn) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw);
}
void save(const std::filesystem::path &p, const Bytes &b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char *>(b.data()), b.size());
    check(bool(f));
}
int main() {
    try {
        auto b = elf();
        dsee::Elf parsed(b);
        check(parsed.image_size == 512 && parsed.symbols.size() == 1);
        for (size_t n : {size_t(0), size_t(63), size_t(300)}) {
            auto c = b;
            c.resize(n);
            rejects([&] { dsee::Elf x(c); });
        }
        for (auto patch : std::vector<std::pair<size_t, uint64_t>>{
                 {32, UINT64_MAX}, {40, UINT64_MAX}, {96, UINT64_MAX}, {104, 17 * 1024 * 1024}}) {
            auto c = b;
            put(c, patch.first, patch.second, 8);
            rejects([&] { dsee::Elf x(c); });
        }
        {
            auto c = b;
            put(c, 68, 7, 4);
            rejects([&] { dsee::Elf x(c); });
        }
        {
            auto c = b;
            section(c, 4, 24, 24);
            put(c, 160, 192, 8);
            put(c, 168, 1027, 8);
            put(c, 176, 16, 8);
            dsee::Elf x(c);
            check(x.relocs.size() == 1 && x.relocs[0].addend == 16);
#ifdef DSEE_TEST_NATIVE_IMAGE
            dsee::Image image(
                x, [](auto &) -> void * { throw std::runtime_error("unexpected import"); });
#endif
            put(c, 160, 513, 8);
            rejects([&] { dsee::Elf y(c); });
            put(c, 160, 192, 8);
            put(c, 168, 9999, 8);
            rejects([&] { dsee::Elf y(c); });
        }
        {
            auto c = b;
            Bytes packed = {'A', 'P', 'S', '2'};
            for (int64_t v : {2, 184, 2, 15, 8, 1027, 16})
                sleb(packed, v);
            section(c, 0x60000002, packed.size(), 1);
            std::copy(packed.begin(), packed.end(), c.begin() + 160);
            dsee::Elf x(c);
            check(x.relocs.size() == 2 && x.relocs[0].address == 192 &&
                  x.relocs[1].address == 200 && x.relocs[1].addend == 16);
            put(c, 480, packed.size() - 1, 8);
            rejects([&] { dsee::Elf y(c); });
        }
        {
            // Packed addends must reject signed overflow on every host compiler.
            for (auto values : {std::pair<int64_t, int64_t>{INT64_MAX, 1},
                                std::pair<int64_t, int64_t>{INT64_MIN, -1}}) {
                auto c = b;
                Bytes packed = {'A', 'P', 'S', '2'};
                for (auto v : std::vector<int64_t>{2, 184, 1, 15, 8, 1027, values.first, 1, 15, 8,
                                                   1027, values.second})
                    sleb(packed, v);
                section(c, 0x60000002, packed.size(), 1);
                std::copy(packed.begin(), packed.end(), c.begin() + 160);
                rejects([&] { dsee::Elf x(c); });
                packed.clear();
                packed.insert(packed.end(), {'A', 'P', 'S', '2'});
                for (auto v : std::vector<int64_t>{2, 184, 1, 15, 8, 1027, values.first, 1, 15, 8,
                                                   1027, -values.second})
                    sleb(packed, v);
                section(c, 0x60000002, packed.size(), 1);
                std::copy(packed.begin(), packed.end(), c.begin() + 160);
                dsee::Elf parsed(c);
                check(parsed.relocs.size() == 2 &&
                      parsed.relocs[1].addend == values.first - values.second);
            }
        }
        {
            auto c = b;
            section(c, 0x6fffff00, 16, 8);
            put(c, 160, 192, 8);
            put(c, 168, 3, 8);
            put(c, 192, 16, 8);
            dsee::Elf x(c);
            check(x.relative == std::vector<uint64_t>({192, 200}));
            put(c, 160, 3, 8);
            rejects([&] { dsee::Elf y(c); });
        }
        // Check the same 64-bit seek used by the offline PCM spool without
        // allocating gigabytes on disk. Windows requires explicit sparse mode.
        {
            std::unique_ptr<FILE, decltype(&std::fclose)> spool(dsee::temporary_spool(),
                                                                std::fclose);
#ifdef _WIN32
            DWORD returned = 0;
            check(DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(spool.get()))),
                                  FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                                  nullptr) != 0);
#endif
            constexpr int64_t offset = INT64_C(2147483664);
            check(dsee::seek_file(spool.get(), offset, SEEK_SET) == 0);
            uint32_t value = 0x12345678, readback = 0;
            check(std::fwrite(&value, sizeof(value), 1, spool.get()) == 1);
            check(dsee::seek_file(spool.get(), offset, SEEK_SET) == 0);
            check(std::fread(&readback, sizeof(readback), 1, spool.get()) == 1 &&
                  readback == value);
        }
        auto root = dsee::temporary_directory("dsee-unit-");
        try {
            Bytes w(48);
            std::copy_n("RIFF", 4, w.begin());
            put(w, 4, 40, 4);
            std::copy_n("WAVEfmt ", 8, w.begin() + 8);
            put(w, 16, 16, 4);
            put(w, 20, 1, 2);
            put(w, 22, 2, 2);
            put(w, 24, 48000, 4);
            put(w, 28, 192000, 4);
            put(w, 32, 4, 2);
            put(w, 34, 16, 2);
            std::copy_n("data", 4, w.begin() + 36);
            put(w, 40, 4, 4);
            put(w, 44, 123, 2);
            put(w, 46, 456, 2);
            save(root / "in.wav", w);
            {
                dsee::WavReader input(root / "in.wav");
                int16_t samples[2];
                check(input.frames == 1 && input.read(samples, 1) == 1 && samples[0] == 123 &&
                      samples[1] == 456 && input.read(samples, 1) == 0);
            }
            {
                auto c = w;
                put(c, 24, 44100, 4);
                save(root / "bad.wav", c);
                rejects([&] { dsee::WavReader x(root / "bad.wav"); });
            }
            {
                auto c = w;
                put(c, 40, UINT32_MAX, 4);
                save(root / "bad.wav", c);
                rejects([&] { dsee::WavReader x(root / "bad.wav"); });
            }
            {
                auto c = w;
                c.insert(c.begin() + 12, {'J', 'U', 'N', 'K', 1, 0, 0, 0, 42, 0});
                put(c, 4, c.size() - 8, 4);
                save(root / "junk.wav", c);
                dsee::WavReader x(root / "junk.wav");
                check(x.frames == 1);
            }
            {
                dsee::WavWriter out(root / "out.wav", 96000);
                int32_t pcm[2] = {123, 456};
                out.write(pcm, 1);
                out.commit();
                check(std::filesystem::file_size(root / "out.wav") == 52);
            }
            rejects([&] { dsee::WavWriter out(root / "out.wav", 96000); });
            {
                dsee::WavWriter out(root / "cancel.wav", 48000);
            }
            check(!std::filesystem::exists(root / "cancel.wav"));
            {
                dsee::WavWriter out(root / "race.wav", 48000);
                save(root / "race.wav", Bytes{1, 2, 3});
                rejects([&] { out.commit(); });
                check(std::filesystem::file_size(root / "race.wav") == 3);
            }
            {
                dsee::WavWriter out(root / "large.wav", 48000);
                int32_t dummy = 0;
                rejects([&] { out.write(&dummy, size_t(dsee::WavWriter::max_frames) + 1); });
            }
            std::filesystem::remove_all(root);
        } catch (...) {
            std::filesystem::remove_all(root);
            throw;
        }
        std::cout << "ELF bounds, RELA/APS2/RELR and WAV boundary tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
