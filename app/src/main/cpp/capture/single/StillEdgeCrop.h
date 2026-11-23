#pragma once

#include <android/log.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "imaging/RawSnapshot.h"
#include "metadata/CameraContextMetadata.h"

namespace rawrcam::capture {
namespace detail {
// Shrinks a sensor rectangle by the dropped edges, in the rectangle's own
// coordinates, and rebases it on the kept area's origin. An empty or absent
// rectangle is left alone.
inline void cropSensorRect(metadata::RectI& r, int32_t left, int32_t top, int32_t right, int32_t bottom,
                           int32_t width, int32_t height) {
    if (!metadata::validRect(r)) return;
    r.left = std::clamp(r.left - left, 0, width);
    r.top = std::clamp(r.top - top, 0, height);
    r.right = std::clamp(r.right - left, 0, width);
    r.bottom = std::clamp(r.bottom - top, 0, height);
    (void)right;
    (void)bottom;
    if (r.right <= r.left || r.bottom <= r.top) r = metadata::RectI{};
}
}  // namespace detail

// Some vendor modes pad the RAW with rows or columns that hold no image (the
// Xiaomi 15 non-root DCG readout leaves 512 blank rows at the bottom). The
// lens profile names them in image.cropEdges; this drops them from the packed
// frame before anything develops or writes it.
//
// Every edge is a multiple of the CFA period (the profile parser refuses
// anything else), so the kept area keeps the camera's own colour order.
// The context travels with the frame and carries the sensor geometry the DNG
// writer reconciles the frame against, so the active arrays move with it:
// leave them at the full sensor and the writer cannot place the image and
// gives up with "unresolved stored RAW geometry".
inline void keepCroppedEdges(imaging::RawSnapshot& frame) {
    const auto context = frame.metadata.cameraContext;
    if (!context) return;
    const uint32_t left = context->cropLeft, top = context->cropTop;
    const uint32_t right = context->cropRight, bottom = context->cropBottom;
    if (!left && !top && !right && !bottom) return;
    if (left + right >= frame.width || top + bottom >= frame.height) return;
    const uint32_t width = frame.width - left - right;
    const uint32_t height = frame.height - top - bottom;
    const size_t srcStride = size_t(frame.width) * 2, dstStride = size_t(width) * 2;
    if (frame.raw16.size() < srcStride * frame.height) return;
    // Move as little as the edges ask. Full-width rows (only top and/or bottom dropped) are one contiguous block:
    // dropping the bottom needs no move at all, dropping the top one move. Only dropped columns repack row by row,
    // forwards, so a row never overwrites source it has yet to read. No shrink_to_fit: it would allocate a second
    // frame-sized buffer and copy into it (a full-size frame twice over at peak) to give back the dropped edges.
    if (left == 0 && right == 0) {
        if (top) std::memmove(frame.raw16.data(), frame.raw16.data() + size_t(top) * srcStride, dstStride * height);
    } else {
        const size_t leftBytes = size_t(left) * 2;
        for (uint32_t y = 0; y < height; ++y)
            std::memmove(frame.raw16.data() + y * dstStride, frame.raw16.data() + (y + top) * srcStride + leftBytes,
                         dstStride);
    }
    frame.raw16.resize(dstStride * height);

    auto cropped = std::make_shared<metadata::CameraContextMetadata>(*context);
    cropped->geometry.rawBufferWidth = width;
    cropped->geometry.rawBufferHeight = height;
    const int32_t w = int32_t(width), h = int32_t(height);
    detail::cropSensorRect(cropped->geometry.preCorrectionActiveArray, int32_t(left), int32_t(top), int32_t(right),
                           int32_t(bottom), w, h);
    detail::cropSensorRect(cropped->geometry.activeArray, int32_t(left), int32_t(top), int32_t(right), int32_t(bottom),
                           w, h);
    // The pixel array describes the sensor the frame came from; a cropped
    // frame is no longer that whole array, so the writer's full-array rules
    // must not match it either.
    if (cropped->geometry.pixelArrayWidth == frame.width) cropped->geometry.pixelArrayWidth = width;
    if (cropped->geometry.pixelArrayHeight == frame.height) cropped->geometry.pixelArrayHeight = height;
    cropped->cropLeft = 0;
    cropped->cropTop = 0;
    cropped->cropRight = 0;
    cropped->cropBottom = 0;
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                        "STILL_EDGE_CROP requestId=%llu raw=%ux%u kept=%ux%u left=%u top=%u right=%u bottom=%u",
                        static_cast<unsigned long long>(frame.requestId), frame.width, frame.height, width, height,
                        left, top, right, bottom);
    frame.width = width;
    frame.height = height;
    frame.packedRowStrideBytes = uint32_t(dstStride);
    frame.metadata.cameraContext = std::move(cropped);
}
}  // namespace rawrcam::capture
