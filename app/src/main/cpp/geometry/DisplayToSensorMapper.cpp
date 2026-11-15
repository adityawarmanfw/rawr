#include "geometry/DisplayToSensorMapper.h"

#include <algorithm>

namespace rawrcam::geometry {
std::optional<NormalizedPoint> displayToSource(float displayX, float displayY, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t surfaceWidth, uint32_t surfaceHeight,
                                               int rotationDegrees) {
    return displayToSource(displayX, displayY, sourceWidth, sourceHeight, surfaceWidth, surfaceHeight,
                           rotationDegrees, SourceCropRect{});
}

std::optional<NormalizedPoint> displayToSource(float displayX, float displayY, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t surfaceWidth, uint32_t surfaceHeight,
                                               int rotationDegrees, const SourceCropRect& crop) {
    if (!sourceWidth || !sourceHeight || !surfaceWidth || !surfaceHeight) return std::nullopt;
    if (displayX < 0.0f || displayX > 1.0f || displayY < 0.0f || displayY > 1.0f) return std::nullopt;

    const uint32_t quarterTurns = static_cast<uint32_t>(((rotationDegrees / 90) % 4 + 4) % 4);
    const bool swapped = quarterTurns == 1u || quarterTurns == 3u;
    float winX0, winY0, winX1, winY1;
    cropWindowFractions(sourceWidth, sourceHeight, crop, quarterTurns, winX0, winY0, winX1, winY1);
    // The presentation samples the window, so fit against the window aspect
    // (identical to the full source when the crop is disabled).
    const float displayedSourceWidth =
        (static_cast<float>(swapped ? sourceHeight : sourceWidth)) * (winX1 - winX0);
    const float displayedSourceHeight =
        (static_cast<float>(swapped ? sourceWidth : sourceHeight)) * (winY1 - winY0);
    const float srcAspect = displayedSourceWidth / displayedSourceHeight;
    const float dstAspect = static_cast<float>(surfaceWidth) / static_cast<float>(surfaceHeight);

    float qx = displayX;
    float qy = displayY;
    if (dstAspect > srcAspect) {
        const float w = srcAspect / dstAspect;
        qx = (displayX - 0.5f) / w + 0.5f;
        if (qx < 0.0f || qx > 1.0f) return std::nullopt;
    } else {
        const float h = dstAspect / srcAspect;
        qy = (displayY - 0.5f) / h + 0.5f;
        if (qy < 0.0f || qy > 1.0f) return std::nullopt;
    }
    // Window (rotated frame) -> full source (rotated frame).
    qx = winX0 + qx * (winX1 - winX0);
    qy = winY0 + qy * (winY1 - winY0);

    NormalizedPoint out{};
    if (quarterTurns == 1u) {
        out.x = qy;
        out.y = 1.0f - qx;
    } else if (quarterTurns == 2u) {
        out.x = 1.0f - qx;
        out.y = 1.0f - qy;
    } else if (quarterTurns == 3u) {
        out.x = 1.0f - qy;
        out.y = qx;
    } else {
        out.x = qx;
        out.y = qy;
    }
    out.x = std::clamp(out.x, 0.0f, 1.0f);
    out.y = std::clamp(out.y, 0.0f, 1.0f);
    return out;
}

std::optional<NormalizedPoint> sourceToDisplay(float sourceX, float sourceY, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t surfaceWidth, uint32_t surfaceHeight,
                                               int rotationDegrees) {
    return sourceToDisplay(sourceX, sourceY, sourceWidth, sourceHeight, surfaceWidth, surfaceHeight,
                           rotationDegrees, SourceCropRect{});
}

std::optional<NormalizedPoint> sourceToDisplay(float sourceX, float sourceY, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t surfaceWidth, uint32_t surfaceHeight,
                                               int rotationDegrees, const SourceCropRect& crop) {
    if (!sourceWidth || !sourceHeight || !surfaceWidth || !surfaceHeight) return std::nullopt;
    if (sourceX < 0.0f || sourceX > 1.0f || sourceY < 0.0f || sourceY > 1.0f) return std::nullopt;

    const uint32_t quarterTurns = static_cast<uint32_t>(((rotationDegrees / 90) % 4 + 4) % 4);
    const bool swapped = quarterTurns == 1u || quarterTurns == 3u;
    float winX0, winY0, winX1, winY1;
    cropWindowFractions(sourceWidth, sourceHeight, crop, quarterTurns, winX0, winY0, winX1, winY1);
    const float winW = winX1 - winX0;
    const float winH = winY1 - winY0;
    const float displayedSourceWidth = (static_cast<float>(swapped ? sourceHeight : sourceWidth)) * winW;
    const float displayedSourceHeight = (static_cast<float>(swapped ? sourceWidth : sourceHeight)) * winH;
    const float srcAspect = displayedSourceWidth / displayedSourceHeight;
    const float dstAspect = static_cast<float>(surfaceWidth) / static_cast<float>(surfaceHeight);

    // Inverse rotation first (displayToSource rotates last).
    float qx = sourceX;
    float qy = sourceY;
    if (quarterTurns == 1u) {
        qx = 1.0f - sourceY;
        qy = sourceX;
    } else if (quarterTurns == 2u) {
        qx = 1.0f - sourceX;
        qy = 1.0f - sourceY;
    } else if (quarterTurns == 3u) {
        qx = sourceY;
        qy = 1.0f - sourceX;
    }

    // Full source (rotated frame) -> window, then the fitted display mapping.
    if (winW <= 0.0f || winH <= 0.0f) return std::nullopt;
    qx = (qx - winX0) / winW;
    qy = (qy - winY0) / winH;

    // Inverse letterbox: source [0,1] maps into the fitted image sub-region.
    NormalizedPoint out{};
    if (dstAspect > srcAspect) {
        const float w = srcAspect / dstAspect;
        out.x = (qx - 0.5f) * w + 0.5f;
        out.y = qy;
    } else {
        const float h = dstAspect / srcAspect;
        out.x = qx;
        out.y = (qy - 0.5f) * h + 0.5f;
    }
    out.x = std::clamp(out.x, 0.0f, 1.0f);
    out.y = std::clamp(out.y, 0.0f, 1.0f);
    return out;
}
}  // namespace rawrcam::geometry
