#include "dsee_postprocess.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F fn, const std::string &message) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, message);
}
struct Source : dsee::AudioSource {
    unsigned rate;
    std::vector<dsee::StereoFrame> audio;
    size_t chunk = SIZE_MAX;
    Source(unsigned fs, size_t count) : rate(fs), audio(count) {}
    unsigned sample_rate() const override {
        return rate;
    }
    uint64_t frame_count() const override {
        return audio.size();
    }
    size_t read_at(uint64_t first, dsee::StereoFrame *out, size_t count) override {
        if (first >= audio.size())
            return 0;
        count = std::min({count, audio.size() - size_t(first), chunk});
        std::copy_n(audio.begin() + first, count, out);
        return count;
    }
    void tone(double hz, double amplitude, double right = 0.7) {
        for (size_t i = 0; i < audio.size(); ++i) {
            double value = amplitude * std::sin(2 * pi * hz * i / rate);
            audio[i].left += value;
            audio[i].right += right * value;
        }
    }
    void noise() {
        uint32_t rng = 18251;
        for (auto &frame : audio) {
            rng = 1664525 * rng + 1013904223;
            frame.left = (double(rng >> 8) / 16777216.0 - 0.5) * 0.2;
            frame.right = -frame.left;
        }
    }
};
struct Run {
    dsee::PostprocessResult result;
    std::vector<int32_t> pcm;
    Run(Source &processed, Source *reference, dsee::PostprocessOptions options) {
        result = dsee::postprocess(processed, reference, options,
                                   [&](const int32_t *values, size_t frames) {
                                       pcm.insert(pcm.end(), values, values + 2 * frames);
                                   });
        check(result.frames == processed.audio.size() && pcm.size() == 2 * result.frames,
              "Output frame count changed");
    }
    double amplitude(double hz, unsigned rate, size_t channel = 0) const {
        size_t edge = rate / 10, end = pcm.size() / 2 - edge;
        double sine = 0, cosine = 0;
        for (size_t i = edge; i < end; ++i) {
            double phase = 2 * pi * hz * i / rate;
            double value = pcm[2 * i + channel] / 2147483648.0;
            sine += value * std::sin(phase);
            cosine += value * std::cos(phase);
        }
        return 2 * std::hypot(sine, cosine) / (end - edge);
    }
};
dsee::PostprocessOptions mix(double amount = 0.5) {
    dsee::PostprocessOptions o;
    o.mode = dsee::PostprocessMode::Mix;
    o.mix = amount;
    o.reference_delay = 0;
    return o;
}
void reconstruction() {
    for (unsigned rate : {48000, 96000, 192000})
        for (size_t length : {size_t(0), size_t(1), size_t(137), size_t(2048), size_t(12317)}) {
            Source source(rate, length);
            source.noise();
            source.chunk = 137;
            if (length) {
                source.audio.front() = {-1, 2147483647 / 2147483648.0};
                source.audio.back() = {0.7, -0.6};
            }
            for (double amount : {0.0, 0.5, 1.0}) {
                Run output(source, &source, mix(amount));
                for (size_t i = 0; i < length; ++i) {
                    check(std::abs(output.pcm[2 * i] / 2147483648.0 - source.audio[i].left) < 3e-10,
                          "Complementary reconstruction, left/boundary");
                    check(std::abs(output.pcm[2 * i + 1] / 2147483648.0 - source.audio[i].right) <
                              3e-10,
                          "Complementary reconstruction, right/boundary");
                }
            }
            Run bypass(source, nullptr, {});
            check(bypass.result.frames == length, "Bypass length");
        }
    Source pcm32(96000, 6);
    int32_t values[] = {INT32_MIN, INT32_MAX, -1, 0, 1, 123456789};
    for (size_t i = 0; i < 6; ++i)
        pcm32.audio[i] = {values[i] / 2147483648.0, values[5 - i] / 2147483648.0};
    Run bypass(pcm32, nullptr, {});
    for (size_t i = 0; i < 6; ++i)
        check(bypass.pcm[2 * i] == values[i] && bypass.pcm[2 * i + 1] == values[5 - i],
              "PCM32 bypass must be bit exact");
}
void crossover() {
    for (unsigned rate : {48000, 96000, 192000}) {
        Source reference(rate, rate / 2 + 31), processed(rate, rate / 2 + 31);
        reference.tone(3000, 0.06);
        processed.tone(3000, 0.1);
        processed.tone(22500, 0.08);
        for (double amount : {0.0, 0.5, 1.0}) {
            Run result(processed, &reference, mix(amount));
            check(std::abs(result.amplitude(3000, rate) - 0.06) < 2e-4,
                  "Low band must use reference");
            check(std::abs(result.amplitude(22500, rate) - amount * 0.08) < 2e-4,
                  "High band mixing amplitude");
        }
    }
    Source silent(96000, 48000), processed(96000, 48000);
    processed.tone(19000, 0.1);
    for (auto curve : {dsee::CrossoverCurve::Linear, dsee::CrossoverCurve::Cosine}) {
        auto o = mix(1);
        o.curve = curve;
        Run result(processed, &silent, o);
        double expected = curve == dsee::CrossoverCurve::Linear ? 0.25 : (1 - std::cos(pi / 4)) / 2;
        check(std::abs(result.amplitude(19000, 96000) - 0.1 * expected) < 2e-5,
              "Crossover curve response");
    }
}
void delay() {
    Source reference(48000, 24013), processed(48000, 24767);
    reference.noise(); // Exactly anti-phase stereo, which must not be mono-summed.
    for (int offset : {-137, 371}) {
        std::fill(processed.audio.begin(), processed.audio.end(), dsee::StereoFrame{});
        for (size_t i = 0; i < processed.audio.size(); ++i) {
            int64_t index = int64_t(i) - offset;
            if (index >= 0 && size_t(index) < reference.audio.size())
                processed.audio[i] = reference.audio[size_t(index)];
        }
        auto o = mix(0);
        o.reference_delay.reset();
        Run automatic(processed, &reference, o);
        check(automatic.result.reference_delay == offset, "Automatic signed delay estimate");
        check(automatic.result.alignment_correlation &&
                  *automatic.result.alignment_correlation > 0.999,
              "Automatic correlation");
        o.reference_delay = offset;
        Run explicit_delay(processed, &reference, o);
        check(automatic.pcm == explicit_delay.pcm, "Automatic/explicit delay consistency");
        for (size_t i = 0; i < processed.audio.size(); ++i)
            check(std::abs(explicit_delay.pcm[2 * i] / 2147483648.0 - processed.audio[i].left) <
                      3e-10,
                  "Signed delay/padding/truncation");
    }
    Source silent(48000, 24000);
    auto o = mix();
    o.reference_delay.reset();
    rejects([&] { Run r(silent, &silent, o); }, "Auto alignment must reject silence");
    o.alignment_start = 1;
    rejects([&] { Run r(processed, &reference, o); }, "Out-of-range alignment start");
    o.alignment_start = 0;
    Source short_audio(48000, 10);
    rejects([&] { Run r(short_audio, &short_audio, o); },
            "Short alignment must require explicit delay");
}
void resampling() {
    for (auto rates : {std::pair<unsigned, unsigned>{44100, 96000},
                       {48000, 192000},
                       {96000, 48000},
                       {88200, 96000},
                       {176400, 192000}}) {
        Source reference(rates.first, rates.first / 2), processed(rates.second, rates.second / 2);
        reference.tone(3000, 0.1);
        auto o = mix(0);
        o.reference_gain_db = -6.020599913279624;
        Run result(processed, &reference, o);
        check(std::abs(result.amplitude(3000, rates.second) - 0.05) < 2e-6,
              "Rational resampling/gain accuracy");
        for (size_t i = rates.second / 10; i < result.result.frames - rates.second / 10; ++i)
            check(std::abs(result.pcm[2 * i] / 2147483648.0 -
                           0.05 * std::sin(2 * pi * 3000 * i / rates.second)) < 2e-6,
                  "Resampling introduced phase or time shift");
    }
    Source reference(192000, 96000), processed(48000, 24000);
    reference.tone(30000, 0.2);
    Run antialias(processed, &reference, mix(0));
    double alias = antialias.amplitude(18000, 48000);
    check(alias < 1e-6, "Downsampling anti-alias rejection: " + std::to_string(alias));
    Source edges(44100, 701), baseline(96000, 1900), delayed(96000, 2100);
    edges.audio.front() = {0.4, -0.4};
    edges.audio.back() = {-0.3, 0.3};
    auto o = mix(0);
    Run base(baseline, &edges, o);
    o.reference_delay = 37;
    Run shifted(delayed, &edges, o);
    for (size_t i = 0; i < shifted.result.frames; ++i) {
        int32_t expected = i >= 37 && i - 37 < base.result.frames ? base.pcm[2 * (i - 37)] : 0;
        check(shifted.pcm[2 * i] == expected,
              "Resampled delay must pad outside reference duration");
    }
}
void envelope() {
    Source reference(96000, 96000), processed(96000, 96000);
    reference.tone(3000, 0.06);
    for (double hz = 15250; hz < 19500; hz += 500)
        reference.tone(hz, 0.002 * std::pow(15000 / hz, 2));
    processed.audio = reference.audio;
    // Broad excess exercises the attenuation ceiling without frequency smoothing
    // diluting an isolated tone into empty neighboring measurement bands.
    for (double hz = 24250; hz < 34000; hz += 500)
        processed.tone(hz, 0.01);
    processed.tone(28000, 0.08);
    double previous = 1;
    for (double strength : {0.0, 0.5, 1.0}) {
        auto o = mix();
        o.mode = dsee::PostprocessMode::Adaptive;
        o.max_cut_db = 6;
        o.envelope_strength = strength;
        Run result(processed, &reference, o);
        double high = result.amplitude(28000, 96000);
        check(std::abs(high - 0.08 * std::pow(10.0, -6 * strength / 20)) < 2e-4,
              "Adaptive attenuation bound and strength: strength=" + std::to_string(strength) +
                  ", amplitude=" + std::to_string(high));
        check(std::abs(result.amplitude(3000, 96000) - 0.06) < 2e-4,
              "Adaptive low band preservation");
        check(high < previous, "Adaptive strength must reduce excess monotonically");
        check(std::abs(result.amplitude(28000, 96000, 1) / high - 0.7) < 1e-5,
              "Envelope must share gains between channels");
        previous = high;
        if (strength == 0) {
            Run bypass(processed, nullptr, {});
            check(bypass.pcm == result.pcm, "Zero-strength adaptive must be exact bypass");
        }
    }
    auto o = mix(0.5);
    o.mode = dsee::PostprocessMode::Combined;
    o.max_cut_db = 6;
    o.smoothing_ms = 0;
    Run combined(processed, &reference, o);
    check(std::abs(combined.amplitude(28000, 96000) - 0.04 * std::pow(10.0, -6.0 / 20)) < 2e-4,
          "Combined processing order");
    Source quiet(96000, processed.audio.size());
    o.mode = dsee::PostprocessMode::Adaptive;
    o.smoothing_ms = 150;
    Run no_confidence(processed, &quiet, o);
    check(std::abs(no_confidence.amplitude(28000, 96000) - 0.08) < 2e-4,
          "Silent reference must not cause arbitrary attenuation");
}
void errors() {
    Source source(96000, 100);
    auto o = mix();
    rejects([&] { Run r(source, nullptr, o); }, "Missing reference");
    for (double value : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN()}) {
        o.mix = value;
        rejects([&] { Run r(source, &source, o); }, "Invalid mix");
    }
    o = mix();
    o.reference_delay = INT64_MIN;
    rejects([&] { Run r(source, &source, o); }, "Extreme delay overflow");
    o = mix();
    o.crossover_high = 48000;
    rejects([&] { Run r(source, &source, o); }, "Nyquist crossover");
    o = mix();
    o.mode = dsee::PostprocessMode::Adaptive;
    o.fit_high = 23000;
    Source low_rate(44100, 100);
    rejects([&] { Run r(source, &low_rate, o); }, "Reference Nyquist fit");
    o = mix();
    source.audio[0].left = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { Run r(source, nullptr, {}); }, "Non-finite samples");
    source.audio[0].left = 1.1;
    rejects([&] { Run r(source, nullptr, {}); }, "PCM clipping");
    source.audio[0].left = 0;
    source.chunk = 0;
    rejects([&] { Run r(source, nullptr, {}); }, "Truncated source");
    source.chunk = SIZE_MAX;
    Source invalid_rate(0, 100);
    rejects([&] { Run r(source, &invalid_rate, mix()); }, "Zero reference rate");
    rejects([&] { dsee::postprocess(source, nullptr, {}, {}); }, "Missing sink");
    rejects(
        [&] {
            dsee::postprocess(source, nullptr, {}, [](const int32_t *, size_t) {
                throw std::runtime_error("Failed output");
            });
        },
        "Sink error propagation");
}
} // namespace
int main() {
    try {
        reconstruction();
        crossover();
        delay();
        resampling();
        envelope();
        errors();
        std::cout << "Native postprocessing: reconstruction, crossover, delay, resampling, "
                     "envelope and errors passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
