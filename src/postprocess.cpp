#include "dsee_postprocess.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace dsee {
namespace {
constexpr double pi = 3.14159265358979323846;
using Complex = std::complex<double>;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool supported_reference(unsigned rate) {
    return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000 || rate == 176400 ||
           rate == 192000;
}
double transition(double hz, double low, double high, CrossoverCurve curve) {
    double t = std::clamp((hz - low) / (high - low), 0.0, 1.0);
    return curve == CrossoverCurve::Linear ? t : (1 - std::cos(pi * t)) * 0.5;
}
size_t power_of_two(size_t count) {
    size_t n = 1;
    while (n < count) {
        require(n <= SIZE_MAX / 2, "FFT size overflow");
        n *= 2;
    }
    return n;
}

// Radix-2 complex FFT. Plans are local to a call and reused for all audio frames.
class Fft {
    size_t n;
    std::vector<size_t> reverse;
    std::vector<Complex> roots;

  public:
    explicit Fft(size_t size) : n(size), reverse(n), roots(n / 2) {
        for (size_t i = 1; i < n; ++i)
            reverse[i] = (reverse[i / 2] / 2) + (i % 2) * (n / 2);
        for (size_t i = 0; i < roots.size(); ++i)
            roots[i] = std::polar(1.0, -2 * pi * i / n);
    }
    void run(std::vector<Complex> &x, bool inverse = false) const {
        require(x.size() == n, "FFT buffer size mismatch");
        for (size_t i = 0; i < n; ++i)
            if (i < reverse[i])
                std::swap(x[i], x[reverse[i]]);
        for (size_t width = 2; width <= n; width *= 2) {
            size_t half = width / 2, step = n / width;
            for (size_t base = 0; base < n; base += width)
                for (size_t j = 0; j < half; ++j) {
                    auto root = inverse ? std::conj(roots[j * step]) : roots[j * step];
                    auto even = x[base + j], odd = x[base + j + half] * root;
                    x[base + j] = even + odd;
                    x[base + j + half] = even - odd;
                }
        }
        if (inverse)
            for (auto &v : x)
                v /= double(n);
    }
};

void read_exact(AudioSource &source, uint64_t first, StereoFrame *out, size_t count) {
    size_t done = 0;
    while (done < count) {
        size_t n = source.read_at(first + done, out + done, count - done);
        require(n && n <= count - done, "Audio source ended early or returned an invalid count");
        for (size_t i = done; i < done + n; ++i)
            require(std::isfinite(out[i].left) && std::isfinite(out[i].right),
                    "Non-finite audio sample");
        done += n;
    }
}
void read_padded(AudioSource &source, int64_t first, StereoFrame *out, size_t count) {
    std::fill_n(out, count, StereoFrame{});
    size_t skip = first < 0 ? size_t(std::min<uint64_t>(uint64_t(-first), count)) : 0;
    first += int64_t(skip);
    if (skip == count || uint64_t(first) >= source.frame_count())
        return;
    size_t take = size_t(std::min<uint64_t>(count - skip, source.frame_count() - first));
    read_exact(source, uint64_t(first), out + skip, take);
}

// Rational polyphase, zero-phase windowed-sinc reference resampling. No audio
// cache survives a call; same-rate reads preserve samples exactly before gain.
class Reference : public AudioSource {
    AudioSource &source;
    unsigned rate, input_rate, phases, step;
    int radius;
    double gain;
    uint64_t frames;
    int64_t delay = 0;
    std::vector<double> coefficients;

