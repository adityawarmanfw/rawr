#pragma once
#include <android/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

#include "capture/BlockRemosaicGpu.h"
#include "capture/ReducedRawMetadata.h"
#include "imaging/CfaRemosaic.h"
#include "imaging/RawSnapshot.h"

namespace rawrcam::capture {
// The photo remosaic quality a lens asked for. Detail is quad only.
inline imaging::CfaRemosaicQuality photoRemosaicQuality(const metadata::CameraContextMetadata& context) {
    using Q = imaging::CfaRemosaicQuality;
    const Q quality = context.cfaPhotoQuality == 3u ? Q::Detail : context.cfaPhotoQuality == 2u ? Q::High : Q::Standard;
    return quality == Q::Detail && context.cfaBlock != 2u ? Q::High : quality;
}
inline const char* remosaicQualityName(imaging::CfaRemosaicQuality quality) {
    return quality == imaging::CfaRemosaicQuality::Detail ? "detail"
           : quality == imaging::CfaRemosaicQuality::High ? "high"
                                                          : "standard";
}
// Remosaics one block-CFA RAW16 buffer to Bayer in place: on the GPU when the
// caller holds a BlockRemosaicGpu that supports the lens, otherwise (or when
// the GPU run throws) on the CPU. Returns true when the GPU did it.
inline bool remosaicBlockCfaBuffer(std::vector<uint8_t>& raw16, uint32_t width, uint32_t height,
                                   const metadata::CameraContextMetadata& context, const std::array<float, 4>& black,
                                   BlockRemosaicGpu* gpu) {
    const auto quality = photoRemosaicQuality(context);
    const float blackMean = 0.25f * (black[0] + black[1] + black[2] + black[3]);
    if (gpu && BlockRemosaicGpu::supports(context.cfaBlock, quality)) {
        try {
            gpu->run(raw16, context.cfaBlock, context.rawPreviewCfa, quality, blackMean);
            return true;
        } catch (const BlockRemosaicGpu::PartialFailure&) {
            throw;  // some rows are already Bayer: the CPU path must not run over them
        } catch (const std::exception& e) {
            __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "BLOCK_REMOSAIC_GPU_FALLBACK error=%s", e.what());
        }
    }
    // All cores: 12 MP takes ~115 ms (Standard) / ~140-175 ms (High) at 8 threads on V2562.
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    imaging::remosaicBlockCfa(raw16, width, height, context.cfaBlock, context.rawPreviewCfa, threads, quality, 1.0f,
                              blackMean);
    return false;
}
// A block-CFA lens (CameraContextMetadata::cfaBlock > 1) hands stills over as
// the block mosaic; remosaic the copy to Bayer before anything (journal, DNG,
// develop) reads it, like ingress does for the viewfinder. `gpu` (optional)
// does it on the GPU.
inline void remosaicBlockCfaStill(imaging::RawSnapshot& frame, BlockRemosaicGpu* gpu = nullptr) {
    const auto& context = frame.metadata.cameraContext;
    if (!context || context->cfaBlock <= 1u || reducedRawRequested(*context)) return;
    const auto start = std::chrono::steady_clock::now();
    const bool onGpu = remosaicBlockCfaBuffer(frame.raw16, frame.width, frame.height, *context,
                                              frame.metadata.blackLevelPhysicalRggb, gpu);
    [[maybe_unused]] const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                        "STILL_CFA_REMOSAIC requestId=%llu raw=%ux%u block=%u quality=%s gpu=%d ms=%lld",
                        static_cast<unsigned long long>(frame.requestId), frame.width, frame.height, context->cfaBlock,
                        remosaicQualityName(photoRemosaicQuality(*context)), onGpu ? 1 : 0, ms);
}
}  // namespace rawrcam::capture
