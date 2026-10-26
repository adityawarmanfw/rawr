#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "camera/CameraControlTypes.h"

namespace rawrcam::camera {

// Optional control echoes accompanying an accepted RAW metadata frame. Missing
// tags stay missing; decoding does not decide how camera state should react.
struct CameraControlResult {
    std::optional<int64_t> frameDurationNs;
    std::optional<std::array<int32_t, 2>> targetFpsRange;
    std::optional<uint8_t> aePriority;
    std::optional<int32_t> evSteps;
    std::optional<uint8_t> afState;
    std::optional<uint8_t> afMode;
    std::optional<uint8_t> lensState;
    std::optional<float> focusDistance;
    std::vector<FaceDetection> faces;
};

// Camera2 reports face rectangles in the standard active array. Accept packed
// [left,top,right,bottom] groups and optional scores; clip, normalize and rank.
std::vector<FaceDetection> normalizeCameraFaces(const metadata::RectI& active, const int32_t* rectangles,
                                                size_t rectangleValueCount, const uint8_t* scores, size_t scoreCount,
                                                const int32_t* landmarks = nullptr, size_t landmarkCount = 0,
                                                const int32_t* ids = nullptr, size_t idCount = 0);

}  // namespace rawrcam::camera
