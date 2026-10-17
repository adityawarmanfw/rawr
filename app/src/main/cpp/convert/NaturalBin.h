#pragma once
// 2x Bayer binning in DNG code units for the DNG converter.
//
// Wraps the renderer's NaturalBayerTile (the QB "natural" resampler, see
// renderer/NaturalBayerBin.h and THIRD_PARTY_NOTICES.md) with the same code
// mapping as the reference implementation: codes are black-subtracted and
// scaled by one shared span (WhiteLevel minus the largest black level), the
// result is mapped back per CFA site, then quantized with an optional output
// gain and deterministic TPDF dither. Parity with the reference was checked
// on full 200MP frames (|difference| <= 1 code, float rounding only).
#include "renderer/NaturalBayerBin.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace rawrcam::convert {

struct BinCalibration {
    // All four arrays are in top-left 2x2 buffer phase order: index 2*(y&1)+(x&1).
    std::array<float, 4> black{};
    std::array<float, 4> wbGains{};  // camera-neutral gains (green / neutral), any common scale
    uint16_t white = 0;
    std::array<uint8_t, 4> cfa{};  // R=0, G=1, B=2
};

struct BinQuantize {
    uint16_t gain = 1;   // output codes = input codes * gain (WhiteLevel/BlackLevel scale too)
    bool dither = true;  // TPDF dither at one output code

    // Reference choice: 10-bit sources keep the two bits binning gains as a
    // 4x code scale without dither; deeper sources dither at gain 1.
    static BinQuantize forWhiteLevel(uint32_t white) {
        return white <= 1024 ? BinQuantize{4, false} : BinQuantize{1, true};
    }
};

class NaturalBin2x {
   public:
    // Returns false with a reason when the source cannot be binned.
    bool configure(const BinCalibration& calibration, BinQuantize quantize, uint32_t sourceWidth,
                   uint32_t sourceHeight, std::string* error) {
        auto fail = [&](const char* message) {
            if (error) *error = message;
            return false;
        };
        const auto& p = calibration.cfa;
        const bool bayer = (p == std::array<uint8_t, 4>{0, 1, 1, 2}) || (p == std::array<uint8_t, 4>{1, 0, 2, 1}) ||
                           (p == std::array<uint8_t, 4>{1, 2, 0, 1}) || (p == std::array<uint8_t, 4>{2, 1, 1, 0});
        if (!bayer) return fail("Binning needs an ordinary 2x2 RGB Bayer pattern");
        if (sourceWidth < 16 || sourceHeight < 16 || sourceWidth % 4 || sourceHeight % 4)
            return fail("Binning needs dimensions of at least 16 that are divisible by four");
        const float white = calibration.white;
        float maxBlack = 0;
        for (float b : calibration.black) {
            if (!std::isfinite(b) || b < 0 || b >= white) return fail("BlackLevel must be below WhiteLevel");
            maxBlack = std::max(maxBlack, b);
        }
        float maxGain = 0;
        for (float g : calibration.wbGains) {
            if (!std::isfinite(g) || g <= 0) return fail("White balance gains must be positive");
            maxGain = std::max(maxGain, g);
        }
        if (quantize.gain == 0 || uint32_t(calibration.white) * quantize.gain > 65535u)
            return fail("WhiteLevel times output gain must fit in 16 bits");
        cal_ = calibration;
        quantize_ = quantize;
        sw_ = sourceWidth;
        sh_ = sourceHeight;
        span_ = white - maxBlack;
        floor_ = std::max(1e-20f, (0.25f / span_) * (0.25f / span_));
        for (int i = 0; i < 4; ++i) {
            gains_[i] = calibration.wbGains[i] / maxGain;
            if (calibration.cfa[i] == 0) red_ = i;
        }
        return true;
    }

    uint32_t outputWidth() const { return sw_ / 2; }
    uint32_t outputHeight() const { return sh_ / 2; }

    // First and one-past-last source rows needed for output rows [oy, oy + rows).
    void sourceRows(uint32_t oy, uint32_t rows, uint32_t* top, uint32_t* bottom) const {
        *top = oy * 2 >= kHalo ? oy * 2 - kHalo : 0;
        *bottom = std::min(sh_, (oy + rows) * 2 + kHalo);
    }

    // Bins output rows [oy, oy + rows) (rows <= 32 keeps the tile scratch small).
    // code(x, y) returns the source code at full-frame coordinates inside the
    // rows reported by sourceRows(). out receives outputWidth() codes per row.
    template <class Code, class Check>
    void band(uint32_t oy, uint32_t rows, Code code, Check check, uint16_t* out, size_t stride) {
        const uint32_t ow = outputWidth();
        auto sample = [&](uint32_t x, uint32_t y) {
            const int site = int(y & 1u) * 2 + int(x & 1u);
            return (float(code(x, y)) - cal_.black[site]) / span_;
        };
        values_.resize(size_t(ow) * rows);
        for (uint32_t x = 0; x < ow; x += kTileWidth) {
            check();
            tile_.run(int(x), int(oy), int(std::min(kTileWidth, ow - x)), int(rows), int(sw_), int(sh_), gains_,
                      red_, floor_, sample, check, values_.data() + x, int(ow));
        }
        for (uint32_t y = 0; y < rows; ++y)
            for (uint32_t x = 0; x < ow; ++x) {
                const int site = int((oy + y) & 1u) * 2 + int(x & 1u);
                const float value = values_[size_t(y) * ow + x] * span_ + cal_.black[site];
                out[size_t(y) * stride + x] = pack(value, x, oy + y);
            }
    }

    static constexpr uint32_t kBandRows = 32;

   private:
    static constexpr uint32_t kHalo = 20;
    static constexpr uint32_t kTileWidth = 128;

    static uint32_t hash32(uint32_t h) {
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        return h;
    }

    uint16_t pack(float v, uint32_t x, uint32_t y) const {
        const float white = cal_.white, gain = quantize_.gain, top = white * gain;
        v = std::clamp(v, 0.f, white);
        if (v >= white - white * 8.f * std::numeric_limits<float>::epsilon()) return uint16_t(top + 0.5f);
        float dither = 0;
        if (quantize_.dither) {
            const uint32_t h = hash32(x ^ (y * 0x9E3779B1u));
            dither = (float(h & 0xffffu) - float(h >> 16)) * (1.f / 65536.f);
        }
        return uint16_t(std::clamp(v * gain + dither, 0.f, top) + 0.5f);
    }

    BinCalibration cal_{};
    BinQuantize quantize_{};
    uint32_t sw_ = 0, sh_ = 0;
    float span_ = 1, floor_ = 1e-20f;
    std::array<float, 4> gains_{};
    int red_ = 0;
    renderer::NaturalBayerTile tile_;
    std::vector<float> values_;
};

}  // namespace rawrcam::convert