  public:
    Reference(AudioSource &input, unsigned output_rate, double gain_db)
        : source(input), rate(output_rate), input_rate(input.sample_rate()),
          phases(rate / std::gcd(rate, input_rate)), step(input_rate / std::gcd(rate, input_rate)),
          radius(int(std::ceil(64.0 / std::min(1.0, double(rate) / input_rate)))),
          gain(std::pow(10.0, gain_db / 20)),
          frames((input.frame_count() * rate + input_rate - 1) / input_rate) {
        if (input_rate == rate)
            return;
        double cutoff = 0.96 * std::min(1.0, double(rate) / input_rate);
        size_t taps = size_t(2 * radius + 1);
        coefficients.resize(phases * taps);
        for (unsigned p = 0; p < phases; ++p) {
            double total = 0, fraction = double(p) / phases;
            for (int j = -radius; j <= radius; ++j) {
                double t = j - fraction, value = 0;
                if (std::abs(t) <= radius) {
                    double x = pi * cutoff * t;
                    double sinc = std::abs(x) < 1e-14 ? 1.0 : std::sin(x) / x;
                    double window = 0.42 + 0.5 * std::cos(pi * t / radius) +
                                    0.08 * std::cos(2 * pi * t / radius);
                    value = cutoff * sinc * window;
                }
                coefficients[p * taps + j + radius] = value;
                total += value;
            }
            for (size_t j = 0; j < taps; ++j)
                coefficients[p * taps + j] /= total;
        }
    }
    unsigned sample_rate() const override {
        return rate;
    }
    uint64_t frame_count() const override {
        return frames;
    }
    void align(int64_t samples, uint64_t length) {
        delay = samples;
        frames = length;
    }
    size_t read_at(uint64_t first, StereoFrame *out, size_t count) override {
        if (first >= frames || !count)
            return 0;
        size_t requested = size_t(std::min<uint64_t>(count, frames - first));
        std::fill_n(out, requested, StereoFrame{});
        int64_t begin = int64_t(first) - delay;
        size_t skip = begin < 0 ? size_t(std::min<uint64_t>(uint64_t(-begin), requested)) : 0;
        begin += int64_t(skip);
        uint64_t available = (source.frame_count() * rate + input_rate - 1) / input_rate;
        if (skip == requested || uint64_t(begin) >= available)
            return requested;
        count = size_t(std::min<uint64_t>(requested - skip, available - uint64_t(begin)));
        out += skip;
        if (input_rate == rate) {
            read_padded(source, begin, out, count);
            for (size_t i = 0; i < count; ++i) {
                out[i].left *= gain;
                out[i].right *= gain;
            }
            return requested;
        }
        // Floor division is needed for samples preceding the reference start.
        auto position = [&](int64_t frame) {
            int64_t numerator = frame * step;
            int64_t whole = numerator / phases, part = numerator % phases;
            if (part < 0) {
                --whole;
                part += phases;
            }
            return std::pair<int64_t, size_t>(whole, size_t(part));
        };
        int64_t low = position(begin).first - radius;
        int64_t high = position(begin + int64_t(count) - 1).first + radius + 1;
        std::vector<StereoFrame> input(size_t(high - low));
        read_padded(source, low, input.data(), input.size());
        size_t taps = size_t(2 * radius + 1);
        for (size_t i = 0; i < count; ++i) {
            auto p = position(begin + int64_t(i));
            const double *weights = coefficients.data() + p.second * taps;
            const StereoFrame *values = input.data() + (p.first - radius - low);
            StereoFrame sum{};
            for (size_t j = 0; j < taps; ++j) {
                sum.left += values[j].left * weights[j];
                sum.right += values[j].right * weights[j];
            }
            out[i] = {sum.left * gain, sum.right * gain};
        }
        return requested;
    }
};

std::pair<int64_t, double> estimate_delay(AudioSource &processed, AudioSource &reference,
                                          double start_seconds) {
    unsigned rate = processed.sample_rate();
    uint64_t start = uint64_t(start_seconds * rate);
    require(start < processed.frame_count() && start < reference.frame_count(),
            "Alignment excerpt is outside audio; choose --alignment-start or --reference-delay");
    size_t a_count = size_t(std::min<uint64_t>(8ULL * rate, processed.frame_count() - start));
    size_t b_count = size_t(std::min<uint64_t>(8ULL * rate, reference.frame_count() - start));
    require(std::min(a_count, b_count) >= rate / 4,
            "Auto alignment needs at least 250 ms; set --reference-delay for short audio");
    std::vector<StereoFrame> a(a_count), b(b_count);
    read_exact(processed, start, a.data(), a.size());
    read_exact(reference, start, b.data(), b.size());
    double left = 0, right = 0;
    for (auto v : b) {
        left += v.left * v.left;
        right += v.right * v.right;
    }
    require(std::max(left, right) > 1e-16, "Silent reference excerpt; set --reference-delay");
    bool use_left = left >= right;
    size_t n = power_of_two(a_count + b_count - 1);
    Fft fft(n);
    std::vector<Complex> x(n), y(n), correlation(n);
    for (size_t i = 0; i < a_count; ++i)
        x[i] = use_left ? a[i].left : a[i].right;
    for (size_t i = 0; i < b_count; ++i)
        y[i] = use_left ? b[i].left : b[i].right;
    fft.run(x);
    fft.run(y);
    for (size_t k = 0; k < n; ++k) {
        double hz = double(std::min(k, n - k)) * rate / n;
        double filter = 1 - transition(hz, 8000, 10000, CrossoverCurve::Cosine);
        x[k] *= filter;
        y[k] *= filter;
        correlation[k] = x[k] * std::conj(y[k]);
    }
    fft.run(correlation, true);
    int64_t limit = rate / 10, lag = 0;
    double best = -std::numeric_limits<double>::infinity();
    for (int64_t candidate = -limit; candidate <= limit; ++candidate) {
        double value =
            correlation[size_t(candidate < 0 ? int64_t(n) + candidate : candidate)].real();
        if (value > best) {
            best = value;
            lag = candidate;
        }
    }
    require(std::abs(lag) < limit, "Alignment reached search boundary; set --reference-delay");
    fft.run(x, true);
    fft.run(y, true);
    size_t a_start = size_t(std::max<int64_t>(lag, 0));
    size_t b_start = size_t(std::max<int64_t>(-lag, 0));
    size_t count = std::min(a_count - a_start, b_count - b_start);
    // Exclude the filter's excerpt edges when validating the correlation.
    size_t edge = rate / 100;
    require(count > 2 * edge, "Alignment excerpt has insufficient overlap");
    double ax = 0, by = 0, xx = 0, yy = 0, xy = 0;
    for (size_t i = edge; i < count - edge; ++i) {
        double u = x[a_start + i].real(), v = y[b_start + i].real();
        ax += u;
        by += v;
        xx += u * u;
        yy += v * v;
        xy += u * v;
    }
    double samples = double(count - 2 * edge);
    double variance_x = xx - ax * ax / samples, variance_y = yy - by * by / samples;
    require(variance_x > 1e-16 && variance_y > 1e-16,
            "Silent low-band alignment excerpt; set --reference-delay");
    double coefficient = (xy - ax * by / samples) / std::sqrt(variance_x * variance_y);
    require(std::isfinite(coefficient) && coefficient >= 0.99,
            "Uncertain alignment (correlation below 0.99); choose --alignment-start or "
            "--reference-delay");
    return {lag, std::min(coefficient, 1.0)};
}

class WindowStream {
    AudioSource &source;
    size_t hop;
    int64_t first;

