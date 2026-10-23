// Local-only diagnostic transport and synthetic fixtures. The runtime core is shared.
#include "emulation.hpp"
#include "emulation_cpu.hpp"
#include "fixtures.hpp"
#include "platform.hpp"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
namespace {
using dsee::Bytes;
using dsee::Resources;
using dsee::sha256;
using Profile = dsee::EmulatedProfile;
void require(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}
Resources probe_resources(const std::string &root) {
    Resources resources;
    auto &bytes = resources.bytes;
    const dsee::AssetSpec *specs[] = {&dsee::library, &dsee::dsx, &dsee::parameters[0],
                                      &dsee::parameters[1], &dsee::parameters[2]};
    bool stream = root == "--stdin-assets";
    if (stream) {
#if defined(_WIN32)
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        char magic[8]{};
        require(bool(std::cin.read(magic, 8)) && !std::memcmp(magic, "DSEEAS01", 8),
                "Invalid resource stream header");
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto &s = *specs[i];
        auto &b = bytes[i];
        b.resize(s.size);
        if (stream) {
            unsigned char n[4]{};
            require(bool(std::cin.read(reinterpret_cast<char *>(n), 4)), "Truncated size");
            uint32_t size = uint32_t(n[0]) | (uint32_t(n[1]) << 8) | (uint32_t(n[2]) << 16) |
                            (uint32_t(n[3]) << 24);
            require(size == s.size, "Unsupported resource size");
            require(bool(std::cin.read(reinterpret_cast<char *>(b.data()), b.size())),
                    "Truncated resource");
        } else {
            b = dsee::read_asset(root.c_str(), s);
        }
        dsee::validate_asset(b, s);
    }
    if (stream)
        require(std::cin.peek() == EOF, "Trailing resource stream bytes");
    // All five files are verified before even creating an emulator instance.
    return resources;
}
unsigned number(const char *text, unsigned limit) {
    size_t end = 0;
    auto n = std::stoul(text, &end);
    require(end == std::strlen(text) && n > 0 && n <= limit, "Invalid numeric option");
    return unsigned(n);
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            dsee::emulation_self_test();
            std::cout << "Emulation self-test passed\n";
            return 0;
        }
        require(argc >= 3 && argc <= 5,
                "Usage: dsee-<engine>-probe RESOURCE_ROOT|--stdin-assets "
                "verify [new-PCM-directory] | benchmark [blocks] [repeats]");
        std::string mode = argv[2];
        require(mode == "verify" || mode == "benchmark", "Unknown probe mode");
        unsigned blocks = mode == "benchmark" && argc >= 4 ? number(argv[3], 1000000) : 64;
        unsigned repeats = mode == "benchmark" && argc == 5 ? number(argv[4], 100) : 3;
        Resources resources = probe_resources(argv[1]);
        const char *profiles[] = {"pcm", "aac128", "mp3128"};
        std::cout << "{\"event\":\"environment\",\"engine\":\"" << dsee::emulation::engine_name()
                  << "\",\"version\":\"" << dsee::emulation::engine_version()
                  << "\",\"configuration\":\"" << dsee::emulation::engine_configuration()
                  << "\",\"pointer_bits\":" << sizeof(void *) * 8 << "}" << std::endl;
        if (mode == "verify") {
            require(argc <= 4, "verify accepts only an optional new PCM directory");
            std::filesystem::path pcm_directory;
            if (argc == 4) {
                pcm_directory = argv[3];
                require(!std::filesystem::exists(pcm_directory) &&
                            std::filesystem::create_directory(pcm_directory),
                        "PCM output directory must be new");
            }
            std::array<int16_t, 2048> zero{};
            for (unsigned profile = 0; profile < 3; ++profile)
                for (unsigned rate : {48000U, 96000U, 192000U})
                    for (const char *kind : {"silence", "impulse", "tone", "noise"}) {
                        auto input = dsee::test::fixture(kind);
                        Profile p(resources, rate, profile);
                        std::vector<int32_t> pcm;
                        for (size_t block = 0; block < 48; ++block) {
                            auto *out = p.block(input.data() + block * 2048);
                            pcm.insert(pcm.end(), out, out + p.output_samples());
                        }
                        auto prefix = sha256(pcm.data(), pcm.size() * 4);
                        unsigned quiet = 0, tail = 0;
                        while (quiet < 4 && tail < 48) {
                            auto *out = p.block(zero.data());
                            quiet = std::all_of(out, out + p.output_samples(),
                                                [](auto v) { return v == 0; })
                                        ? quiet + 1
                                        : 0;
                            pcm.insert(pcm.end(), out, out + p.output_samples());
                            ++tail;
                        }
                        require(quiet == 4, "Guest tail did not become silent");
                        p.close();
                        if (!pcm_directory.empty()) {
                            auto name = std::string(kind) + "-" + profiles[profile] + "-" +
                                        std::to_string(rate) + ".s32";
                            std::ofstream out(pcm_directory / name, std::ios::binary);
                            require(bool(out.write(reinterpret_cast<const char *>(pcm.data()),
                                                   std::streamsize(pcm.size() * 4))),
                                    "Cannot write synthetic PCM fixture");
                        }
                        std::cout << "{\"event\":\"case\",\"stimulus\":\"" << kind
                                  << "\",\"profile\":\"" << profiles[profile]
                                  << "\",\"rate\":" << rate << ",\"sha256\":\"" << prefix
                                  << "\",\"full_sha256\":\"" << sha256(pcm.data(), pcm.size() * 4)
                                  << "\",\"input_sha256\":\""
                                  << sha256(input.data(), input.size() * 2)
                                  << "\",\"output_frames\":" << pcm.size() / 2
                                  << ",\"tail_blocks\":" << tail
                                  << ",\"import_calls\":" << p.calls() << "}" << std::endl;
                    }
        } else {
            std::array<int16_t, 2048> input{};
            uint32_t rng = 12345;
            for (auto &v : input) {
                rng = rng * 1664525 + 1013904223;
                v = int16_t(int(rng >> 16) / 8 - 4096);
            }
            for (unsigned repeat = 1; repeat <= repeats; ++repeat)
                for (unsigned profile = 0; profile < 3; ++profile)
                    for (unsigned rate : {48000U, 96000U, 192000U}) {
                        Profile p(resources, rate, profile);
                        for (unsigned i = 0; i < 64; ++i)
                            p.block(input.data());
                        auto imports = p.calls();
                        auto start = std::chrono::steady_clock::now();
                        for (unsigned i = 0; i < blocks; ++i)
                            p.block(input.data());
                        double seconds =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                                .count();
                        auto calls = p.calls() - imports;
                        p.close();
                        double audio = double(blocks) * 1024 / 48000;
                        std::cout << "{\"event\":\"benchmark\",\"profile\":\"" << profiles[profile]
                                  << "\",\"rate\":" << rate << ",\"repeat\":" << repeat
                                  << ",\"warmup_blocks\":64,\"blocks\":" << blocks
                                  << ",\"audio_seconds\":" << audio << ",\"seconds\":" << seconds
                                  << ",\"realtime_factor\":" << audio / seconds
                                  << ",\"import_calls\":" << calls << "}" << std::endl;
                    }
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << dsee::emulation::engine_name() << " probe: " << e.what() << '\n';
        return 1;
    }
}
