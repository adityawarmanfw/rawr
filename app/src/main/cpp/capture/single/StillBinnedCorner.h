#pragma once

#include <android/log.h>
#include <memory>
#include <utility>

#include "capture/FullSizeFallbacks.h"
#include "imaging/BinnedCorner.h"
#include "imaging/RawSnapshot.h"

namespace rawrcam::capture {
// A failed full-size sensor switch can leave the base readout in the corner.
// Its CFA belongs to the base stream, not to the requested photo mode.
inline void keepBinnedCorner(imaging::RawSnapshot& frame) {
    const auto context = frame.metadata.cameraContext;
    if (!context) return;
    const auto check = imaging::fullSizeBinnedCorner(frame.raw16, frame.width, frame.height,
                                                     context->geometry.pixelArrayWidth,
                                                     context->geometry.pixelArrayHeight,
                                                     frame.metadata.vendorSeamlessMode);
    const uint32_t d = check.divisor;
    if (d <= 1) return;
    const uint32_t width = frame.width / d, height = frame.height / d;
    const auto& base = context->binnedFallbackContext;
    // Crop every detected corner, whatever its size. The full-size context carries the photo
    // stream's CFA (photoCfaBlock), but a corner frame is binned base-stream data, so when a
    // base context was recorded (separate photo stream) its CFA is restored, including for
    // block-CFA photo streams. Without one, the frame's own CFA is kept.
    auto cropped = std::make_shared<metadata::CameraContextMetadata>(*context);
    if (base) cropped->cfaBlock = base->cfaBlock;
    cropped->geometry.rawBufferWidth = width;
    cropped->geometry.rawBufferHeight = height;
    __android_log_print(ANDROID_LOG_WARN, "RawrCamNative",
                        "FULL_SIZE_BINNED_CORNER requestId=%llu raw=%ux%u kept=%ux%u reason=%s seamlessMode=%lld",
                        static_cast<unsigned long long>(frame.requestId), frame.width, frame.height, width, height,
                        check.reason, static_cast<long long>(frame.metadata.vendorSeamlessMode.value_or(-1)));
    imaging::cropPackedRaw16ToCorner(frame.raw16, frame.width, frame.height, d);
    frame.width = width;
    frame.height = height;
    frame.packedRowStrideBytes = width * 2;
    frame.metadata.cameraContext = std::move(cropped);
    fullSizeFallbacks().binnedStills.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace rawrcam::capture
