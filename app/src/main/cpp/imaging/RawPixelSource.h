#pragma once

#include <array>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <string>

#include "geometry/RawGeometry.h"

namespace rawrcam::imaging {

// RAW10 groups are four MSB bytes followed by one byte of the four 2-bit LSBs.
using geometry::RawPixelFormat;
using geometry::rawPixelFormatName;

// A CPU-visible view of one RAW plane as delivered by the camera.
struct RawPixelSource {
    const uint8_t* base = nullptr;
    RawPixelFormat format = RawPixelFormat::Raw16;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t rowStrideBytes = 0;
};

// Minimum row bytes a valid source must provide for its width.
[[nodiscard]] size_t minimumRowBytes(RawPixelFormat format, uint32_t width) noexcept;

[[nodiscard]] uint16_t readRawPixel(const RawPixelSource& source, uint32_t x, uint32_t y) noexcept;

// Unpacks one RAW10 row (width must be a multiple of 4) into uint16 values.
void unpackRaw10Row(const uint8_t* packed, uint32_t width, uint16_t* destination) noexcept;

// Copies the source into tightly packed RAW16 rows (stride = width). Returns
// false when the source layout is invalid for its declared width.
bool copyRawToPackedRaw16(const RawPixelSource& source, uint16_t* destination) noexcept;

struct RawContentStatsInput {
    // CFA as Camera2 SENSOR_INFO_COLOR_FILTER_ARRANGEMENT (0=RGGB 1=GRBG 2=GBRG 3=BGGR).
    int cfa = 0;
    float blackLevel = 0.0f;
    float whiteLevel = 1023.0f;
    // Sample every Nth Bayer quad in each direction; 1 samples every quad.
    uint32_t quadStep = 8;
};

// Share of aligned 2x2 / 4x4 blocks whose pixels are all equal, in a centred
// window: a vendor full-size mode can hand over a binned frame upscaled by
// pixel repetition, which only this tells apart from real full-size detail.
struct RawBlockUniformity {
    double uniform2 = 0.0;
    double uniform4 = 0.0;
    uint64_t sampled = 0;          // 2x2 blocks counted (not at black or white)
    const char* verdict = "flat";  // "real", "upscaled2x", "upscaled4x" or "flat"
};
[[nodiscard]] RawBlockUniformity rawBlockUniformity(const RawPixelSource& source, const RawContentStatsInput& input);

// Lower-tail sample per physical R,Gr,Gb,B channel. Sparse sampling keeps this
// cheap on binned viewfinder frames; it never changes the frame's levels.
[[nodiscard]] std::optional<std::array<float, 4>> rawBlackFloor(const RawPixelSource& source, int cfa);

// One-line summary of the RAW plane content, to tell a black or garbage
// sensor buffer apart from a development problem in device logs.
[[nodiscard]] std::string rawContentStatsLine(const RawPixelSource& source, const RawContentStatsInput& input);

}  // namespace rawrcam::imaging
