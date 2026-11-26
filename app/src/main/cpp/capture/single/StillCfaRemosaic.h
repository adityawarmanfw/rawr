#pragma once
#include <android/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <thread>

#include "capture/BlockRemosaicGpu.h"
#include "capture/ReducedRawMetadata.h"
#include "imaging/CfaRemosaic.h"
#include "imaging/QuadGainField.h"
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
// A quad lens with "Correct per-position response" (photoPhaseFit): the
// sensor's per-position gain field, fitted from this frame (QuadGainField.h)
// unless `cache` already holds one; a burst passes the same cache for every
// frame, so it fits once (the field belongs to the sensor, not the scene).
inline std::optional<imaging::QuadGainField> quadGainFieldFor(const std::vector<uint8_t>& raw16, uint32_t width,
                                                              uint32_t height,
                                                              const metadata::CameraContextMetadata& context,
                                                              float black, float white,
                                                              std::optional<imaging::QuadGainField>* cache) {
    if (!context.photoPhaseFit || context.cfaBlock != 2u) return std::nullopt;
    if (cache && *cache) return *cache;
    if (!(white > black)) white = context.baselineWhiteLevel;
    const auto start = std::chrono::steady_clock::now();
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    auto field = imaging::fitQuadGainField(reinterpret_cast<const uint16_t*>(raw16.data()), width, height, black,
                                           white, threads);
    [[maybe_unused]] const long long us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                        "QUAD_GAIN_FIELD active=%d deviation=%.3f%% noise=%.3f%% fit_us=%lld", field.active ? 1 : 0,
                        100.0f * field.deviation, 100.0f * field.noise, us);
    if (cache) *cache = field;
    return field;
}
// Remosaics one block-CFA RAW16 buffer to Bayer in place: on the GPU when the
// caller holds a BlockRemosaicGpu that supports the lens, otherwise (or when
// the GPU run throws) on the CPU. Returns true when the GPU did it. `white` is
// the raw white level (for the gain field; 0 = the lens baseline).
inline bool remosaicBlockCfaBuffer(std::vector<uint8_t>& raw16, uint32_t width, uint32_t height,
                                   const metadata::CameraContextMetadata& context, const std::array<float, 4>& black,
                                   BlockRemosaicGpu* gpu, float white = 0.0f,
                                   std::optional<imaging::QuadGainField>* fieldCache = nullptr) {
    const auto quality = photoRemosaicQuality(context);
    const float blackMean = 0.25f * (black[0] + black[1] + black[2] + black[3]);
    const auto field = quadGainFieldFor(raw16, width, height, context, blackMean, white, fieldCache);
    const imaging::QuadGainField* activeField = field && field->active ? &*field : nullptr;
    if (gpu && BlockRemosaicGpu::supports(context.cfaBlock, quality)) {
        try {
            gpu->run(raw16, context.cfaBlock, context.rawPreviewCfa, quality, blackMean, activeField);
            return true;
        } catch (const BlockRemosaicGpu::PartialFailure&) {
            throw;  // some rows are already Bayer: the CPU path must not run over them
        } catch (const std::exception& e) {
            __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "BLOCK_REMOSAIC_GPU_FALLBACK error=%s", e.what());
        }
    }
    // All cores: 12 MP takes ~115 ms (Standard) / ~140-175 ms (High) at 8 threads on V2562.
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    if (activeField)
        imaging::applyQuadGainField(reinterpret_cast<uint16_t*>(raw16.data()), width, height, blackMean, *activeField,
                                    threads);
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
    [[maybe_unused]] const bool onGpu = remosaicBlockCfaBuffer(frame.raw16, frame.width, frame.height, *context,
                                              frame.metadata.blackLevelPhysicalRggb, gpu,
                                              frame.metadata.effectiveWhiteLevel);
    [[maybe_unused]] const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                        "STILL_CFA_REMOSAIC requestId=%llu raw=%ux%u block=%u quality=%s gpu=%d ms=%lld",
                        static_cast<unsigned long long>(frame.requestId), frame.width, frame.height, context->cfaBlock,
                        remosaicQualityName(photoRemosaicQuality(*context)), onGpu ? 1 : 0, ms);
}
}  // namespace rawrcam::capture
