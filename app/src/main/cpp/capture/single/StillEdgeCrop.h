#pragma once

#include <android/log.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "imaging/RawSnapshot.h"
#include "metadata/CameraContextMetadata.h"

namespace rawrcam::capture {
// Some vendor modes pad the RAW with rows or columns that hold no image (the
// Xiaomi 15 non-root DCG readout leaves 512 blank rows at the bottom). The
// lens profile names them in image.cropEdges; this drops them from the packed
// frame before anything develops or writes it.
//
// Right and bottom only: the CFA origin stays where it was, so the colour
// order needs no fixing. Both are multiples of the CFA period (the profile
// parser refuses anything else).
inline void keepCroppedEdges(imaging::RawSnapshot& frame) {
    const auto context = frame.metadata.cameraContext;
    if (!context || (!context->cropRight && !context->cropBottom)) return;
    if (context->cropRight >= frame.width || context->cropBottom >= frame.height) return;
    const uint32_t width = frame.width - context->cropRight;
    const uint32_t height = frame.height - context->cropBottom;
    if (width == frame.width && height == frame.height) return;
    const size_t srcStride = size_t(frame.width) * 2, dstStride = size_t(width) * 2;
    if (frame.raw16.size() < srcStride * frame.height) return;
    if (dstStride != srcStride)
        for (uint32_t y = 0; y < height; ++y)
            std::memmove(frame.raw16.data() + y * dstStride, frame.raw16.data() + y * srcStride, dstStride);
    frame.raw16.resize(dstStride * height);
    frame.raw16.shrink_to_fit();
    // The context travels with the frame: give the copy the kept geometry so
    // the DNG and every later stage size themselves to it.
    auto cropped = std::make_shared<metadata::CameraContextMetadata>(*context);
    cropped->geometry.rawBufferWidth = width;
    cropped->geometry.rawBufferHeight = height;
    cropped->cropRight = 0;
    cropped->cropBottom = 0;
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                        "STILL_EDGE_CROP requestId=%llu raw=%ux%u kept=%ux%u right=%u bottom=%u",
                        static_cast<unsigned long long>(frame.requestId), frame.width, frame.height, width, height,
                        context->cropRight, context->cropBottom);
    frame.width = width;
    frame.height = height;
    frame.packedRowStrideBytes = uint32_t(dstStride);
    frame.metadata.cameraContext = std::move(cropped);
}
}  // namespace rawrcam::capture
