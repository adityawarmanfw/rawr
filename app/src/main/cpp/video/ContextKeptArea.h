#pragma once
#include <algorithm>
#include <cstdint>

#include "metadata/CameraContextMetadata.h"
#include "video_pipeline/VideoCrop.h"

namespace rawrcam::video {
// Image area of a camera context for video framing. Above 30 fps the sensor
// mode can pad more edges (lens profile image.cropEdgesHighFps).
inline VideoKeptArea contextKeptArea(uint32_t rawWidth, uint32_t rawHeight,
                                     const metadata::CameraContextMetadata& m, bool highFps) {
    return videoKeptArea(rawWidth, rawHeight, highFps ? std::max(m.cropLeft, m.highFpsCropLeft) : m.cropLeft,
                         highFps ? std::max(m.cropTop, m.highFpsCropTop) : m.cropTop,
                         highFps ? std::max(m.cropRight, m.highFpsCropRight) : m.cropRight,
                         highFps ? std::max(m.cropBottom, m.highFpsCropBottom) : m.cropBottom);
}
}  // namespace rawrcam::video
