#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace dsee {

struct StereoFrame {
    double left = 0, right = 0;
};

// Offline, seekable audio. Implementations must remain unchanged for a complete
// postprocess call. Samples are finite doubles with normalized full scale
// [-1, 1). read_at may return a short read; zero means EOF. The source must keep
// its advertised rate and frame count stable. Exceptions propagate to the caller.
class AudioSource {
  public:
    virtual ~AudioSource() = default;
    virtual unsigned sample_rate() const = 0;
    virtual uint64_t frame_count() const = 0;
    virtual size_t read_at(uint64_t first, StereoFrame *output, size_t count) = 0;
};

enum class PostprocessMode { None, Adaptive, Mix, Combined };
enum class CrossoverCurve { Linear, Cosine };
enum class AutoMixObjective { Seam, Envelope };

struct PostprocessOptions {
    PostprocessMode mode = PostprocessMode::None;
    double mix = 0.5;
    bool auto_mix = false;
    AutoMixObjective auto_mix_objective = AutoMixObjective::Seam;
    double auto_mix_min = 0, auto_mix_max = 1, auto_mix_fallback = 0.5;
    double auto_mix_margin_db = 2, auto_mix_coverage = 0.9;
    CrossoverCurve curve = CrossoverCurve::Cosine;
    double crossover_low = 18000, crossover_high = 22000;
    double envelope_strength = 1, max_cut_db = 12;
    double fit_low = 15000, fit_high = 19500;
    double envelope_low = 18500, envelope_high = 22000;
    double smoothing_ms = 150;
    double reference_gain_db = 0;
    std::optional<int64_t> reference_delay; // Output-rate frames; unset = automatic.
    double alignment_start = 0;             // Seconds into both files.

    bool mixes() const;
    bool adapts() const;
    bool active() const;
    void validate(unsigned output_rate, unsigned reference_rate = 0) const;
};

struct AutoMixAnalysis {
    double selected_mix = 0.5;
    bool fallback = true;
    std::string reason = "insufficient_audio";
    size_t analyzed_windows = 0, active_windows = 0, reliable_windows = 0;
    double fit_low_hz = 0, fit_high_hz = 0;
    double slope_db_per_octave = 0, fit_error_db = 0;
    double achieved_coverage = 0;
    std::optional<double> envelope_mix, phase_limit;
    std::optional<double> seam_score_before, seam_score_after;
};

struct PostprocessResult {
    uint64_t frames = 0;
    int64_t reference_delay = 0;
    std::optional<double> alignment_correlation;
    double peak = 0, rms = 0; // Normalized sample peak and RMS; zero for silence.
    double effective_mix = 0.5;
    std::optional<AutoMixAnalysis> auto_mix;
};

using PcmSink = std::function<void(const int32_t *interleaved, size_t frames)>;

// Own, optional DSP: no proprietary resources, native runtime calls, codecs or
// external executables. PCM32 output has exactly processed.frame_count() frames.
// Throws std::exception on invalid inputs, failed I/O, uncertain auto alignment,
// or clipping. A sink may have received a prefix before a later failure; file
// callers should publish atomically. Calls on distinct sources are independent.
// Default options bypass DSP. Reference is needed only when active() is true;
// zero-strength Adaptive is also a bypass. PCM32-representable samples bypass
// bit-exactly. The sink's buffer is valid only during its callback.
PostprocessResult postprocess(AudioSource &processed, AudioSource *reference,
                              const PostprocessOptions &options, const PcmSink &sink);

} // namespace dsee
