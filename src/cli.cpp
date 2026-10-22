#include "dsee_compat.h"
#include "wav.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
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
           "          [--postprocess none|adaptive|mix|combined] [controls below]\n"
           "  postprocess --input PROCESSED.wav --reference ORIGINAL.wav --output OUTPUT.wav\n"
           "              [--mode none|adaptive|mix|combined] [controls below]\n"
           "Input: 48 kHz stereo PCM16 WAV. Output: PCM32, default 96 kHz / pcm.\n"
           "Preserves algorithm latency and zero-fed tail. Never overwrites output.\n"
           "Postprocess reads stereo PCM16/PCM32 WAV; processed rate: 48/96/192 kHz.\n"
           "Reference rates: 44.1/48/88.2/96/176.4/192 kHz. No external runtime needed.\n"
           "Standalone default: mix. Process default: none; reference defaults to input.\n"
           "Controls (all optional; frequencies in Hz):\n"
           "  --mix 0.5|auto                Fixed 0..1 weight or per-track estimation\n"
           "  --auto-mix-objective seam|envelope  Default seam continuity\n"
           "  --auto-mix-min 0 --auto-mix-max 1 --auto-mix-fallback 0.5\n"
           "  --auto-mix-margin-db 2 --auto-mix-coverage 0.9\n"
           "  --curve linear|cosine         Default cosine\n"
           "  --crossover-low 18000 --crossover-high 22000\n"
           "  --envelope-strength 1 --max-cut-db 12\n"
           "  --fit-low 15000 --fit-high 19500\n"
           "  --envelope-low 18500 --envelope-high 22000\n"
           "  --smoothing-ms 150            Gaussian time-smoothing sigma\n"
           "  --reference-gain-db 0         Match any gain applied before DSEE\n"
           "  --reference-delay auto        Or signed delay in output-rate samples\n"
           "  --alignment-start 0           Start of automatic alignment excerpt, seconds\n"
           "  --reference ORIGINAL.wav      Optional separate reference for process\n"
           "Mode none needs no reference. Mix 0 selects the aligned reference.\n";
}
struct Deleter {
    void operator()(dsee_context *c) const {
        dsee_destroy(c);
    }
};
using Arguments = std::map<std::string, std::string>;
const std::map<std::string, double dsee::PostprocessOptions::*> controls = {
    {"--mix", &dsee::PostprocessOptions::mix},
    {"--auto-mix-min", &dsee::PostprocessOptions::auto_mix_min},
    {"--auto-mix-max", &dsee::PostprocessOptions::auto_mix_max},
    {"--auto-mix-fallback", &dsee::PostprocessOptions::auto_mix_fallback},
    {"--auto-mix-margin-db", &dsee::PostprocessOptions::auto_mix_margin_db},
    {"--auto-mix-coverage", &dsee::PostprocessOptions::auto_mix_coverage},
    {"--crossover-low", &dsee::PostprocessOptions::crossover_low},
    {"--crossover-high", &dsee::PostprocessOptions::crossover_high},
    {"--envelope-strength", &dsee::PostprocessOptions::envelope_strength},
    {"--max-cut-db", &dsee::PostprocessOptions::max_cut_db},
    {"--fit-low", &dsee::PostprocessOptions::fit_low},
    {"--fit-high", &dsee::PostprocessOptions::fit_high},
    {"--envelope-low", &dsee::PostprocessOptions::envelope_low},
    {"--envelope-high", &dsee::PostprocessOptions::envelope_high},
    {"--smoothing-ms", &dsee::PostprocessOptions::smoothing_ms},
    {"--reference-gain-db", &dsee::PostprocessOptions::reference_gain_db},
    {"--alignment-start", &dsee::PostprocessOptions::alignment_start},
};
bool postprocess_option(const std::string &key) {
    return controls.count(key) || key == "--curve" || key == "--reference" ||
           key == "--reference-delay" || key == "--auto-mix-objective";
}
dsee::PostprocessMode mode(const std::string &value) {
    if (value == "none")
        return dsee::PostprocessMode::None;
    if (value == "adaptive")
        return dsee::PostprocessMode::Adaptive;
    if (value == "mix")
        return dsee::PostprocessMode::Mix;
    if (value == "combined")
        return dsee::PostprocessMode::Combined;
    throw std::runtime_error("Mode must be none, adaptive, mix or combined");
}
dsee::PostprocessOptions settings(const Arguments &args, const std::string &selected) {
    dsee::PostprocessOptions result;
    result.mode = mode(selected);
    for (const auto &control : controls) {
        auto value = args.find(control.first);
        if (value == args.end())
            continue;
        if (control.first == "--mix" && value->second == "auto") {
            result.auto_mix = true;
            continue;
        }
        size_t used = 0;
        double number = std::stod(value->second, &used);
        if (used != value->second.size() || !std::isfinite(number))
            throw std::runtime_error("Expected finite number: " + control.first);
        result.*(control.second) = number;
    }
    if (args.count("--curve")) {
        auto curve = args.at("--curve");
        if (curve != "linear" && curve != "cosine")
            throw std::runtime_error("Curve must be linear or cosine");
        result.curve =
            curve == "linear" ? dsee::CrossoverCurve::Linear : dsee::CrossoverCurve::Cosine;
    }
    if (args.count("--reference-delay") && args.at("--reference-delay") != "auto") {
        size_t used = 0;
        result.reference_delay = std::stoll(args.at("--reference-delay"), &used);
        if (used != args.at("--reference-delay").size())
            throw std::runtime_error("Reference delay must be auto or an integer");
    }
    if (args.count("--auto-mix-objective")) {
        auto objective = args.at("--auto-mix-objective");
        if (objective != "seam" && objective != "envelope")
            throw std::runtime_error("Auto-mix objective must be seam or envelope");
        result.auto_mix_objective =
            objective == "seam" ? dsee::AutoMixObjective::Seam : dsee::AutoMixObjective::Envelope;
    }
    for (const auto &arg : args)
        if (arg.first.rfind("--auto-mix-", 0) == 0 && !result.auto_mix)
            throw std::runtime_error("Auto-mix controls require --mix auto");
    return result;
}
void report(const dsee::PostprocessResult &result, const std::string &selected,
            const dsee::PostprocessOptions &options) {
    std::cout << "{\"mode\":\"" << selected << "\",\"frames\":" << result.frames
              << ",\"reference_delay\":" << result.reference_delay << ",\"alignment_correlation\":";
    if (result.alignment_correlation)
        std::cout << *result.alignment_correlation;
    else
        std::cout << "null";
    std::cout << ",\"peak\":" << result.peak << ",\"rms\":" << result.rms
              << ",\"effective_mix\":" << result.effective_mix << ",\"auto_mix\":";
    if (result.auto_mix) {
        const auto &a = *result.auto_mix;
        std::cout << "{\"fallback\":" << (a.fallback ? "true" : "false") << ",\"reason\":\""
                  << a.reason << "\",\"analyzed_windows\":" << a.analyzed_windows
                  << ",\"active_windows\":" << a.active_windows
                  << ",\"reliable_windows\":" << a.reliable_windows
                  << ",\"fit_low_hz\":" << a.fit_low_hz << ",\"fit_high_hz\":" << a.fit_high_hz
                  << ",\"slope_db_per_octave\":" << a.slope_db_per_octave
                  << ",\"fit_error_db\":" << a.fit_error_db
                  << ",\"achieved_coverage\":" << a.achieved_coverage;
        auto optional_number = [](const char *key, const std::optional<double> &value) {
            std::cout << ",\"" << key << "\":";
            if (value)
                std::cout << *value;
            else
                std::cout << "null";
        };
        optional_number("envelope_mix", a.envelope_mix);
        optional_number("phase_limit", a.phase_limit);
        optional_number("seam_score_before", a.seam_score_before);
        optional_number("seam_score_after", a.seam_score_after);
        std::cout << '}';
    } else
        std::cout << "null";
    std::cout << ",\"curve\":\""
              << (options.curve == dsee::CrossoverCurve::Linear ? "linear" : "cosine")
              << "\",\"parameters\":{";
    bool first = true;
    for (const auto &control : controls) {
        std::cout << (first ? "" : ",") << '"' << control.first.substr(2) << "\":";
        if (control.first == "--mix" && options.auto_mix)
            std::cout << "\"auto\"";
        else
            std::cout << options.*(control.second);
        first = false;
    }
    std::cout << ",\"auto-mix-objective\":\""
              << (options.auto_mix_objective == dsee::AutoMixObjective::Seam ? "seam" : "envelope")
              << "\"}}";
}

