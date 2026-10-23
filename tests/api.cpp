#include "dsee_compat.h"
#include "fixtures.hpp"
#include "platform.hpp"
#include "resources.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>

#include <psapi.h>
#else
#include <sys/resource.h>
#endif
#include <thread>
#include <vector>
void expect(bool b) {
    if (!b)
        throw std::runtime_error(std::string("API test: ") + dsee_last_error());
}
void ok(dsee_status s) {
    expect(s == DSEE_OK);
}
dsee_context *create(const char *root, unsigned rate = 96000, dsee_profile p = DSEE_AAC128) {
    dsee_context *c = nullptr;
    ok(dsee_create(root, rate, p, &c));
    expect(c);
    return c;
}
std::vector<int32_t> run(const char *root, const std::vector<int16_t> &input, bool fragmented,
                         bool finish, unsigned rate = 96000, dsee_profile p = DSEE_AAC128) {
    auto *c = create(root, rate, p);
    std::vector<int32_t> result;
    std::array<int32_t, 8192> out{};
    size_t pos = 0;
    while (pos < input.size() / 2) {
        size_t n = std::min(input.size() / 2 - pos, fragmented ? (pos % 1031 + 1) : size_t(1024));
        size_t used = 0, made = 0;
        ok(dsee_process(c, input.data() + 2 * pos, n, out.data(), 4096, &used, &made));
        expect(used || made);
        pos += used;
        result.insert(result.end(), out.begin(), out.begin() + 2 * made);
    }
    if (finish) {
        int done = 0;
        int calls = 0;
        while (!done) {
            size_t made = 0, padding = 0;
            ok(dsee_finish(c, out.data(), 4096, &made, &padding, &done));
            expect(++calls <= 50);
            result.insert(result.end(), out.begin(), out.begin() + 2 * made);
        }
        size_t used, made;
        expect(dsee_process(c, input.data(), 0, out.data(), 4096, &used, &made) ==
               DSEE_STATE_ERROR);
    }
    ok(dsee_destroy(c));
    return result;
}
int64_t rss() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                              sizeof(counters)))
        throw std::runtime_error("Cannot query private process memory");
    return int64_t(counters.PrivateUsage);
#else
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
#if defined(__APPLE__)
    return r.ru_maxrss;
#else
    return r.ru_maxrss * 1024;
