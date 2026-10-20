#pragma once
#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "imaging/CfaRemosaic.h"
#include "imaging/Raw16CpuSnapshot.h"

namespace rawrcam::capture {
// A block-CFA lens (CameraContextMetadata::cfaBlock > 1) hands stills over as
// the block mosaic; remosaic the copy to Bayer before anything (journal, DNG,
// develop) reads it, like ingress does for the viewfinder.
inline void remosaicBlockCfaStill(imaging::RawSnapshot& frame) {
    const auto& context = frame.metadata.cameraContext;
    if (!context || context->cfaBlock <= 1u) return;
    const auto start = std::chrono::steady_clock::now();
    // All cores: 12 MP takes ~115 ms (Standard) / ~140-175 ms (High) at 8 threads on V2562.
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    const auto quality = context->cfaPhotoQuality == 2u ? imaging::CfaRemosaicQuality::High
                                                        : imaging::CfaRemosaicQuality::Standard;
    imaging::remosaicBlockCfa(frame.raw16, frame.width, frame.height, context->cfaBlock, context->rawPreviewCfa,
                              threads, quality);
    [[maybe_unused]] const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                        "STILL_CFA_REMOSAIC requestId=%llu raw=%ux%u block=%u quality=%s ms=%lld",
                        static_cast<unsigned long long>(frame.requestId), frame.width, frame.height, context->cfaBlock,
                        quality == imaging::CfaRemosaicQuality::High ? "high" : "standard", ms);
}
}  // namespace rawrcam::capture
