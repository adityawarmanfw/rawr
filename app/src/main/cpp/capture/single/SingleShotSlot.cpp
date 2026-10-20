#include "capture/single/SingleShotSlot.h"

#include "capture/single/StillCfaRemosaic.h"
#include "imaging/Raw16CpuSnapshot.h"

#ifndef NDEBUG
#include "diagnostics/probes/Raw16SourceParityProbe.h"
#endif

#include <android/log.h>

#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rawrcam::capture {

SingleShotSlot::SingleShotSlot(Diagnostic diagnostic) : diagnostic_(std::move(diagnostic)) {}

void SingleShotSlot::configure(uint32_t width, uint32_t height, uint64_t cameraGeneration) {
    if (width == 0 || height == 0) throw std::invalid_argument("still capture geometry is empty");
    const uint64_t packedRowBytes = static_cast<uint64_t>(width) * sizeof(uint16_t);
    const uint64_t totalBytes = packedRowBytes * static_cast<uint64_t>(height);
    if (packedRowBytes > std::numeric_limits<uint32_t>::max() ||
        totalBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::overflow_error("still capture RAW allocation overflow");
    }

    disabled_ = false;
    configuredWidth_ = width;
    configuredHeight_ = height;
    configuredCameraGeneration_ = cameraGeneration;
    // Preserve only the intentional pre-start request semantics: a request made
    // before any camera was configured binds to the first configured generation.
    if (pendingRequestId_ && !pendingCameraGeneration_) {
        pendingCameraGeneration_ = cameraGeneration;
    }
    completedFrame_.reset();
    prepareStagingSlot();

    std::ostringstream out;
    out << "RAW_STILL_CAPTURE_CONFIG raw=" << width << "x" << height << " packedRowBytes=" << slot_.packedRowStrideBytes
        << " packedBytes=" << slot_.raw16.size() << " slots=1 allocationTiming=outside_shutter";
    emit(out.str());
}

void SingleShotSlot::prepareStagingSlot() {
    if (configuredWidth_ == 0 || configuredHeight_ == 0) {
        slot_ = {};
        return;
    }
    const uint64_t packedRowBytes = static_cast<uint64_t>(configuredWidth_) * sizeof(uint16_t);
    const uint64_t totalBytes = packedRowBytes * static_cast<uint64_t>(configuredHeight_);

    slot_ = {};
    slot_.width = configuredWidth_;
    slot_.height = configuredHeight_;
    slot_.packedRowStrideBytes = static_cast<uint32_t>(packedRowBytes);
    slot_.raw16.resize(static_cast<size_t>(totalBytes));
}

void SingleShotSlot::disable(const std::string& reason) {
    disabled_ = true;
    pendingRequestId_.reset();
    pendingCameraGeneration_.reset();
    expectedOptimizedRequestId_.reset();
    emit("RAW_STILL_CAPTURE_DISABLED reason=" + reason);
}

void SingleShotSlot::reset() noexcept {
    pendingRequestId_.reset();
    pendingCameraGeneration_.reset();
    expectedOptimizedRequestId_.reset();
    completedFrame_.reset();
    disabled_ = false;
    configuredWidth_ = 0;
    configuredHeight_ = 0;
    configuredCameraGeneration_ = 0;
    slot_ = {};
}

bool SingleShotSlot::configured() const noexcept {
    return !disabled_ && configuredWidth_ != 0 && configuredHeight_ != 0 && !slot_.raw16.empty();
}

bool SingleShotSlot::requestCapture(uint64_t requestId) {
    if (disabled_) {
        emit("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=capture_disabled");
        return false;
    }
    if (requestId == 0) {
        emit("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=invalid_request_id");
        return false;
    }
    if (pendingRequestId_) {
        emit("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=request_already_pending pendingRequestId=" +
             std::to_string(*pendingRequestId_));
        return false;
    }
    if (completedFrame_) {
        emit("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=snapshot_slot_occupied requestId=" +
             std::to_string(completedFrame_->requestId));
        return false;
    }
    pendingRequestId_ = requestId;
    pendingCameraGeneration_ =
        configuredCameraGeneration_ != 0 ? std::optional<uint64_t>{configuredCameraGeneration_} : std::nullopt;
    emit("RAW_STILL_CAPTURE_REQUEST_ACCEPTED requestId=" + std::to_string(requestId) +
         " waitsForNextMatchedRaw=true cameraGeneration=" +
         (pendingCameraGeneration_ ? std::to_string(*pendingCameraGeneration_)
                                   : std::string("bind_on_first_configure")));
    return true;
}