#endif
#endif
}
int api_main(int argc, char **argv) {
    try {
        expect(dsee_inspect(nullptr) == DSEE_INVALID_ARGUMENT);
        dsee_context *c = nullptr;
        expect(dsee_create("missing", 44100, DSEE_PCM, &c) == DSEE_INVALID_ARGUMENT);
        expect(!c);
        expect(dsee_inspect("/nonexistent/dsee-compat-assets") == DSEE_ASSET_ERROR);
        ok(dsee_destroy(nullptr));
        size_t a, b;
        int32_t dummy[8192];
        expect(dsee_process(nullptr, nullptr, 1, dummy, 4096, &a, &b) == DSEE_INVALID_ARGUMENT);
        if (argc == 1) {
            std::cout << "No-assets API tests passed\n";
            return 0;
        }
        const char *root = argv[1];
        // Emulator/instrumentation RSS includes translation caches, not just
        // allocations owned by the library. Still run every lifecycle check.
        bool instrumented = argc == 3 && std::string(argv[2]) == "instrumented";
        if (argc >= 3 && std::string(argv[2]) == "verify") {
            expect(argc == 3);
            const char *profiles[] = {"pcm", "aac128", "mp3128"};
            for (int p = 0; p < 3; ++p)
                for (unsigned rate : {48000U, 96000U, 192000U})
                    for (const char *kind : {"silence", "impulse", "tone", "noise"}) {
                        auto input = dsee::test::fixture(kind);
                        auto pcm = run(root, input, false, true, rate, dsee_profile(p));
                        auto fragmented = run(root, input, true, true, rate, dsee_profile(p));
                        expect(pcm == fragmented);
                        size_t prefix_samples = input.size() * (rate / 48000);
                        expect(pcm.size() >= prefix_samples);
                        size_t tail_blocks =
                            (pcm.size() - prefix_samples) / (2048 * (rate / 48000));
                        std::cout << "{\"event\":\"case\",\"stimulus\":\"" << kind
                                  << "\",\"profile\":\"" << profiles[p] << "\",\"rate\":" << rate
                                  << ",\"input_sha256\":\""
                                  << dsee::sha256(input.data(), input.size() * 2)
                                  << "\",\"sha256\":\""
                                  << dsee::sha256(pcm.data(), prefix_samples * 4)
                                  << "\",\"full_sha256\":\""
                                  << dsee::sha256(pcm.data(), pcm.size() * 4)
                                  << "\",\"output_frames\":" << pcm.size() / 2
                                  << ",\"tail_blocks\":" << tail_blocks << "}" << std::endl;
                    }
            return 0;
        }
        if (argc >= 3 && std::string(argv[2]) == "benchmark") {
            expect(argc <= 5);
            auto number = [](const char *text, size_t limit) {
                size_t end = 0;
                auto n = std::stoull(text, &end);
                if (end != std::strlen(text) || !n || n > limit)
                    throw std::runtime_error("Invalid benchmark count");
                return size_t(n);
            };
            size_t blocks = argc >= 4 ? number(argv[3], 1000000) : 1440;
            size_t repeats = argc == 5 ? number(argv[4], 100) : 1;
            double audio_seconds = double(blocks) * 1024 / 48000;
            std::array<int16_t, 2048> input{};
            unsigned rng = 12345;
            for (auto &sample : input) {
                rng = rng * 1664525 + 1013904223;
                sample = int16_t(int(rng >> 16) / 8 - 4096);
            }
            using Clock = std::chrono::steady_clock;
            auto ms = [](auto duration) {
                return std::chrono::duration<double, std::milli>(duration).count();
            };
            std::cout << std::setprecision(12);
            for (size_t repeat = 1; repeat <= repeats; ++repeat)
                for (int p = 0; p < 3; ++p)
                    for (unsigned rate : {48000U, 96000U, 192000U}) {
                        auto start = Clock::now();
                        c = create(root, rate, dsee_profile(p));
                        double init_ms = ms(Clock::now() - start);
                        start = Clock::now();
                        ok(dsee_process(c, input.data(), 1024, dummy, 4096, &a, &b));
                        double first_ms = ms(Clock::now() - start);
                        for (int i = 1; i < 64; ++i)
                            ok(dsee_process(c, input.data(), 1024, dummy, 4096, &a, &b));
                        std::vector<double> latency;
                        latency.reserve(blocks);
                        start = Clock::now();
                        for (size_t i = 0; i < blocks; ++i) {
                            auto block_start = Clock::now();
                            ok(dsee_process(c, input.data(), 1024, dummy, 4096, &a, &b));
                            latency.push_back(ms(Clock::now() - block_start));
                            expect(a == 1024 && b == 1024 * rate / 48000);
                        }
                        double seconds =
                            std::chrono::duration<double>(Clock::now() - start).count();
                        ok(dsee_destroy(c));
                        std::sort(latency.begin(), latency.end());
                        auto quantile = [&](double q) {
                            return latency[size_t(std::ceil(q * double(blocks))) - 1];
                        };
                        std::cout << "{\"repeat\":" << repeat << ",\"profile\":" << p
                                  << ",\"rate\":" << rate
                                  << ",\"warmup_blocks\":64,\"measured_blocks\":" << blocks
                                  << ",\"audio_seconds\":" << audio_seconds
                                  << ",\"seconds\":" << seconds
                                  << ",\"realtime_factor\":" << audio_seconds / seconds
                                  << ",\"initialization_ms\":" << init_ms
                                  << ",\"first_block_ms\":" << first_ms
                                  << ",\"p50_ms\":" << quantile(0.5)
                                  << ",\"p95_ms\":" << quantile(0.95)
                                  << ",\"p99_ms\":" << quantile(0.99)
                                  << ",\"max_ms\":" << latency.back() << "}" << std::endl;
                    }
            return 0;
        }
        if (argc >= 3 && std::string(argv[2]) == "long-stream") {
            expect(argc == 3 || argc == 4);
            size_t end = 0;
            auto seconds = argc == 4 ? std::stoull(argv[3], &end) : 1800;
            expect(seconds && seconds <= 86400 && (argc != 4 || end == std::strlen(argv[3])));
            const uint64_t blocks = (seconds * 48000 + 1023) / 1024;
            c = create(root, 192000, DSEE_MP3128);
            std::array<int16_t, 2048> input{};
            uint32_t rng = 12345;
            auto fill = [&] {
                for (auto &sample : input) {
                    rng = rng * 1664525 + 1013904223;
                    sample = int16_t(int(rng >> 16) / 8 - 4096);
                }
            };
            for (int i = 0; i < 64; ++i) {
                fill();
                ok(dsee_process(c, input.data(), 1024, dummy, 4096, &a, &b));
            }
            int64_t before = rss(), peak = before;
            uint64_t checksum = 14695981039346656037ULL, nonzero_blocks = 0;
            auto start = std::chrono::steady_clock::now();
            for (uint64_t i = 0; i < blocks; ++i) {
                fill();
                ok(dsee_process(c, input.data(), 1024, dummy, 4096, &a, &b));
                expect(a == 1024 && b == 4096);
                nonzero_blocks += std::any_of(std::begin(dummy), std::end(dummy),
                                              [](int32_t x) { return x != 0; });
                for (auto sample : dummy)
                    checksum = (checksum ^ uint32_t(sample)) * 1099511628211ULL;
                if ((i + 1) % 4096 == 0) {
                    peak = std::max(peak, rss());
                    std::cout << "{\"event\":\"progress\",\"blocks\":" << i + 1
                              << ",\"memory_growth_bytes\":" << peak - before << "}" << std::endl;
                }
            }
            peak = std::max(peak, rss());
            double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            expect(nonzero_blocks == blocks && peak - before < 16 * 1024 * 1024);
            int done = 0;
            unsigned tail = 0;
            while (!done) {
                size_t made = 0, padding = 0;
                ok(dsee_finish(c, dummy, 4096, &made, &padding, &done));
                expect(made == 4096 && ++tail <= 48);
            }
            ok(dsee_destroy(c));
            std::cout << "{\"event\":\"complete\",\"profile\":2,\"rate\":192000,\"blocks\":"
                      << blocks << ",\"audio_seconds\":" << double(blocks) * 1024 / 48000
                      << ",\"seconds\":" << elapsed << ",\"memory_growth_bytes\":" << peak - before
                      << ",\"checksum\":" << checksum << ",\"nonzero_blocks\":" << nonzero_blocks
                      << ",\"tail_blocks\":" << tail << "}" << std::endl;
            return 0;
        }
        c = create(root);
        dsee_context *other = nullptr;
        expect(dsee_create(root, 96000, DSEE_PCM, &other) == DSEE_BUSY);
        expect(!other);
        ok(dsee_destroy(c));
        expect(run(root, {}, false, true).empty());
        for (int p = 0; p < 3; ++p)
            for (unsigned rate : {48000U, 96000U, 192000U}) {
                std::vector<int16_t> input(2 * 4097);
                input[input.size() - 2] = 16384;
                input.back() = -8192;
                auto full = run(root, input, false, true, rate, dsee_profile(p));
                auto chunks = run(root, input, true, true, rate, dsee_profile(p));
                expect(full == chunks);
                input.resize(2 * (5120 + 64 * 1024), 0);
                auto long_tail = run(root, input, false, false, rate, dsee_profile(p));
                expect(full.size() <= long_tail.size() &&
                       std::equal(full.begin(), full.end(), long_tail.begin()));
                expect(std::all_of(long_tail.begin() + full.size(), long_tail.end(),
                                   [](auto x) { return x == 0; }));
                expect(std::any_of(full.begin(), full.end(), [](auto x) { return x != 0; }));
            }
        c = create(root);
        std::atomic<int> ready{0}, busy{0}, bad{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&] {
                std::array<int16_t, 2048> input{};
                std::array<int32_t, 8192> out{};
                ++ready;
                while (ready < 4)
                    std::this_thread::yield();
                for (int i = 0; i < 100; ++i) {
                    size_t used, made;
                    auto s = dsee_process(c, input.data(), 1024, out.data(), 4096, &used, &made);
                    if (s == DSEE_BUSY)
                        ++busy;
                    else if (s != DSEE_OK)
                        ++bad;
                }
            });
        for (auto &t : threads)
            t.join();
        expect(busy > 0 && bad == 0);
        ok(dsee_destroy(c));
        int64_t before = rss();
        for (int i = 0; i < 200; ++i) {
            c = create(root);
            ok(dsee_destroy(c));
        }
        int64_t growth = rss() - before;
        if (!instrumented)
            expect(growth < 16 * 1024 * 1024);
        c = create(root);
        std::array<int16_t, 2048> input{};
        for (int i = 0; i < 5000; ++i) {
            ok(dsee_process(c, input.data(), 1024, dummy, 4096, &a, &b));
            expect(a == 1024);
        }
        ok(dsee_destroy(c));
        std::cout << "API fragmentation, tail reference (9 modes), concurrent rejection, 200 "
                     "lifecycles and long stream passed; RSS growth="
                  << growth << " bytes\n";
        if (instrumented)
            std::cout << "RSS limit skipped for instrumentation; inspect its memory report\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    try {
        auto args = dsee::utf8_arguments(argc, argv);
        std::vector<char *> pointers;
        for (auto &arg : args)
            pointers.push_back(arg.data());
        return api_main(argc, pointers.data());
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char **argv) {
    return api_main(argc, argv);
}
#endif
