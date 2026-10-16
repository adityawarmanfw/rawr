#pragma once

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <tonemap/TonemapEngine.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "color/FrameColorTransform.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace spektrafilm_native {
struct FilmLook;
}

namespace rawrcam::pipeline {

class FrameDiagnosticsPort {
   public:
    virtual ~FrameDiagnosticsPort() = default;
    struct RenderedFeedback {
        float lumaP50 = 0.0f;
        float lumaP95 = 0.0f;
        float lumaP99 = 0.0f;
        float brightFraction90 = 0.0f;
        float brightFraction96 = 0.0f;
        float anyChannelClippedFraction = 0.0f;
    };
    // Auto key exposure state after a retired frame's scene measurement.
    struct SceneKeyFeedback {
        float keyEv = 0.0f;      // trimmed weighted median log2 scene luma
        float autoKeyEv = 0.0f;  // smoothed lift of the adaptive curve at the scene key
    };
    struct CpuCopyOutcome {
        bool frameRejected = false;
        bool copied = false;
        int fenceFd = -1;
    };

    virtual void discardScopesSlot(std::uint32_t slotIndex) = 0;
    virtual void retireScopesSlot(std::uint32_t slotIndex) = 0;
    virtual std::optional<RenderedFeedback> consumeExposureFeedback(std::uint32_t slotIndex) = 0;
    // True when frames should record the scene key meter.
    virtual bool sceneKeyMeteringEnabled() = 0;
    // Spot meter rectangle on the full source frame (normalized x0,y0,x1,y1;
    // all zero = whole-frame metering).
    virtual std::array<float, 4> sceneMeteringSpot() = 0;
    // Weighting outside a spot (image_scopes::SceneWeighting; 0 = matrix).
    virtual uint32_t sceneMeteringWeighting() = 0;
    // Consumes the slot's scene measurement and advances the auto key lift.
    virtual std::optional<SceneKeyFeedback> updateSceneKey(std::uint32_t slotIndex,
                                                           const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                                           std::uint64_t timestampNs, bool recording) = 0;
    virtual bool overlayNeedsRawState() = 0;
    virtual void recordAuditFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                  const rawrcam::color::FrameColorTransform& colorState) = 0;
    virtual void integrityComplete(std::uint32_t slotIndex) = 0;
    virtual bool cpuCopyNeedsSample() = 0;
    virtual CpuCopyOutcome sampleCpuCopy(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd,
                                         std::uint64_t timestampNs) = 0;
};
}  // namespace rawrcam::pipeline