  public:
    std::vector<StereoFrame> audio;
    WindowStream(AudioSource &input, size_t n)
        : source(input), hop(n / 4), first(-int64_t(n / 2)), audio(n) {
        read_padded(source, first, audio.data(), audio.size());
    }
    void next() {
        first += int64_t(hop);
        std::move(audio.begin() + hop, audio.end(), audio.begin());
        read_padded(source, first + int64_t(audio.size() - hop), audio.data() + audio.size() - hop,
                    hop);
    }
};
struct Spectrum {
    std::vector<Complex> left, right;
    explicit Spectrum(size_t n) : left(n), right(n) {}
    void analyze(const WindowStream &source, const std::vector<double> &window, const Fft &fft) {
        for (size_t i = 0; i < window.size(); ++i) {
            left[i] = source.audio[i].left * window[i];
            right[i] = source.audio[i].right * window[i];
        }
        fft.run(left);
        fft.run(right);
    }
};

using Matrix = std::vector<float>;
std::vector<double> gaussian(double sigma) {
    size_t radius = size_t(std::ceil(4 * sigma));
    std::vector<double> weights(2 * radius + 1);
    double total = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        double x = (double(i) - radius) / sigma;
        total += weights[i] = std::exp(-0.5 * x * x);
    }
    for (auto &v : weights)
        v /= total;
    return weights;
}
void smooth_time(Matrix &matrix, size_t rows, size_t columns, double sigma) {
    if (sigma < 0.01 || rows < 2)
        return;
    auto weights = gaussian(sigma);
    size_t radius = weights.size() / 2;
    size_t n = power_of_two(rows + 4 * radius);
    Fft fft(n);
    std::vector<Complex> kernel(n), values(n);
    for (size_t i = 0; i < weights.size(); ++i)
        kernel[i] = weights[i];
    fft.run(kernel);
    for (size_t c = 0; c < columns; ++c) {
        std::fill(values.begin(), values.end(), Complex{});
        for (size_t i = 0; i < rows + 2 * radius; ++i) {
            size_t row = i < radius ? 0 : std::min(i - radius, rows - 1);
            values[i] = matrix[row * columns + c];
        }
        fft.run(values);
        for (size_t i = 0; i < n; ++i)
            values[i] *= kernel[i];
        fft.run(values, true);
        for (size_t i = 0; i < rows; ++i)
            matrix[i * columns + c] = float(values[i + 2 * radius].real());
    }
}
void smooth_frequency(Matrix &matrix, size_t rows, size_t columns) {
    auto weights = gaussian(1.5); // 750 Hz sigma with 500 Hz measurements.
    int radius = int(weights.size() / 2);
    std::vector<float> row(columns);
    for (size_t t = 0; t < rows; ++t) {
        for (size_t c = 0; c < columns; ++c) {
            double value = 0;
            for (int j = -radius; j <= radius; ++j)
                value += weights[size_t(j + radius)] *
                         matrix[t * columns + size_t(std::clamp(int(c) + j, 0, int(columns) - 1))];
            row[c] = float(value);
        }
        std::copy(row.begin(), row.end(), matrix.begin() + t * columns);
    }
}
double median(std::vector<double> &values) {
    size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    double result = values[middle];
    if (values.size() % 2 == 0)
        result = (result + *std::max_element(values.begin(), values.begin() + middle)) * 0.5;
    return result;
}