// One anonymous PCM intermediate for the integrated offline path. It is closed
// and removed on every exit, and never contains external runtime resources.
class PcmSpool : public dsee::AudioSource {
    FILE *file = nullptr;
    unsigned rate;
    uint64_t frames = 0;

  public:
    explicit PcmSpool(unsigned sample_rate) : file(std::tmpfile()), rate(sample_rate) {
        if (!file)
            throw std::runtime_error("Cannot create temporary PCM spool");
    }
    ~PcmSpool() override {
        std::fclose(file);
    }
    unsigned sample_rate() const override {
        return rate;
    }
    uint64_t frame_count() const override {
        return frames;
    }
    void write(const int32_t *pcm, size_t count) {
        if (count > dsee::WavWriter::max_frames - frames ||
            std::fwrite(pcm, 8, count, file) != count)
            throw std::runtime_error("Cannot write temporary PCM spool");
        frames += count;
    }
    size_t read_at(uint64_t first, dsee::StereoFrame *out, size_t count) override {
        if (first >= frames)
            return 0;
        count = size_t(std::min<uint64_t>(count, frames - first));
        std::vector<int32_t> pcm(count * 2);
        if (fseeko(file, off_t(first * 8), SEEK_SET) ||
            std::fread(pcm.data(), 8, count, file) != count)
            throw std::runtime_error("Cannot read temporary PCM spool");
        for (size_t i = 0; i < count; ++i)
            out[i] = {pcm[2 * i] / 2147483648.0, pcm[2 * i + 1] / 2147483648.0};
        return count;
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        std::cout << std::setprecision(12);
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << dsee_version() << '\n';
            return 0;
        }
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
            help();
            return 0;
        }
        std::string command = argv[1];
        if (command != "inspect" && command != "process" && command != "postprocess")
            throw std::runtime_error("Unknown command");
        if (argc == 3 && std::string(argv[2]) == "--help") {
            help();
            return 0;
        }
        Arguments args;
        for (int i = 2; i < argc; i += 2) {
            std::string k = argv[i];
            if (i + 1 >= argc || args.count(k))
                throw std::runtime_error("Missing or duplicate option value");
            bool allowed = command == "inspect"
                               ? k == "--assets"
                               : k == "--input" || k == "--output" || postprocess_option(k) ||
                                     (command == "process" &&
                                      (k == "--assets" || k == "--output-rate" ||
                                       k == "--source-profile" || k == "--postprocess")) ||
                                     (command == "postprocess" && k == "--mode");
            if (!allowed)
                throw std::runtime_error("Unknown option: " + k);
            args[k] = argv[i + 1];
        }
        auto required = [&](const std::string &key) -> std::string {
            if (!args.count(key) || args.at(key).empty())
                throw std::runtime_error("Required: " + key);
            return args.at(key);
        };
        if (command == "inspect") {
            std::string root = required("--assets");
            check(dsee_inspect(root.c_str()));
            std::cout << "{\"supported\":true,\"firmware\":\"67.1.A.2.229\",\"profiles\":[\"pcm\","
                         "\"aac128\",\"mp3128\"]}\n";
            return 0;
        }
        auto input = required("--input"), output = required("--output");
        if (std::filesystem::weakly_canonical(input) == std::filesystem::weakly_canonical(output))
            throw std::runtime_error("Input and output must differ");
        if (std::filesystem::path(output).extension() != ".wav")
            throw std::runtime_error("Output must be a PCM32 .wav file");
        std::string selected = command == "postprocess" ? "mix" : "none";
        std::string selection_key = command == "postprocess" ? "--mode" : "--postprocess";
        if (args.count(selection_key))
            selected = args.at(selection_key);
        if (command == "process" && !args.count(selection_key))
            for (const auto &arg : args)
                if (postprocess_option(arg.first))
                    throw std::runtime_error("Use --postprocess to enable postprocessing controls");
        auto options = settings(args, selected);
        if (command == "postprocess") {
            dsee::WavReader processed(input, false);
            if (processed.frames > dsee::WavWriter::max_frames)
                throw std::runtime_error("PCM32 output would exceed the RIFF size limit");
            std::unique_ptr<dsee::WavReader> reference;
            if (options.active())
                reference = std::make_unique<dsee::WavReader>(required("--reference"), false);
            options.validate(processed.sample_rate(), reference ? reference->sample_rate() : 0);
            dsee::WavWriter writer(output, processed.sample_rate());
            auto start = std::chrono::steady_clock::now();
            auto result = dsee::postprocess(
                processed, reference.get(), options,
                [&](const int32_t *pcm, size_t count) { writer.write(pcm, count); });
            writer.commit();
            std::cout
                << "{\"input_frames\":" << processed.frames
                << ",\"output_frames\":" << writer.frames
                << ",\"output_rate\":" << processed.sample_rate() << ",\"seconds\":"
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                << ",\"postprocess\":";
            report(result, selected, options);
            std::cout << "}\n";
            return 0;
        }
        std::string root = required("--assets");
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
        std::unique_ptr<dsee::WavReader> separate_reference;
        if (options.active() && args.count("--reference"))
            separate_reference = std::make_unique<dsee::WavReader>(required("--reference"), false);
        dsee::AudioSource *reference = separate_reference ? separate_reference.get() : &reader;
        options.validate(rate, reference->sample_rate());
        uint64_t padded = (reader.frames + 1023) / 1024 * 1024;
        if ((padded + (reader.frames ? 48 * 1024 : 0)) * (rate / 48000) >
            dsee::WavWriter::max_frames)
            throw std::runtime_error("Output including maximum tail may exceed RIFF limit");
        dsee_context *raw = nullptr;
        check(dsee_create(root.c_str(), rate, p, &raw));
        std::unique_ptr<dsee_context, Deleter> ctx(raw);
        dsee::WavWriter writer(output, rate);
        std::unique_ptr<PcmSpool> spool;
        if (options.active())
            spool = std::make_unique<PcmSpool>(rate);
        auto write = [&](const int32_t *pcm, size_t count) {
            if (spool)
                spool->write(pcm, count);
            else
                writer.write(pcm, count);
        };
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
                write(out.data(), made);
            }
        }
        int done = 0;
        while (!done) {
            size_t made = 0, pad = 0;
            check(dsee_finish(ctx.get(), out.data(), 4096, &made, &pad, &done));
            padding_frames += pad;
            write(out.data(), made);
        }
        check(dsee_destroy(ctx.release()));
        std::optional<dsee::PostprocessResult> post_result;
        if (spool)
            post_result = dsee::postprocess(
                *spool, reference, options,
                [&](const int32_t *pcm, size_t count) { writer.write(pcm, count); });
        writer.commit();
        double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "{\"input_frames\":" << reader.frames
                  << ",\"padding_frames\":" << padding_frames
                  << ",\"output_frames\":" << writer.frames
                  << ",\"input_rate\":48000,\"output_rate\":" << rate << ",\"source_profile\":\""
                  << profile << "\",\"seconds\":" << elapsed;
        if (post_result) {
            std::cout << ",\"postprocess\":";
            report(*post_result, selected, options);
        }
        std::cout << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "dsee-compat: " << e.what() << '\n';
        return 1;
    }
}
