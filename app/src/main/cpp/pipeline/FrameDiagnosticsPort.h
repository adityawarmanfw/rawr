#pragma once

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <tonemap/TonemapEngine.h>

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
    struct CpuCopyOutcome {
        bool frameRejected = false;
        bool copied = false;
        int fenceFd = -1;
    };

    virtual void discardScopesSlot(std::uint32_t slotIndex) = 0;
    virtual void retireScopesSlot(std::uint32_t slotIndex) = 0;
    virtual std::optional<RenderedFeedback> consumeExposureFeedback(std::uint32_t slotIndex) = 0;
    // Software shutter/ISO priority: true while the camera needs rendered-frame brightness, so the (otherwise
    // disabled) GPU measurement is recorded. exposureMeter() then receives each measured frame.
    virtual bool exposureMeterWanted() = 0;
    virtual void exposureMeter(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                               const RenderedFeedback& rendered) = 0;
    virtual bool overlayNeedsRawState() = 0;
    virtual void recordAuditFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                  const rawrcam::color::FrameColorTransform& colorState) = 0;
    virtual void integrityComplete(std::uint32_t slotIndex) = 0;
    virtual bool cpuCopyNeedsSample() = 0;
    virtual CpuCopyOutcome sampleCpuCopy(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd,
                                         std::uint64_t timestampNs) = 0;
};
}  // namespace rawrcam::pipeline
