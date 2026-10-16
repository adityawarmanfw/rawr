#include <cstdio>
#include <cstdlib>
#include <vector>

#include "convert/NaturalBin.h"

using rawrcam::convert::BinCalibration;
using rawrcam::convert::BinQuantize;
using rawrcam::convert::NaturalBin2x;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                            \
        }                                                        \
    } while (0)

static std::vector<uint16_t> run(NaturalBin2x& bin, const std::vector<uint16_t>& in, uint32_t w) {
    std::vector<uint16_t> out(size_t(bin.outputWidth()) * bin.outputHeight());
    for (uint32_t y = 0; y < bin.outputHeight(); y += NaturalBin2x::kBandRows) {
        const uint32_t rows = std::min(NaturalBin2x::kBandRows, bin.outputHeight() - y);
        uint32_t top, bottom;
        bin.sourceRows(y, rows, &top, &bottom);
        bin.band(y, rows,
                 [&](uint32_t x, uint32_t sy) {
                     if (sy < top || sy >= bottom) std::abort();  // band contract
                     return in[size_t(sy) * w + x];
                 },
                 [] {}, out.data() + size_t(y) * bin.outputWidth(), bin.outputWidth());
    }
    return out;
}

int main() {
    constexpr uint32_t w = 96, h = 80;
    BinCalibration cal;
    cal.black = {64, 64, 64, 64};
    cal.wbGains = {2.f, 1.f, 1.f, 1.6f};
    cal.white = 1023;
    cal.cfa = {0, 1, 1, 2};
    std::string error;

    // A flat colored field stays flat per CFA site; 4x gain scales codes.
    const uint16_t site[4] = {300, 500, 500, 400};
    std::vector<uint16_t> flat(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) flat[size_t(y) * w + x] = site[(y & 1) * 2 + (x & 1)];
    for (const auto q : {BinQuantize{1, false}, BinQuantize{4, false}}) {
        NaturalBin2x bin;
        CHECK(bin.configure(cal, q, w, h, &error));
        const auto out = run(bin, flat, w);
        for (uint32_t y = 0; y < h / 2; ++y)
            for (uint32_t x = 0; x < w / 2; ++x)
                CHECK(out[size_t(y) * (w / 2) + x] == site[(y & 1) * 2 + (x & 1)] * q.gain);
    }

    // Saturation is preserved exactly; dither never pushes codes past white.
    std::vector<uint16_t> clipped(size_t(w) * h, 1023);
    {
        NaturalBin2x bin;
        CHECK(bin.configure(cal, BinQuantize{1, true}, w, h, &error));
        for (const auto v : run(bin, clipped, w)) CHECK(v == 1023);
    }

    // Output stays inside the same-site neighbourhood range (no overshoot halos).
    std::vector<uint16_t> edge(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) edge[size_t(y) * w + x] = x < w / 2 ? 80 : 1000;
    {
        NaturalBin2x bin;
        CHECK(bin.configure(cal, BinQuantize{1, false}, w, h, &error));
        for (const auto v : run(bin, edge, w)) CHECK(v >= 80 && v <= 1000);
    }

    CHECK(BinQuantize::forWhiteLevel(1023).gain == 4 && !BinQuantize::forWhiteLevel(1023).dither);
    CHECK(BinQuantize::forWhiteLevel(16383).gain == 1 && BinQuantize::forWhiteLevel(16383).dither);

    // Rejected inputs.
    NaturalBin2x bin;
    CHECK(!bin.configure(cal, BinQuantize{1, false}, 98, 80, &error));
    auto quad = cal;
    quad.cfa = {0, 0, 1, 1};
    CHECK(!bin.configure(quad, BinQuantize{1, false}, w, h, &error));
    auto deep = cal;
    deep.white = 65535;
    CHECK(!bin.configure(deep, BinQuantize{4, false}, w, h, &error));
    std::puts("convert_natural_bin_test ok");
    return 0;
}
