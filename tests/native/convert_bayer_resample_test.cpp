#include <cstdio>
#include <vector>

#include "convert/BayerResample.h"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main() {
    using rawrcam::convert::BayerResample;
    constexpr uint32_t iw = 160, ih = 120, ow = 112, oh = 84;  // ~sqrt(2) per side
    BayerResample resample;
    CHECK(resample.configure(iw, ih, ow, oh, 4092));
    CHECK(!BayerResample().configure(iw, ih, 111, oh, 4092));  // odd output

    auto run = [&](const std::vector<uint16_t>& in) {
        std::vector<uint16_t> out(size_t(ow) * oh);
        for (uint32_t y = 0; y < oh; y += 16) {
            const uint32_t rows = std::min(16u, oh - y);
            uint32_t top, bottom;
            resample.sourceRows(y, rows, &top, &bottom);
            resample.band(y, rows,
                          [&](uint32_t x, uint32_t sy) {
                              if (sy < top || sy >= bottom || (x >= iw)) std::abort();
                              return double(in[size_t(sy) * iw + x]);
                          },
                          out.data() + size_t(y) * ow, ow);
        }
        return out;
    };

    // A flat coloured field keeps each CFA site's value (phase preserved).
    const uint16_t site[4] = {1200, 2400, 2300, 900};
    std::vector<uint16_t> flat(size_t(iw) * ih);
    for (uint32_t y = 0; y < ih; ++y)
        for (uint32_t x = 0; x < iw; ++x) flat[size_t(y) * iw + x] = site[(y & 1) * 2 + (x & 1)];
    auto out = run(flat);
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) CHECK(out[size_t(y) * ow + x] == site[(y & 1) * 2 + (x & 1)]);

    // A hard edge stays inside the same-site range (no ringing past it).
    std::vector<uint16_t> edge(size_t(iw) * ih);
    for (uint32_t y = 0; y < ih; ++y)
        for (uint32_t x = 0; x < iw; ++x) edge[size_t(y) * iw + x] = x < iw / 2 ? 300 : 3000;
    for (const auto v : run(edge)) CHECK(v >= 300 && v <= 3000);
    std::puts("convert_bayer_resample_test ok");
    return 0;
}