struct Envelope {
    std::vector<double> centers;
    Matrix gains;
    Envelope(AudioSource &processed, AudioSource &reference, const PostprocessOptions &options,
             const std::vector<double> &window, const Fft &fft) {
        unsigned rate = processed.sample_rate();
        size_t n = window.size(), hop = n / 4;
        size_t rows = size_t(processed.frame_count() / hop + 1);
        double start = std::max(
            0.0, std::floor((std::min(options.fit_low, options.envelope_low) - 3000) / 500) * 500);
        for (double edge = start; edge < rate / 2.0; edge += 500)
            centers.push_back((edge + std::min(edge + 500, rate / 2.0)) * 0.5);
        size_t bands = centers.size();
        require(rows <= SIZE_MAX / bands, "Envelope size overflow");
        Matrix ref(rows * bands), out(rows * bands);
        std::vector<size_t> lo(bands), hi(bands), fit;
        for (size_t c = 0; c < bands; ++c) {
            lo[c] = size_t(std::ceil((start + 500 * c) * n / rate));
            hi[c] =
                c + 1 == bands ? n / 2 + 1 : size_t(std::ceil((start + 500 * (c + 1)) * n / rate));
            if (centers[c] >= options.fit_low && centers[c] <= options.fit_high)
                fit.push_back(c);
        }
        require(fit.size() >= 3, "Envelope fit needs at least three 500 Hz bands");
        double scale =
            1.0 / (rate * std::inner_product(window.begin(), window.end(), window.begin(), 0.0));
        WindowStream a(processed, n), b(reference, n);
        Spectrum spec(n);
        for (size_t t = 0; t < rows; ++t) {
            auto measure = [&](WindowStream &stream, Matrix &target) {
                spec.analyze(stream, window, fft);
                for (size_t c = 0; c < bands; ++c) {
                    double sum = 0;
                    for (size_t k = lo[c]; k < hi[c]; ++k)
                        sum += (std::norm(spec.left[k]) + std::norm(spec.right[k])) *
                               (k == 0 || k == n / 2 ? 0.5 : 1.0);
                    target[t * bands + c] = float(sum * scale / (hi[c] - lo[c]));
                }
            };
            measure(a, out);
            measure(b, ref);
            if (t + 1 < rows) {
                a.next();
                b.next();
            }
        }
        double sigma = options.smoothing_ms * rate / (1000 * hop);
        smooth_time(ref, rows, bands, sigma);
        smooth_time(out, rows, bands, sigma);
        Matrix slopes(rows), confidence(rows);
        std::vector<double> logarithms(bands), pairs, anchors;
        for (size_t c = 0; c < bands; ++c)
            logarithms[c] = std::log2(centers[c] / options.fit_high);
        for (size_t t = 0; t < rows; ++t) {
            double power = 0;
            for (auto c : fit)
                power += std::max(0.0f, ref[t * bands + c]) * 500;
            confidence[t] =
                float(std::clamp((10 * std::log10(std::max(power, 1e-20)) + 90) / 20, 0.0, 1.0));
            for (size_t c = 0; c < bands; ++c) {
                ref[t * bands + c] =
                    float(10 * std::log10(std::max(double(ref[t * bands + c]), 1e-20)));
                out[t * bands + c] =
                    float(10 * std::log10(std::max(double(out[t * bands + c]), 1e-20)));
            }
            pairs.clear();
            for (size_t i = 0; i < fit.size(); ++i)
                for (size_t j = i + 1; j < fit.size(); ++j)
                    pairs.push_back((ref[t * bands + fit[j]] - ref[t * bands + fit[i]]) /
                                    (logarithms[fit[j]] - logarithms[fit[i]]));
            slopes[t] = float(std::clamp(median(pairs), -48.0, -12.0));
        }
        smooth_time(slopes, rows, 1, sigma * 5 / 3);
        smooth_time(confidence, rows, 1, sigma);
        for (size_t t = 0; t < rows; ++t) {
            anchors.clear();
            double slope = std::clamp(double(slopes[t]), -48.0, -12.0);
            for (auto c : fit)
                anchors.push_back(ref[t * bands + c] - slope * logarithms[c]);
            double anchor = median(anchors);
            for (size_t c = 0; c < bands; ++c) {
                double target = anchor + slope * logarithms[c] + 2;
                out[t * bands + c] =
                    float(std::clamp(target - out[t * bands + c], -options.max_cut_db, 0.0));
            }
        }
        smooth_frequency(out, rows, bands);
        smooth_time(out, rows, bands, sigma * 0.8);
        for (size_t t = 0; t < rows; ++t)
            for (size_t c = 0; c < bands; ++c)
                out[t * bands + c] =
                    float(std::clamp(double(out[t * bands + c]), -options.max_cut_db, 0.0) *
                          std::clamp(double(confidence[t]), 0.0, 1.0) * options.envelope_strength);
        gains = std::move(out);
    }
    double gain(size_t frame, double frequency) const {
        auto upper = std::upper_bound(centers.begin(), centers.end(), frequency);
        size_t hi = size_t(upper - centers.begin());
        if (hi == 0)
            return gains[frame * centers.size()];
        if (hi == centers.size())
            return gains[(frame + 1) * centers.size() - 1];
        double t = (frequency - centers[hi - 1]) / (centers[hi] - centers[hi - 1]);
        return (1 - t) * gains[frame * centers.size() + hi - 1] +
               t * gains[frame * centers.size() + hi];
    }
};

