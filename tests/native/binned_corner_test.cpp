// A full-size still whose HAL skipped the sensor switch holds the binned
// readout in its top-left corner and zeros elsewhere; it must be told apart
// from a full frame (including a very dark one) and cropped to the corner.
#include <cstdio>
#include <cstring>
#include <vector>

#include "imaging/BinnedCorner.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
std::vector<uint16_t> frame(uint32_t w, uint32_t h, uint32_t cornerW, uint32_t cornerH, uint16_t base) {
    std::vector<uint16_t> px(size_t(w) * h, 0);
    for (uint32_t y = 0; y < cornerH; ++y)
        for (uint32_t x = 0; x < cornerW; ++x) px[size_t(y) * w + x] = uint16_t(base + (x * 7 + y * 3) % 50);
    return px;
}
}  // namespace

int main() {
    using rawrcam::imaging::binnedCornerDivisor;
    using rawrcam::imaging::cropPackedRaw16ToCorner;
    constexpr uint32_t W = 1632, H = 1224;

    const auto full = frame(W, H, W, H, 64);
    check(binnedCornerDivisor(full.data(), W, H) == 1, "full frame");

    // Dark frame: values just above a 64 black level, never zero.
    const auto dark = frame(W, H, W, H, 60);
    check(binnedCornerDivisor(dark.data(), W, H) == 1, "dark full frame");

    const auto quarter = frame(W, H, W / 4, H / 4, 64);
    check(binnedCornerDivisor(quarter.data(), W, H) == 4, "binned quarter in 200 MP buffer");

    const auto half = frame(W, H, W / 2, H / 2, 64);
    check(binnedCornerDivisor(half.data(), W, H) == 2, "binned half in 50 MP buffer");

    const std::vector<uint16_t> empty(size_t(W) * H, 0);
    check(binnedCornerDivisor(empty.data(), W, H) == 1, "all-zero frame is not a corner");

    // A non-zero sample outside the quarter: never cropped to the quarter.
    auto stray = quarter;
    stray[size_t(H / 16) * W + W * 5 / 16] = 70;
    check(binnedCornerDivisor(stray.data(), W, H) == 2, "quarter test needs every outside sample empty");
    stray[size_t(H * 15 / 16) * W + W * 15 / 16] = 70;
    check(binnedCornerDivisor(stray.data(), W, H) == 1, "data outside the half is a real frame");

    // The HAL's report decides first; zero samples are the fallback.
    using rawrcam::imaging::fullSizeBinnedCorner;
    const auto bytes = [](const std::vector<uint16_t>& px) {
        std::vector<uint8_t> out(px.size() * 2);
        std::memcpy(out.data(), px.data(), out.size());
        return out;
    };
    const auto fullBytes = bytes(full), quarterBytes = bytes(quarter);
    check(fullSizeBinnedCorner(fullBytes, W, H, W / 4, H / 4, 768).divisor == 1, "200 MP switch reported, full data");
    check(fullSizeBinnedCorner(fullBytes, W, H, W / 2, H / 2, 259).divisor == 1, "50 MP switch reported, full data");
    // A reused buffer: stale full-size data around a binned corner, but the HAL says it did not switch.
    const auto stale = fullSizeBinnedCorner(fullBytes, W, H, W / 4, H / 4, 1);
    check(stale.divisor == 4 && std::strcmp(stale.reason, "reported") == 0, "no switch reported, stale buffer");
    check(fullSizeBinnedCorner(fullBytes, W, H, W / 2, H / 2, 768).divisor == 2, "200 MP value on a 50 MP buffer");
    const auto empty4 = fullSizeBinnedCorner(quarterBytes, W, H, W / 4, H / 4, std::nullopt);
    check(empty4.divisor == 4 && std::strcmp(empty4.reason, "empty") == 0, "no tag: zero samples decide");
    check(fullSizeBinnedCorner(quarterBytes, W, H, W / 4, H / 4, 768).divisor == 4, "switch claimed, corner only");
    check(fullSizeBinnedCorner(quarterBytes, W, H, W / 3, H / 3, 1).divisor == 1, "not a 2x/4x buffer: never checked");
    check(fullSizeBinnedCorner(quarterBytes, W / 4, H / 4, W / 4, H / 4, 1).divisor == 1, "binned stream: never checked");

    std::vector<uint8_t> packed(quarter.size() * 2);
    std::memcpy(packed.data(), quarter.data(), packed.size());
    cropPackedRaw16ToCorner(packed, W, H, 4);
    check(packed.size() == size_t(W / 4) * (H / 4) * 2, "crop size");
    const auto* out = reinterpret_cast<const uint16_t*>(packed.data());
    bool same = true;
    for (uint32_t y = 0; y < H / 4 && same; ++y)
        for (uint32_t x = 0; x < W / 4 && same; ++x) same = out[size_t(y) * (W / 4) + x] == quarter[size_t(y) * W + x];
    check(same, "crop keeps corner pixels in order");

    if (failures == 0) std::printf("binned_corner_test: ok\n");
    return failures == 0 ? 0 : 1;
}
