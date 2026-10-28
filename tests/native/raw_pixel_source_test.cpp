#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "imaging/RawPixelSource.h"

using namespace rawrcam::imaging;

namespace {

// Packs four 10-bit values the way Camera2 RAW10 does.
void packRaw10(const uint16_t* values, uint8_t* out) {
    uint8_t lsb = 0;
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(values[i] >> 2);
        lsb = static_cast<uint8_t>(lsb | ((values[i] & 0x3u) << (2 * i)));
    }
    out[4] = lsb;
}

void testRaw10Unpack() {
    const uint16_t values[4] = {0, 1023, 0x2AA, 0x155};
    uint8_t packed[5];
    packRaw10(values, packed);
    assert(packed[4] == ((0 & 3) | ((1023 & 3) << 2) | ((0x2AA & 3) << 4) | ((0x155 & 3) << 6)));
    uint16_t out[4] = {};
    unpackRaw10Row(packed, 4, out);
    for (int i = 0; i < 4; ++i) assert(out[i] == values[i]);

    RawPixelSource source{packed, RawPixelFormat::Raw10, 4, 1, 5};
    for (uint32_t x = 0; x < 4; ++x) assert(readRawPixel(source, x, 0) == values[x]);
}

void testPackedCopyHonoursStride() {
    // RAW16 with 2 pixels of row padding.
    const uint32_t w = 4, h = 2, stridePixels = 6;
    std::vector<uint16_t> raw16(stridePixels * h, 0xFFFF);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) raw16[y * stridePixels + x] = static_cast<uint16_t>(y * 100 + x);
    std::vector<uint16_t> packed(w * h);
    RawPixelSource s16{reinterpret_cast<const uint8_t*>(raw16.data()), RawPixelFormat::Raw16, w, h,
                       stridePixels * sizeof(uint16_t)};
    assert(copyRawToPackedRaw16(s16, packed.data()));
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) assert(packed[y * w + x] == y * 100 + x);

    // RAW10 with row padding (stride 8 bytes for a 4-pixel row).
    std::vector<uint8_t> raw10(8 * h, 0xEE);
    const uint16_t row0[4] = {10, 20, 30, 40};
    const uint16_t row1[4] = {1000, 900, 800, 700};
    packRaw10(row0, raw10.data());
    packRaw10(row1, raw10.data() + 8);
    RawPixelSource s10{raw10.data(), RawPixelFormat::Raw10, w, h, 8};
    assert(copyRawToPackedRaw16(s10, packed.data()));
    for (uint32_t x = 0; x < w; ++x) {
        assert(packed[x] == row0[x]);
        assert(packed[w + x] == row1[x]);
    }

    // Too-small stride is rejected.
    RawPixelSource bad{raw10.data(), RawPixelFormat::Raw10, w, h, 4};
    assert(!copyRawToPackedRaw16(bad, packed.data()));
    // RAW10 width must be a multiple of 4.
    RawPixelSource odd{raw10.data(), RawPixelFormat::Raw10, 6, 1, 10};
    assert(!copyRawToPackedRaw16(odd, packed.data()));
}

void testStatsSeparateChannels() {
    // RGGB 4x4: R=100, Gr=200, Gb=300, B=400.
    const uint32_t w = 4, h = 4;
    std::vector<uint16_t> raw(w * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t i = (y & 1u) * 2u + (x & 1u);
            raw[y * w + x] = static_cast<uint16_t>(100 * (i + 1));
        }
    RawPixelSource source{reinterpret_cast<const uint8_t*>(raw.data()), RawPixelFormat::Raw16, w, h,
                          w * sizeof(uint16_t)};
    RawContentStatsInput input;
    input.cfa = 0;
    input.blackLevel = 64.0f;
    input.whiteLevel = 400.0f;
    input.quadStep = 1;
    const std::string line = rawContentStatsLine(source, input);
    assert(line.find("R=[min=100 mean=100.0") != std::string::npos);
    assert(line.find("Gr=[min=200") != std::string::npos);
    assert(line.find("Gb=[min=300") != std::string::npos);
    assert(line.find("B=[min=400") != std::string::npos);
    assert(line.find("samples=16") != std::string::npos);
    assert(line.find("fracAtOrAboveWhite=0.2500") != std::string::npos);

    // BGGR swaps the R and B readings for the same buffer.
    input.cfa = 3;
    const std::string bggr = rawContentStatsLine(source, input);
    assert(bggr.find("R=[min=400") != std::string::npos);
    assert(bggr.find("B=[min=100") != std::string::npos);

    RawPixelSource empty{};
    assert(rawContentStatsLine(empty, input).find("error=invalid_source") != std::string::npos);
}

// A binned frame upscaled by pixel repetition (what the Xiaomi 17 Ultra HAL
// delivers for an 8192x6144 RAW without its companion) vs real per-pixel noise.
void testBlockUniformity() {
    const uint32_t w = 256, h = 256;
    RawContentStatsInput input;
    input.blackLevel = 64.0f;
    input.whiteLevel = 1023.0f;
    const auto make = [&](uint32_t repeat) {
        std::vector<uint16_t> raw(w * h);
        uint32_t seed = 12345;
        std::vector<uint16_t> binned((w / repeat) * (h / repeat));
        for (auto& v : binned) {
            seed = seed * 1664525u + 1013904223u;
            v = static_cast<uint16_t>(100 + (seed >> 16) % 800);
        }
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) raw[y * w + x] = binned[(y / repeat) * (w / repeat) + x / repeat];
        return raw;
    };
    for (const auto& [repeat, verdict] : {std::pair{1u, "real"}, std::pair{2u, "upscaled2x"}, std::pair{4u, "upscaled4x"}}) {
        const auto raw = make(repeat);
        RawPixelSource source{reinterpret_cast<const uint8_t*>(raw.data()), RawPixelFormat::Raw16, w, h,
                              w * sizeof(uint16_t)};
        const auto u = rawBlockUniformity(source, input);
        assert(std::string(u.verdict) == verdict);
        assert(rawContentStatsLine(source, input).find(std::string("detail=") + verdict) != std::string::npos);
    }
    // A covered lens (everything at black) is flat, not upscaled.
    std::vector<uint16_t> dark(w * h, 64);
    RawPixelSource source{reinterpret_cast<const uint8_t*>(dark.data()), RawPixelFormat::Raw16, w, h,
                          w * sizeof(uint16_t)};
    assert(std::string(rawBlockUniformity(source, input).verdict) == "flat");
}

}  // namespace

int main() {
    testBlockUniformity();
    testRaw10Unpack();
    testPackedCopyHonoursStride();
    testStatsSeparateChannels();
    std::cout << "raw_pixel_source_test passed\n";
    return 0;
}
