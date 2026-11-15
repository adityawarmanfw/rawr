#pragma once

#include <cstdint>
#include <optional>

namespace rawrcam::geometry {

struct NormalizedPoint {
    float x = 0.5f;
    float y = 0.5f;
};

// Centered crop window in pre-rotation source pixels (same space as the
// sourceWidth/sourceHeight arguments). Disabled (or empty) means the full
// source. Mirrors the recording source rect (see video_pipeline/VideoCrop.h)
// so tap-to-focus, spot-AE, and face boxes track the recorded crop exactly.
struct SourceCropRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    bool enabled = false;
};

// Converts a normalized point in the displayed Surface into normalized sensor/source
// coordinates using the exact same rotation and aspect-fit contract as present.frag.
std::optional<NormalizedPoint> displayToSource(float displayX, float displayY, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t surfaceWidth, uint32_t surfaceHeight,
                                               int rotationDegrees);
std::optional<NormalizedPoint> displayToSource(float displayX, float displayY, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t surfaceWidth, uint32_t surfaceHeight,
                                               int rotationDegrees, const SourceCropRect& crop);

// Exact inverse of displayToSource (same rotation/aspect-fit contract): sensor/
// source normalized -> displayed Surface normalized. Used for overlaying
// sensor-space detections (faces) on the viewfinder.
std::optional<NormalizedPoint> sourceToDisplay(float sourceX, float sourceY, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t surfaceWidth, uint32_t surfaceHeight,
                                               int rotationDegrees);
std::optional<NormalizedPoint> sourceToDisplay(float sourceX, float sourceY, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t surfaceWidth, uint32_t surfaceHeight,
                                               int rotationDegrees, const SourceCropRect& crop);

// Centered pre-rotation crop rect -> rotated-frame window fractions
// [x0,y0,x1,y1]. Identity when the crop is disabled/empty/out of range.
// Shared with the presentation shader path (PresentRecorder) so Kotlin-side
// focus boxes, face boxes, and displayed pixels always agree.
inline void cropWindowFractions(uint32_t sourceWidth, uint32_t sourceHeight, const SourceCropRect& crop,
                                uint32_t quarterTurns, float& x0, float& y0, float& x1, float& y1) noexcept {
    x0 = 0.0f;
    y0 = 0.0f;
    x1 = 1.0f;
    y1 = 1.0f;
    if (!crop.enabled || crop.w == 0 || crop.h == 0 || sourceWidth == 0 || sourceHeight == 0) return;
    if (crop.x + crop.w > sourceWidth || crop.y + crop.h > sourceHeight) return;
    // The window in source fractions, then turned into the rotated frame the
    // presentation samples in. present.frag applies the window to its output
    // UV and only then undoes the rotation (sourceUv), so each quarter turn
    // sends the window's origin to a different corner: a window that is not
    // centred needs the turn count, not just whether the axes swap.
    const float fx0 = float(crop.x) / float(sourceWidth);
    const float fx1 = float(crop.x + crop.w) / float(sourceWidth);
    const float fy0 = float(crop.y) / float(sourceHeight);
    const float fy1 = float(crop.y + crop.h) / float(sourceHeight);
    switch (quarterTurns & 3u) {
        case 1u:  // sourceUv: (q.y, 1 - q.x)
            x0 = 1.0f - fy1;
            x1 = 1.0f - fy0;
            y0 = fx0;
            y1 = fx1;
            return;
        case 2u:  // sourceUv: (1 - q.x, 1 - q.y)
            x0 = 1.0f - fx1;
            x1 = 1.0f - fx0;
            y0 = 1.0f - fy1;
            y1 = 1.0f - fy0;
            return;
        case 3u:  // sourceUv: (1 - q.y, q.x)
            x0 = fy0;
            x1 = fy1;
            y0 = 1.0f - fx1;
            y1 = 1.0f - fx0;
            return;
        default:
            x0 = fx0;
            x1 = fx1;
            y0 = fy0;
            y1 = fy1;
            return;
    }
}

}  // namespace rawrcam::geometry
