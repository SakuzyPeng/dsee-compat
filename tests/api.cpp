#include "dsee_compat.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
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
long rss() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_maxrss;
}
int main(int argc, char **argv) {
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
        if (argc == 3 && std::string(argv[2]) == "benchmark") {
            std::array<int16_t, 2048> input{};
            unsigned rng = 12345;
            for (auto &s : input) {
                rng = rng * 1664525 + 1013904223;
                s = int16_t(int(rng >> 16) / 8 - 4096);
            }
            for (int p = 0; p < 3; ++p)
                for (unsigned rate : {48000U, 96000U, 192000U}) {
                    c = create(root, rate, dsee_profile(p));
                    for (int i = 0; i < 64; ++i)
                        ok(dsee_process(c, input.data(), 1024, dummy, 4096, &a, &b));
                    auto start = std::chrono::steady_clock::now();
                    for (int i = 0; i < 1440; ++i) {
                        ok(dsee_process(c, input.data(), 1024, dummy, 4096, &a, &b));
                        expect(a == 1024 && b == 1024 * rate / 48000);
                    }
                    double seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                            .count();
                    ok(dsee_destroy(c));
                    std::cout << "{\"profile\":" << p << ",\"rate\":" << rate
                              << ",\"audio_seconds\":30.72,\"seconds\":" << seconds
                              << ",\"realtime_factor\":" << 30.72 / seconds << "}\n";
                }
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
        long before = rss();
        for (int i = 0; i < 200; ++i) {
            c = create(root);
            ok(dsee_destroy(c));
        }
        long growth = rss() - before;
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
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
