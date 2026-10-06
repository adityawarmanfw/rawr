#pragma once

#include <array>
#include <cstdint>

namespace rawrcam::metadata {

struct RectI {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    bool valid = false;
};

// Present, non-negative and non-empty.
inline bool validRect(const RectI& r) { return r.valid && r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top; }
inline uint32_t rectWidth(const RectI& r) { return static_cast<uint32_t>(r.right - r.left); }
inline uint32_t rectHeight(const RectI& r) { return static_cast<uint32_t>(r.bottom - r.top); }

struct Matrix3x3 {
    std::array<float, 9> rowMajor{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    bool valid = false;
};

struct SensorGeometry {
    uint32_t rawBufferWidth = 0;
    uint32_t rawBufferHeight = 0;
    uint32_t pixelArrayWidth = 0;
    uint32_t pixelArrayHeight = 0;
    RectI preCorrectionActiveArray{};
    RectI activeArray{};
    int32_t sensorOrientationDegrees = 0;
};

}  // namespace rawrcam::metadata