void SingleShotSlot::expectOptimizedFrame(uint64_t requestId, std::chrono::milliseconds extraWait) {
    if (!pendingRequestId_ || *pendingRequestId_ != requestId) return;
    expectedOptimizedRequestId_ = requestId;
    optimizedWaitFrames_ = 0;
    optimizedLong_ = extraWait.count() > 0;
    optimizedDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2) + extraWait;
    emit("RAW_STILL_CAPTURE_EXPECT_OPTIMIZED requestId=" + std::to_string(requestId) +
         " extraWaitMs=" + std::to_string(extraWait.count()));
}

void SingleShotSlot::cancelPendingCapture(const std::string& reason) {
    if (!pendingRequestId_) return;
    const uint64_t requestId = *pendingRequestId_;
    pendingRequestId_.reset();
    pendingCameraGeneration_.reset();
    expectedOptimizedRequestId_.reset();
    emit("RAW_STILL_CAPTURE_FAILED requestId=" + std::to_string(requestId) + " reason=" + reason);
}

bool SingleShotSlot::captureMatchedFrame(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd,
                                         const metadata::FrameMetadataSnapshot& metadata,
                                         const color::FrameColorTransform& colorState, int* gpuAcquireFenceFd) {
    if (gpuAcquireFenceFd) *gpuAcquireFenceFd = acquireFenceFd;
    if (!pendingRequestId_ || !configured() || gpuAcquireFenceFd == nullptr) return false;
    const uint64_t requestId = *pendingRequestId_;
    if (expectedOptimizedRequestId_ &&
        (!metadata.optimizedStillRequestId || *metadata.optimizedStillRequestId != *expectedOptimizedRequestId_)) {
        ++optimizedWaitFrames_;
        if ((optimizedLong_ || optimizedWaitFrames_ < 8) && std::chrono::steady_clock::now() < optimizedDeadline_) return false;
        emit("RAW_STILL_CAPTURE_OPTIMIZED_TIMEOUT requestId=" + std::to_string(requestId) +
             " action=fallback_to_camera2_frame");
        expectedOptimizedRequestId_.reset();
    }
    if (metadata.timestampNs == 0) {
        pendingRequestId_.reset();
        pendingCameraGeneration_.reset();
        emit("RAW_STILL_CAPTURE_FAILED requestId=" + std::to_string(requestId) +
             " reason=missing_paired_sensor_timestamp");
        return false;
    }
    if (!metadata.cameraContext) {
        pendingRequestId_.reset();
        pendingCameraGeneration_.reset();
        emit("RAW_STILL_CAPTURE_FAILED requestId=" + std::to_string(requestId) +
             " timestampNs=" + std::to_string(metadata.timestampNs) + " reason=missing_camera_context");
        return false;
    }
    if (!pendingCameraGeneration_ || metadata.cameraContext->cameraContextGeneration != *pendingCameraGeneration_) {
        pendingRequestId_.reset();
        const std::string expectedGeneration =
            pendingCameraGeneration_ ? std::to_string(*pendingCameraGeneration_) : std::string("none");
        pendingCameraGeneration_.reset();
        emit("RAW_STILL_CAPTURE_FAILED requestId=" + std::to_string(requestId) +
             " timestampNs=" + std::to_string(metadata.timestampNs) +
             " reason=camera_generation_mismatch expectedGeneration=" + expectedGeneration +
             " actualGeneration=" + std::to_string(metadata.cameraContext->cameraContextGeneration));
        return false;
    }
    if (metadata.cameraContext->geometry.rawBufferWidth != slot_.width ||
        metadata.cameraContext->geometry.rawBufferHeight != slot_.height) {
        pendingRequestId_.reset();
        pendingCameraGeneration_.reset();
        emit("RAW_STILL_CAPTURE_FAILED requestId=" + std::to_string(requestId) +
             " timestampNs=" + std::to_string(metadata.timestampNs) + " reason=camera_context_geometry_mismatch");
        return false;
    }

    const rawrcam::imaging::Raw16CpuSnapshotResult copied =
        rawrcam::imaging::copyRaw16AhbToPacked(image, ahb, acquireFenceFd, slot_.width, slot_.height, slot_.raw16);
    *gpuAcquireFenceFd = copied.gpuAcquireFenceFd;
    if (!copied.success) {
        pendingRequestId_.reset();
        pendingCameraGeneration_.reset();
        emit("RAW_STILL_CAPTURE_FAILED requestId=" + std::to_string(requestId) +
             " timestampNs=" + std::to_string(metadata.timestampNs) + " reason=" + copied.error);
        return false;
    }

#ifndef NDEBUG
    // Diagnostic-only hook: completely compiled out of release builds. It performs an
    // additional read-only mapping only when explicitly enabled through adb setprop.
    if (rawrcam::diagnostics::raw16SourceParityProbeEnabled()) {
        *gpuAcquireFenceFd = rawrcam::diagnostics::runRaw16SourceParityProbe(
            image, ahb, *gpuAcquireFenceFd, slot_.width, slot_.height, slot_.raw16, diagnostic_);
        if (*gpuAcquireFenceFd == -2) {
            pendingRequestId_.reset();
            pendingCameraGeneration_.reset();
            emit("RAW_STILL_CAPTURE_FAILED requestId=" + std::to_string(requestId) + " timestampNs=" +
                 std::to_string(metadata.timestampNs) + " reason=raw16_source_parity_probe_unlock_failed");
            return false;
        }
    }
#endif

    slot_.requestId = requestId;
    slot_.timestampNs = metadata.timestampNs;
    slot_.sourceRowStrideBytes = copied.sourceRowStrideBytes;
    slot_.sourcePixelStrideBytes = copied.sourcePixelStrideBytes;
    slot_.metadata = metadata;
    slot_.colorState = colorState;
    capture::remosaicBlockCfaStill(slot_);
    pendingRequestId_.reset();
    pendingCameraGeneration_.reset();
    expectedOptimizedRequestId_.reset();
    completedFrame_ = std::make_shared<const rawrcam::imaging::RawSnapshot>(std::move(slot_));
    const auto& completed = *completedFrame_;
    {
        // Always to logcat: tells a black/garbage sensor buffer apart from a
        // development problem when a device only gives us an exported logcat.
        const std::string stats = rawrcam::imaging::stillRawContentStatsLine(completed);
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "%s", stats.c_str());
        emit(stats);
    }

    std::ostringstream out;
    out << "RAW_STILL_CAPTURE_SNAPSHOT_READY requestId=" << requestId << " timestampNs=" << completed.timestampNs
        << " frameOrdinal=" << completed.metadata.frameOrdinal << " raw=" << completed.width << "x" << completed.height
        << " sourceRowStrideBytes=" << completed.sourceRowStrideBytes
        << " sourcePixelStrideBytes=" << completed.sourcePixelStrideBytes
        << " packedRowStrideBytes=" << completed.packedRowStrideBytes << " packedBytes=" << completed.raw16.size()
        << " exposureTimeNs=" << completed.metadata.exposureTimeNs << " sensitivity=" << completed.metadata.sensitivity
        << " lockMs=" << copied.lockMs << " copyMs=" << copied.copyMs << " unlockMs=" << copied.unlockMs
        << " gpuAcquireFence=" << copied.gpuAcquireFenceFd
        << " cameraContextPinned=" << (completed.metadata.cameraContext ? "true" : "false");
    emit(out.str());
    return true;
}

