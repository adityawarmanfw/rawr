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


    // A pale highlight must recover pink without changing its luminance;
    // shadows/mids keep the fitted brightness, including the scene lift.
    {
        using rawrcam::encoding::jpeg::restoreHighlightColor;
        std::vector<unsigned char> b = {0,0,0,255, 80,80,80,255, 250,250,250,255, 250,250,250,255};
        const auto savedBase = b;
        std::vector<unsigned char> m(16, 128), color(16, 0);
        for (int i = 0; i < 4; ++i) {
            const unsigned r = 43690, g = 13107; // chromaticity 2/3, .2, .1333
            color[i*4] = r & 255; color[i*4+1] = r >> 8;
            color[i*4+2] = g & 255; color[i*4+3] = g >> 8;
        }
        for (int k = 0; k < 4; ++k) color[12+k] = 255; // invalid tap must retain the old result
        const float cap = 1.45f;
        float lo = 0, hi = cap;
        check(restoreHighlightColor(m, color.data(), 4, 1, b.data(), 4, 1, uh, cap, lo, hi), "restore capture color");
        check(b == savedBase, "SDR base untouched");
        check(lo < 0 && hi > cap, "signed color range independent of display capacity");
        for (int i = 0; i < 4; ++i) {
            const float v = float(b[i*4]) / 255;
            const float lin = v <= .04045f ? v / 12.92f : std::pow((v+.055f)/1.055f, 2.4f);
            const float expected = (lin + uh.offsetSdr) * std::exp2(128.0f/255*cap) - uh.offsetHdr;
            float rgb[3];
            for (int k = 0; k < 3; ++k)
                rgb[k] = (lin + uh.offsetSdr) * std::exp2(lo + float(m[i*4+k])/255*(hi-lo)) - uh.offsetHdr;
            const float y = .2126f*rgb[0] + .7152f*rgb[1] + .0722f*rgb[2];
            check(std::abs(y-expected) < .012f*std::max(expected, .02f), "decoded luminance preserved within quantization");
            if (i == 2) check(std::abs(rgb[0]/rgb[1] - (10.0f/3)) < .04f, "highlight restores pink ratio");
            else check(m[i*4] == m[i*4+1] && m[i*4+1] == m[i*4+2], "shadow or invalid tap keeps original color");
        }
    }

    // Neutral, skin-like, and saturated highlight colors: restoring color
    // must conserve luminance even when one target component is zero.
    {
        using rawrcam::encoding::jpeg::restoreHighlightColor;
        const float colors[][3] = {{1,1,1}, {1,.65f,.45f}, {1,0,0}, {0,1,0},
                                   {0,0,1}, {1,1,0}, {0,1,1}, {1,0,1}};
        std::vector<unsigned char> b(32, 250), m(32, 160), c(32);
        for (unsigned i = 0; i < 8; ++i) {
            const float sum = colors[i][0] + colors[i][1] + colors[i][2];
            const unsigned r = unsigned(std::round(colors[i][0]/sum*65535));
            const unsigned g = unsigned(std::round(colors[i][1]/sum*65535));
            c[i*4] = r & 255; c[i*4+1] = r >> 8; c[i*4+2] = g & 255; c[i*4+3] = g >> 8;
        }
        float lo = 0, hi = 1.45f;
        check(restoreHighlightColor(m, c.data(), 8, 1, b.data(), 8, 1, uh, 1.45f, lo, hi), "color sweep restore");
        const float linear = std::pow((250.0f/255+.055f)/1.055f, 2.4f);
        const float expectedY = (linear+uh.offsetSdr)*std::exp2(160.0f/255*1.45f)-uh.offsetHdr;
        for (unsigned i = 0; i < 8; ++i) {
            float rgb[3];
            const float cy = .2126f*colors[i][0] + .7152f*colors[i][1] + .0722f*colors[i][2];
            for (int k = 0; k < 3; ++k) {
                rgb[k] = (linear+uh.offsetSdr)*std::exp2(lo+float(m[i*4+k])/255*(hi-lo))-uh.offsetHdr;
                const float expected = colors[i][k]*expectedY/cy;
                check(std::isfinite(rgb[k]) && std::abs(rgb[k]-expected) < .03f*std::max(expected, .02f),
                      "neutral/skin/saturated RGB target survives signed quantization");
            }
            const float y = .2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2];
            check(std::abs(y-expectedY) < .03f*expectedY, "color sweep conserves luminance");
        }
    }
    if (failures == 0) std::printf("ultrahdr_map_fit_test ok\n");
    return failures == 0 ? 0 : 1;
}