class Output {
    const PcmSink &sink;
    long double squares = 0;
    std::vector<int32_t> pcm;

  public:
    PostprocessResult result;
    explicit Output(const PcmSink &output) : sink(output) {}
    void write(const StereoFrame *audio, size_t count) {
        pcm.resize(count * 2);
        for (size_t i = 0; i < count; ++i) {
            double values[] = {audio[i].left, audio[i].right};
            for (size_t c = 0; c < 2; ++c) {
                double value = values[c];
                require(std::isfinite(value), "Non-finite postprocessing output");
                // Tolerate only sub-LSB floating-point reconstruction error at -1.
                require(value >= -1 - 1e-12 && value < 1,
                        "Postprocessing would clip PCM32; reduce input/reference gain");
                double rounded = std::round(std::max(-1.0, value) * 2147483648.0);
                pcm[2 * i + c] = int32_t(std::clamp(rounded, -2147483648.0, 2147483647.0));
                result.peak = std::max(result.peak, std::abs(value));
                squares += value * value;
            }
        }
        if (count)
            sink(pcm.data(), count);
        result.frames += count;
        result.rms = result.frames ? std::sqrt(double(squares / (2.0L * result.frames))) : 0;
    }
};
} // namespace

bool PostprocessOptions::mixes() const {
    return mode == PostprocessMode::Mix || mode == PostprocessMode::Combined;
}
bool PostprocessOptions::adapts() const {
    return (mode == PostprocessMode::Adaptive || mode == PostprocessMode::Combined) &&
           envelope_strength > 0 && max_cut_db > 0;
}
bool PostprocessOptions::active() const {
    return mixes() || adapts();
}
void PostprocessOptions::validate(unsigned output_rate, unsigned reference_rate) const {
    require(mode == PostprocessMode::None || mode == PostprocessMode::Adaptive ||
                mode == PostprocessMode::Mix || mode == PostprocessMode::Combined,
            "Invalid postprocess mode");
    require(curve == CrossoverCurve::Linear || curve == CrossoverCurve::Cosine,
            "Invalid crossover curve");
    for (double value :
         {mix, crossover_low, crossover_high, envelope_strength, max_cut_db, fit_low, fit_high,
          envelope_low, envelope_high, smoothing_ms, reference_gain_db, alignment_start})
        require(std::isfinite(value), "Postprocessing parameters must be finite");
    require(output_rate == 48000 || output_rate == 96000 || output_rate == 192000,
            "Processed audio must be 48, 96 or 192 kHz");
    require(mix >= 0 && mix <= 1 && envelope_strength >= 0 && envelope_strength <= 1,
            "Mix and envelope strength must be between 0 and 1");
    require(max_cut_db >= 0 && max_cut_db <= 60, "Maximum attenuation must be 0..60 dB");
    require(smoothing_ms >= 0 && smoothing_ms <= 5000, "Smoothing must be 0..5000 ms");
    require(reference_gain_db >= -120 && reference_gain_db <= 24,
            "Reference gain must be -120..24 dB");
    require(alignment_start >= 0 && alignment_start <= 86400,
            "Alignment start must be 0..86400 seconds");
    require(crossover_low > 0 && crossover_low < crossover_high && envelope_low > 0 &&
                envelope_low < envelope_high && fit_low > 0 && fit_low < fit_high &&
                fit_high - fit_low >= 1500,
            "Frequency intervals must increase; envelope fit must span at least 1500 Hz");
    if (mixes())
        require(crossover_high < output_rate / 2.0, "Crossover must end below output Nyquist");
    if (adapts())
        require(std::max(fit_high, envelope_high) < output_rate / 2.0,
                "Envelope frequencies must be below output Nyquist");
    if (reference_delay)
        require(*reference_delay >= -10LL * output_rate && *reference_delay <= 10LL * output_rate,
                "Explicit reference delay must be within +/-10 seconds");
    if (active() && reference_rate) {
        require(supported_reference(reference_rate), "Unsupported reference sample rate");
        if (adapts())
            require(fit_high < reference_rate / 2.0,
                    "Fit interval must be below reference Nyquist");
    }
}