std::shared_ptr<rawrcam::imaging::RawSnapshot> SingleShotSlot::claimMatchedFrameForDeferredCopy(
    const metadata::FrameMetadataSnapshot& metadata, const color::FrameColorTransform& colorState) {
    if (!pendingRequestId_) return {};
    // Pre-startup shutter: preserved intentionally until configured.
    if (!configured()) return {};
    // Staging moved out by an in-flight claim, awaiting recycle: retry
    // briefly, then fail loudly instead of sticking pending forever
    // (which wedges every future capture).
    if (slot_.raw16.empty()) {
        if (++stagingWaitFrames_ > 90) {
            stagingWaitFrames_ = 0;
            const uint64_t stuckId = *pendingRequestId_;
            pendingRequestId_.reset();
            pendingCameraGeneration_.reset();
            emit("RAW_STILL_CAPTURE_FAILED requestId=" + std::to_string(stuckId) +
                 " timestampNs=" + std::to_string(metadata.timestampNs) + " reason=staging_slot_unavailable");
            return {};
        }
        return {};
    }
    stagingWaitFrames_ = 0;
    const uint64_t requestId = *pendingRequestId_;
    if (expectedOptimizedRequestId_ &&
        (!metadata.optimizedStillRequestId || *metadata.optimizedStillRequestId != *expectedOptimizedRequestId_)) {
        ++optimizedWaitFrames_;
        if ((optimizedLong_ || optimizedWaitFrames_ < 8) && std::chrono::steady_clock::now() < optimizedDeadline_) return {};
        emit("RAW_STILL_CAPTURE_OPTIMIZED_TIMEOUT requestId=" + std::to_string(requestId) +
             " action=fallback_to_camera2_frame");
        expectedOptimizedRequestId_.reset();
    }
    auto reject = [&](const std::string& reason) {
        pendingRequestId_.reset();
        pendingCameraGeneration_.reset();
        stagingWaitFrames_ = 0;
        emit("RAW_STILL_CAPTURE_FAILED requestId=" + std::to_string(requestId) +
             " timestampNs=" + std::to_string(metadata.timestampNs) + " reason=" + reason);
        return std::shared_ptr<rawrcam::imaging::RawSnapshot>{};
    };
    if (metadata.timestampNs == 0) return reject("missing_paired_sensor_timestamp");
    if (!metadata.cameraContext) return reject("missing_camera_context");
    if (!pendingCameraGeneration_ || metadata.cameraContext->cameraContextGeneration != *pendingCameraGeneration_) {
        return reject("camera_generation_mismatch");
    }
    if (metadata.cameraContext->geometry.rawBufferWidth != slot_.width ||
        metadata.cameraContext->geometry.rawBufferHeight != slot_.height) {
        return reject("camera_context_geometry_mismatch");
    }
    slot_.requestId = requestId;
    slot_.timestampNs = metadata.timestampNs;
    slot_.metadata = metadata;
    slot_.colorState = colorState;
    pendingRequestId_.reset();
    pendingCameraGeneration_.reset();
    expectedOptimizedRequestId_.reset();
    auto claimed = std::make_shared<rawrcam::imaging::RawSnapshot>(std::move(slot_));
    emit("RAW_STILL_CAPTURE_DEFERRED requestId=" + std::to_string(requestId) +
         " timestampNs=" + std::to_string(metadata.timestampNs) + " waitsForPreviewFence=true");
    return claimed;
}

void SingleShotSlot::recycleDeferredSlot() noexcept {
    if (!slot_.raw16.empty() || configuredWidth_ == 0 || configuredHeight_ == 0) return;
    try {
        prepareStagingSlot();
    } catch (...) {
        slot_ = {};
    }
}

const rawrcam::imaging::RawSnapshot* SingleShotSlot::completedCapture() const noexcept { return completedFrame_.get(); }

std::shared_ptr<const rawrcam::imaging::RawSnapshot> SingleShotSlot::completedCaptureHandle() const noexcept {
    return completedFrame_;
}

void SingleShotSlot::releaseCompletedCapture() noexcept {
    if (!completedFrame_) return;
    const uint64_t requestId = completedFrame_->requestId;
    completedFrame_.reset();
    try {
        prepareStagingSlot();
    } catch (...) {
        // releaseCompletedCapture is noexcept. Allocation failure leaves capture
        // temporarily unconfigured; the next camera configure recreates staging.
        slot_ = {};
    }
    emit("RAW_STILL_CAPTURE_SLOT_RELEASED requestId=" + std::to_string(requestId));
}

void SingleShotSlot::emit(const std::string& line) const {
    if (diagnostic_) diagnostic_(line);
}

}  // namespace rawrcam::capture
