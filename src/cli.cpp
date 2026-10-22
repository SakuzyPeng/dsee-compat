#include "dsee_compat.h"
#include "wav.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
namespace {
void check(dsee_status s) {
    if (s != DSEE_OK)
        throw std::runtime_error(dsee_last_error());
}
void help() {
    std::cout
        << "dsee-compat " << dsee_version()
        << " (unofficial compatibility runtime)\n"
           "  inspect --assets ROOT\n"
           "  process --assets ROOT --input INPUT.wav --output OUTPUT.wav\n"
           "          [--output-rate 48000|96000|192000] [--source-profile pcm|aac128|mp3128]\n"
           "Input: 48 kHz stereo PCM16 WAV. Output: PCM32, default 96 kHz / pcm.\n"
           "Preserves algorithm latency and zero-fed tail. Never overwrites output.\n";
}
struct Deleter {
    void operator()(dsee_context *c) const {
        dsee_destroy(c);
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << dsee_version() << '\n';
            return 0;
        }
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
            help();
            return 0;
        }
        std::string command = argv[1];
        if (command != "inspect" && command != "process")
            throw std::runtime_error("Unknown command");
        std::map<std::string, std::string> args;
        for (int i = 2; i < argc; i += 2) {
            std::string k = argv[i];
            if (i + 1 >= argc || args.count(k))
                throw std::runtime_error("Missing or duplicate option value");
            if (k != "--assets" &&
                (command != "process" || (k != "--input" && k != "--output" &&
                                          k != "--output-rate" && k != "--source-profile")))
                throw std::runtime_error("Unknown option: " + k);
            args[k] = argv[i + 1];
        }
        auto required = [&](const std::string &key) -> std::string {
            if (!args.count(key) || args.at(key).empty())
                throw std::runtime_error("Required: " + key);
            return args.at(key);
        };
        std::string root = required("--assets");
        if (command == "inspect") {
            check(dsee_inspect(root.c_str()));
            std::cout << "{\"supported\":true,\"firmware\":\"67.1.A.2.229\",\"profiles\":[\"pcm\","
                         "\"aac128\",\"mp3128\"]}\n";
            return 0;
        }
        auto input = required("--input"), output = required("--output");
        if (std::filesystem::weakly_canonical(input) == std::filesystem::weakly_canonical(output))
            throw std::runtime_error("Input and output must differ");
        unsigned rate = 96000;
        if (args.count("--output-rate")) {
            auto r = args.at("--output-rate");
            if (r != "48000" && r != "96000" && r != "192000")
                throw std::runtime_error("Unsupported output rate");
            rate = unsigned(std::stoul(r));
        }
        std::string profile = args.count("--source-profile") ? args.at("--source-profile") : "pcm";
        dsee_profile p = profile == "pcm"      ? DSEE_PCM
                         : profile == "aac128" ? DSEE_AAC128
                         : profile == "mp3128" ? DSEE_MP3128
                                               : dsee_profile(-1);
        if (int(p) < 0)
            throw std::runtime_error("Unsupported source profile");
        dsee::WavReader reader(input);
        uint64_t padded = (reader.frames + 1023) / 1024 * 1024;
        if ((padded + (reader.frames ? 48 * 1024 : 0)) * (rate / 48000) >
            dsee::WavWriter::max_frames)
            throw std::runtime_error("Output including maximum tail may exceed RIFF limit");
        dsee_context *raw = nullptr;
        check(dsee_create(root.c_str(), rate, p, &raw));
        std::unique_ptr<dsee_context, Deleter> ctx(raw);
        dsee::WavWriter writer(output, rate);
        std::array<int16_t, 2048> in{};
        std::array<int32_t, 8192> out{};
        uint64_t padding_frames = 0;
        auto start = std::chrono::steady_clock::now();
        while (size_t n = reader.read(in.data(), 1024)) {
            size_t pos = 0;
            while (pos < n) {
                size_t used = 0, made = 0;
                check(dsee_process(ctx.get(), in.data() + 2 * pos, n - pos, out.data(), 4096, &used,
                                   &made));
                if (!used && !made)
                    throw std::runtime_error("Processing made no progress");
                pos += used;
                writer.write(out.data(), made);
            }
        }
        int done = 0;
        while (!done) {
            size_t made = 0, pad = 0;
            check(dsee_finish(ctx.get(), out.data(), 4096, &made, &pad, &done));
            padding_frames += pad;
            writer.write(out.data(), made);
        }
        check(dsee_destroy(ctx.release()));
        writer.commit();
        double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "{\"input_frames\":" << reader.frames
                  << ",\"padding_frames\":" << padding_frames
                  << ",\"output_frames\":" << writer.frames
                  << ",\"input_rate\":48000,\"output_rate\":" << rate << ",\"source_profile\":\""
                  << profile << "\",\"seconds\":" << elapsed << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "dsee-compat: " << e.what() << '\n';
        return 1;
    }
}
