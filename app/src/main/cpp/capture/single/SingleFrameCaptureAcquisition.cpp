#include "capture/single/SingleFrameCaptureAcquisition.h"

#include <android/log.h>
#include <unistd.h>

#include <stdexcept>

#include "imaging/BinnedCorner.h"
#include "imaging/Raw16CpuSnapshot.h"
#include "imaging/RawFrameLease.h"
#include "sys/resource.h"

namespace rawrcam::capture {
namespace {
// The HAL sometimes returns a full-size still holding only the binned readout
// in its corner. Save that readout as what it is (a binned frame with its own
// geometry) instead of a mostly black full-size file.
void keepBinnedCorner(imaging::RawSnapshot& frame) {
    const auto& context = frame.metadata.cameraContext;
    if (!context) return;
    const uint32_t d = imaging::fullSizeBinnedCorner(frame.raw16, frame.width, frame.height,
                                                     context->geometry.pixelArrayWidth,
                                                     context->geometry.pixelArrayHeight);
    if (d <= 1) return;
    __android_log_print(ANDROID_LOG_WARN, "RawrCamNative",
                        "FULL_SIZE_BINNED_CORNER requestId=%llu raw=%ux%u kept=%ux%u",
                        static_cast<unsigned long long>(frame.requestId), frame.width, frame.height, frame.width / d,
                        frame.height / d);
    imaging::cropPackedRaw16ToCorner(frame.raw16, frame.width, frame.height, d);
    frame.width /= d;
    frame.height /= d;
    frame.packedRowStrideBytes = frame.width * 2;
}
}  // namespace
bool SingleFrameCaptureAcquisition::busy() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.has_value() || ready_.has_value() || failure_.has_value() || workerRunning_;
}
bool SingleFrameCaptureAcquisition::deferSubmittedFrame(const metadata::FrameMetadataSnapshot& metadata,
                                                        const color::FrameColorTransform& colorState,
                                                        const tonemap::TonemapParams& tonemapParams, float aePostGain,
                                                        bool filmEnabled,
                                                        const spektrafilm_native::FilmLook& filmLook) {
    if (!slot_.captureRequested()) return false;
    const bool lensShadingExpected = metadata.cameraContext && metadata.cameraContext->lensShadingMapWidth >= 2 &&
                                     metadata.cameraContext->lensShadingMapHeight >= 2;
    if (lensShadingExpected && metadata.lensShadingMap.empty()) {
        ++waitFrames_;
        if (waitFrames_ >= 8) failLensShadingTimeout();
        return false;
    }
    auto frame = slot_.claimMatchedFrameForDeferredCopy(metadata, colorState);
    if (!frame) return false;
    SingleFrameSnapshot deferred{};
    deferred.frame = std::move(frame);
    deferred.tonemapParams = tonemapParams;
    deferred.aePostGain = aePostGain;
    deferred.filmEnabled = filmEnabled;
    deferred.filmLook = filmLook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(deferred);
    }
    return true;
}
bool SingleFrameCaptureAcquisition::beginDeferredSnapshot(AImage* imageLease, AHardwareBuffer* ahb,
                                                          uint64_t timestampNs, int acquireFenceFd) {
    if (!imageLease || !ahb) return false;
    std::optional<SingleFrameSnapshot> deferred;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_ || pending_->frame->timestampNs != timestampNs || workerRunning_) return false;
        deferred = std::move(pending_);
        pending_.reset();
        workerRunning_ = true;
    }
    const int copyFence = acquireFenceFd >= 0 ? dup(acquireFenceFd) : -1;
    if (acquireFenceFd >= 0 && copyFence < 0) {
        AImage_delete(imageLease);
        std::lock_guard<std::mutex> lock(mutex_);
        failure_ = "acquire_fence_dup_failed";
        workerRunning_ = false;
        return true;
    }
    imaging::RawFrameLease lease({imageLease, ahb, copyFence, timestampNs, 0});
    try {
        worker_ = std::thread([this, lease = std::move(lease), deferred = std::move(*deferred)]() mutable {
            setpriority(PRIO_PROCESS, 0, 5);
            try {
                const auto& frame = lease.get();
                deferred.copyResult =
                    imaging::copyCompletedRaw16AhbToPacked(frame.ahb, deferred.frame->width, deferred.frame->height,
                                                           deferred.frame->raw16, frame.acquireFenceFd, frame.image);
                // Release the camera's image/fence as soon as CPU reading ends, before disk IO.
                lease = imaging::RawFrameLease({});
                if (deferred.copyResult.success) {
                    deferred.frame->sourceRowStrideBytes = deferred.copyResult.sourceRowStrideBytes;
                    deferred.frame->sourcePixelStrideBytes = deferred.copyResult.sourcePixelStrideBytes;
                    keepBinnedCorner(*deferred.frame);
                    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "%s",
                                        imaging::stillRawContentStatsLine(*deferred.frame).c_str());
                    journal_.commit(deferred);
                }
            } catch (const std::exception& e) {
                deferred.copyResult.success = false;
                deferred.copyResult.error = e.what();
            } catch (...) {
                deferred.copyResult.success = false;
                deferred.copyResult.error = "raw_snapshot_failed";
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (deferred.copyResult.success)
                ready_ = std::move(deferred);
            else
                failure_ = deferred.copyResult.error;
            workerRunning_ = false;
        });
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_ = "raw_copy_thread_failed";
        workerRunning_ = false;
    }
    return true;
}
SingleMatchedFrameResult SingleFrameCaptureAcquisition::processMatchedFrame(
    AImage* image, AHardwareBuffer* ahb, int acquireFenceFd, const metadata::FrameMetadataSnapshot& metadata,
    const color::FrameColorTransform& colorState, const tonemap::TonemapParams& tonemapParams, float aePostGain) {
    SingleMatchedFrameResult result{};
    result.gpuAcquireFenceFd = acquireFenceFd;

    if (!slot_.captureRequested()) return result;

    const bool lensShadingExpected = metadata.cameraContext && metadata.cameraContext->lensShadingMapWidth >= 2 &&
                                     metadata.cameraContext->lensShadingMapHeight >= 2;
    if (lensShadingExpected && metadata.lensShadingMap.empty()) {
        ++waitFrames_;
        if (waitFrames_ >= 8) failLensShadingTimeout();
        return result;
    }

    int gpuAcquireFenceFd = acquireFenceFd;
    const bool copied = slot_.captureMatchedFrame(image, ahb, acquireFenceFd, metadata, colorState, &gpuAcquireFenceFd);
    result.gpuAcquireFenceFd = gpuAcquireFenceFd;
    result.cpuUnlockFailed = gpuAcquireFenceFd == -2;
    if (result.cpuUnlockFailed) return result;

    if (!copied) {
        emit("RAW_STILL_CAPTURE_FRAME_NOT_SNAPSHOTTED timestampNs=" + std::to_string(metadata.timestampNs));
        return result;
    }

    if (auto captured = slot_.completedCaptureHandle()) {
        SingleFrameSnapshot deferred;
        deferred.frame = std::const_pointer_cast<rawrcam::imaging::RawSnapshot>(captured);
        deferred.tonemapParams = tonemapParams;
        deferred.aePostGain = aePostGain;
        slot_.reset();
        std::lock_guard<std::mutex> lock(mutex_);
        workerRunning_ = true;
        try {
            worker_ = std::thread([this, deferred = std::move(deferred)]() mutable {
                setpriority(PRIO_PROCESS, 0, 5);
                std::string error;
                try {
                    journal_.commit(deferred);
                } catch (const std::exception& e) {
                    error = e.what();
                } catch (...) {
                    error = "raw_journal_failed";
                }
                std::lock_guard<std::mutex> guard(mutex_);
                if (error.empty())
                    ready_ = std::move(deferred);
                else
                    failure_ = error;
                workerRunning_ = false;
            });
        } catch (...) {
            workerRunning_ = false;
            failure_ = "spool_thread_failed";
        }
    }
    return result;
}
bool SingleFrameCaptureAcquisition::expectsTimestamp(uint64_t timestamp) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_ && pending_->frame->timestampNs == timestamp;
}
std::optional<SingleFrameSnapshot> SingleFrameCaptureAcquisition::takeReady() {
    std::optional<SingleFrameSnapshot> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out = std::move(ready_);
        ready_.reset();
    }
    if (out && worker_.joinable()) worker_.join();
    return out;
}
std::optional<std::string> SingleFrameCaptureAcquisition::takeFailure() {
    std::optional<std::string> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out = std::move(failure_);
        failure_.reset();
    }
    if (out && worker_.joinable()) worker_.join();
    return out;
}
void SingleFrameCaptureAcquisition::failLensShadingTimeout(const std::string& reason) {
    slot_.cancelPendingCapture(reason);
}
void SingleFrameCaptureAcquisition::cancel(const std::string& reason) {
    // Committed/worker-owned input survives camera reader destruction.
    slot_.cancelPendingCapture(reason);
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
}
void SingleFrameCaptureAcquisition::shutdown() noexcept {
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
    ready_.reset();
    failure_.reset();
    workerRunning_ = false;
}

}  // namespace rawrcam::capture
