// A test-only backend exercises the C boundary without any original resources.
// This executable does not link the production resource loader or backend.
#include "backend.hpp"
#include "dsee_compat.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
int live = 0, closes = 0, block_number = 0, fail_block = 0;
bool fail_close = false, fail_create = false, noisy = false, unknown_exception = false;
void expect(bool value) {
    if (!value)
        throw std::runtime_error("Runtime state test failed");
}
class Fake final : public dsee::Backend {
    unsigned ratio;

  public:
    explicit Fake(unsigned rate) : ratio(rate / 48000) {
        ++live;
    }
    ~Fake() override {
        --live;
    }
    void process_block(const int16_t *, int32_t *out) override {
        if (++block_number == fail_block) {
            if (unknown_exception)
                throw 42;
            throw std::runtime_error("Injected block failure");
        }
        std::fill_n(out, 2048 * ratio, noisy ? 1 : 0);
    }
    void close() override {
        ++closes;
        if (fail_close)
            throw std::runtime_error("Injected close failure");
    }
};
dsee_context *create() {
    block_number = closes = 0;
    dsee_context *c = nullptr;
    expect(dsee_create("test-only", 48000, DSEE_PCM, &c) == DSEE_OK && c && live == 1);
    return c;
}
} // namespace
namespace dsee {
Resources load_resources(const char *, int) {
    return {};
}
std::unique_ptr<Backend> make_backend(Resources, unsigned rate, unsigned) {
    if (fail_create)
        throw ProcessError("Injected constructor failure");
    return std::make_unique<Fake>(rate);
}
} // namespace dsee
int main() {
    try {
        std::array<int16_t, 6144> in{};
        std::array<int32_t, 6144> out{};
        size_t used = 0, made = 0, padding = 0;
        int done = 0;
        fail_create = true;
        dsee_context *c = nullptr;
        expect(dsee_create("test-only", 48000, DSEE_PCM, &c) == DSEE_PROCESS_ERROR && !c && !live);
        fail_create = false;
        for (bool nonstandard : {false, true}) {
            unknown_exception = nonstandard;
            c = create();
            fail_block = 2;
            expect(dsee_process(c, in.data(), 3072, out.data(), 3072, &used, &made) ==
                   DSEE_PROCESS_ERROR);
            expect(used == 2048 && made == 1024 && std::strlen(dsee_last_error()));
            expect(dsee_process(c, in.data(), 1024, out.data(), 3072, &used, &made) ==
                   DSEE_STATE_ERROR);
            expect(!used && !made);
            expect(dsee_finish(c, out.data(), 3072, &made, &padding, &done) == DSEE_STATE_ERROR);
            expect(!made && !padding && !done);
            expect(dsee_destroy(c) == DSEE_OK && !live && !closes);
        }
        fail_block = 0;
        unknown_exception = false;
        c = create();
        fail_close = true;
        expect(dsee_destroy(c) == DSEE_PROCESS_ERROR && !live && closes == 1);
        fail_close = false;
        c = create();
        expect(dsee_finish(c, out.data(), 3072, &made, &padding, &done) == DSEE_OK && done &&
               !made);
        expect(dsee_destroy(c) == DSEE_OK && !live);
        c = create();
        expect(dsee_process(c, in.data(), 1, out.data(), 3072, &used, &made) == DSEE_OK &&
               used == 1 && !made);
        expect(dsee_finish(c, out.data(), 3072, &made, &padding, &done) == DSEE_OK &&
               made == 1024 && padding == 1023 && !done);
        for (int i = 0; i < 4; ++i) {
            expect(dsee_finish(c, out.data(), 3072, &made, &padding, &done) == DSEE_OK);
            expect(made == 1024 && padding == 1024 && bool(done) == (i == 3));
        }
        expect(dsee_finish(c, out.data(), 3072, &made, &padding, &done) == DSEE_OK && done &&
               !made && !padding);
        expect(dsee_destroy(c) == DSEE_OK && !live);
        c = create();
        noisy = true;
        expect(dsee_process(c, in.data(), 1024, out.data(), 3072, &used, &made) == DSEE_OK);
        for (int i = 0; i < 48; ++i)
            expect(dsee_finish(c, out.data(), 3072, &made, &padding, &done) ==
                   (i == 47 ? DSEE_TAIL_LIMIT : DSEE_OK));
        expect(dsee_destroy(c) == DSEE_OK && !live && !closes);
        std::cout << "C API failure, prefix counts, cleanup and drain state tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
