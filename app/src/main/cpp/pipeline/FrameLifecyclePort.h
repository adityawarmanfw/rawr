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

class FrameLifecyclePort {
   public:
    virtual ~FrameLifecyclePort() = default;
    virtual void postDiagnostic(const std::string& line) = 0;
    virtual void postAudit(const std::string& line) = 0;
    virtual void notifySwapchainOutOfDate() = 0;
    virtual void finalizeDetachedStill() = 0;
    virtual void applyAutoKey(tonemap::TonemapParams& tone) = 0;
    virtual float postGainFor(const rawrcam::metadata::FrameMetadataSnapshot& metadata) = 0;
};
}  // namespace rawrcam::pipeline
