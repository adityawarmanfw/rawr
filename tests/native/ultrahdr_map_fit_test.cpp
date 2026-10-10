// UltraHDR writer remap: luma baseline floor. The floor must rise with base
// luma, never lower a measured code, and baseShare 0 must reproduce the
// floor-less remap byte for byte.
#include <cstdio>
#include <vector>

#include "encoding/jpeg/UltraHdrMapFit.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
}  // namespace

int main() {
    using rawrcam::color::UltraHdrParams;
    using rawrcam::encoding::jpeg::fitAndRemapMap;
    // 256x16 gray ramp base; map is identity (code 0) except a bright
    // highlight column so the fit has a ceiling to find.
    constexpr std::uint32_t W = 256, H = 16;
    std::vector<unsigned char> base(W * H * 4), map(W * H * 4, 0);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            unsigned char* b = &base[(y * W + x) * 4];
            b[0] = b[1] = b[2] = static_cast<unsigned char>(x);
            b[3] = 255;
            map[(y * W + x) * 4 + 3] = 255;
        }
    for (std::uint32_t y = 0; y < H; ++y)
        for (int k = 0; k < 3; ++k) map[(y * W + (W - 1)) * 4 + k] = 200;

    UltraHdrParams uh;
    std::vector<unsigned char> off, on;
    float maxOff = 0.0f, maxOn = 0.0f;
    UltraHdrParams none = uh;
    none.baseShare = 0.0f;
    check(fitAndRemapMap(map.data(), W, H, none, off, maxOff, base.data(), W, H), "fit without floor");
    std::vector<unsigned char> noBase;
    float maxNoBase = 0.0f;
    check(fitAndRemapMap(map.data(), W, H, uh, noBase, maxNoBase), "fit without base");
    check(off == noBase && maxOff == maxNoBase, "baseShare 0 == no base pointer");
    check(fitAndRemapMap(map.data(), W, H, uh, on, maxOn, base.data(), W, H), "fit with floor");
    check(maxOn == maxOff, "floor leaves fitted max unchanged");

    check(on[0] == 0, "black base keeps identity");
    int prev = -1;
    bool monotonic = true, neverLower = true;
    for (std::uint32_t x = 0; x < W; ++x) {
        const int code = on[x * 4 + 1];
        if (code < prev) monotonic = false;
        if (code < off[x * 4 + 1]) neverLower = false;
        prev = code;
    }
    check(monotonic, "floor rises with base luma");
    check(neverLower, "floor never lowers a code");
    check(on[128 * 4 + 1] > 64, "mid-gray gets a real share of headroom");

    // Half-res map samples the base at the scaled pixel.
    constexpr std::uint32_t HW = W / 2, HH = H / 2;
    std::vector<unsigned char> half(HW * HH * 4, 0), halfOut;
    for (std::uint32_t y = 0; y < HH; ++y)
        for (int k = 0; k < 3; ++k) half[(y * HW + (HW - 1)) * 4 + k] = 200;
    float maxHalf = 0.0f;
    check(fitAndRemapMap(half.data(), W / 2, H / 2, uh, halfOut, maxHalf, base.data(), W, H), "half-res fit");
    check(maxHalf == maxOn, "half-res fit matches full-res ceiling");
    check(halfOut[64 * 4 + 1] == on[128 * 4 + 1], "half-res maps to scaled base pixel");

    if (failures == 0) std::printf("ultrahdr_map_fit_test ok\n");
    return failures == 0 ? 0 : 1;
}