PostprocessResult postprocess(AudioSource &processed, AudioSource *reference,
                              const PostprocessOptions &options, const PcmSink &sink) {
    unsigned rate = processed.sample_rate();
    options.validate(rate, reference ? reference->sample_rate() : 0);
    require(bool(sink), "Missing PCM output sink");
    require(!options.active() || reference, "Active postprocessing requires reference audio");
    require(!options.active() || supported_reference(reference->sample_rate()),
            "Unsupported reference sample rate");
    // Keeps signed timeline, rational resampling and frame arithmetic representable.
    constexpr uint64_t limit = uint64_t(INT64_MAX) / 384000;
    require(processed.frame_count() < limit && (!reference || reference->frame_count() < limit),
            "Audio is too long for postprocessing");
    Output output(sink);
    uint64_t length = processed.frame_count();
    if (!length)
        return output.result;
    if (!options.active()) {
        std::vector<StereoFrame> block(32768);
        for (uint64_t first = 0; first < length; first += block.size()) {
            size_t count = size_t(std::min<uint64_t>(block.size(), length - first));
            read_exact(processed, first, block.data(), count);
            output.write(block.data(), count);
        }
        return output.result;
    }
    Reference aligned(*reference, rate, options.reference_gain_db);
    int64_t delay;
    if (options.reference_delay)
        delay = *options.reference_delay;
    else {
        auto estimate = estimate_delay(processed, aligned, options.alignment_start);
        delay = estimate.first;
        output.result.alignment_correlation = estimate.second;
    }
    output.result.reference_delay = delay;
    aligned.align(delay, length);
    size_t n = 2048 * (rate / 48000), hop = n / 4;
    Fft fft(n);
    std::vector<double> window(n);
    for (size_t i = 0; i < n; ++i)
        window[i] = (1 - std::cos(2 * pi * i / n)) * 0.5;
    std::optional<Envelope> envelope;
    if (options.adapts())
        envelope.emplace(processed, aligned, options, window, fft);
    WindowStream input(processed, n), original(aligned, n);
    Spectrum a(n), b(n);
    std::vector<StereoFrame> accumulation(n), block(hop);
    std::vector<double> weight(n), mixed(n / 2 + 1), taper(n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k) {
        double hz = double(k) * rate / n;
        mixed[k] = options.mixes() ? options.mix * transition(hz, options.crossover_low,
                                                              options.crossover_high, options.curve)
                                   : 1;
        taper[k] =
            transition(hz, options.envelope_low, options.envelope_high, CrossoverCurve::Cosine);
    }
    int64_t first = -int64_t(n / 2);
    auto emit = [&](size_t count) {
        size_t made = 0;
        for (size_t i = 0; i < count; ++i) {
            int64_t position = first + int64_t(i);
            if (position >= 0 && uint64_t(position) < length) {
                require(weight[i] > 1e-15, "Invalid overlap-add normalization");
                block[made++] = {accumulation[i].left / weight[i],
                                 accumulation[i].right / weight[i]};
            }
        }
        output.write(block.data(), made);
        std::move(accumulation.begin() + count, accumulation.end(), accumulation.begin());
        std::fill(accumulation.end() - count, accumulation.end(), StereoFrame{});
        std::move(weight.begin() + count, weight.end(), weight.begin());
        std::fill(weight.end() - count, weight.end(), 0);
        first += int64_t(count);
    };
    size_t rows = size_t(length / hop + 1);
    for (size_t t = 0; t < rows; ++t) {
        a.analyze(input, window, fft);
        if (options.mixes())
            b.analyze(original, window, fft);
        for (size_t k = 0; k <= n / 2; ++k) {
            double gain =
                envelope ? std::pow(10.0, envelope->gain(t, double(k) * rate / n) * taper[k] / 20)
                         : 1;
            a.left[k] = mixed[k] * gain * a.left[k] + (1 - mixed[k]) * b.left[k];
            a.right[k] = mixed[k] * gain * a.right[k] + (1 - mixed[k]) * b.right[k];
            if (k && k != n / 2) {
                a.left[n - k] = std::conj(a.left[k]);
                a.right[n - k] = std::conj(a.right[k]);
            }
        }
        fft.run(a.left, true);
        fft.run(a.right, true);
        for (size_t i = 0; i < n; ++i) {
            accumulation[i].left += a.left[i].real() * window[i];
            accumulation[i].right += a.right[i].real() * window[i];
            weight[i] += window[i] * window[i];
        }
        emit(hop);
        if (t + 1 < rows) {
            input.next();
            original.next();
        }
    }
    for (size_t rest = n - hop; rest; rest -= hop)
        emit(hop);
    require(output.result.frames == length, "Postprocessing output length mismatch");
    return output.result;
}
} // namespace dsee
