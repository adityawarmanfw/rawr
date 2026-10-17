#include "capture/single/SingleFrameCoordinator.h"

#include <android/log.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "capture/CaptureRequest.h"
#include "capture/multiframe/SharedCompletionMailbox.h"
#include "capture/multiframe/mfsr/MfsrCaptureJob.h"
#include "capture/persistence/CaptureJob.h"
#include "capture/persistence/ProcessingRecipe.h"
#include "capture/single/SingleFrameCaptureJob.h"
#include "capture/single/SingleShotSlot.h"
#include "color/ColorMath.h"
#include "color/FilmExposure.h"
#include "color/WhiteBalance.h"
#include "develop/HotPixelConceal.h"
#include "develop/demosaic/DualStillProcessor.h"
#include "develop/demosaic/RcdStillProcessor.h"
#include "develop/demosaic/VngStillProcessor.h"
#include "develop/render/DenoiseProfile.h"
#include "develop/render/StillImageRenderer.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "encoding/dng/DngCaptureWriter.h"
#include "encoding/jpeg/JpegCaptureWriter.h"
#include "encoding/jpeg/JpegPublicDescription.h"
#include "geometry/OrientationTransform.h"
#include "imaging/Raw16CpuSnapshot.h"
#include "spektrafilm/SpektraFilm.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::capture {

// A job owns its RAW and settings from acquisition through publication. Only acquisition
// runs under the session lock; development, readback, encoding and GPU destruction run
// on this FIFO's worker. Eight presses can be acquired while the first JPEG develops.
class SingleFrameCoordinator::Impl {
   public:
    struct Entry {
        std::unique_ptr<SingleFrameCaptureJob> shot;
        std::thread preparation;
        std::atomic<bool> prepared{false};
        std::atomic<bool> handedOff{false};
        bool preparing = false;
        bool optimized = false;
        bool optimizedApplied = false;
        std::chrono::milliseconds optimizedWait{};
        uint64_t id = 0;
        bool jpegRequested = false;
        bool dngRequested = false;
        std::string dngName, jpegName;
        std::string preparationError;
        std::string recoveryPath;
        bool recoverMultiframe = false;
        // Held until a recovery job takes them; still held here means not yet handed off.
        encoding::OutputFd recoveryDng, recoveryMerged, recoveryJpeg;
        std::chrono::steady_clock::time_point accepted = std::chrono::steady_clock::now();
        ~Entry() {
            if (preparation.joinable()) preparation.join();
        }
    };

    Impl(const rawrcam::vulkan::VulkanContext& vulkan, std::mutex& submitMutex, std::string filesDir,
         Diagnostic diagnostic)
        : vulkan_(vulkan),
          submitMutex_(submitMutex),
          filesDir_(std::move(filesDir)),
          diagnostic_(std::move(diagnostic)),
          worker_([this] { run(); }) {}
    ~Impl() { shutdown(); }

    // Caller holds mutex_. The head job is developed next, so it keeps its RAW in RAM.
    void markHeadResidentLocked() {
        if (!queue_.empty() && queue_.front()->shot) queue_.front()->shot->setResident(true);
    }
    void popFrontLocked() {
        queue_.pop_front();
        markHeadResidentLocked();
    }
    std::vector<std::shared_ptr<Entry>> entries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {queue_.begin(), queue_.end()};
    }
    void prepare(const std::shared_ptr<Entry>& entry) {
        if (entry->preparing || entry->handedOff.load() || !width_ || !height_) return;
        entry->preparing = true;
        try {
            entry->preparation = std::thread([entry, w = width_, h = height_, generation = generation_] {
                setpriority(PRIO_PROCESS, 0, 5);
                try {
                    entry->shot->configureRawSnapshot(w, h, generation);
                } catch (const std::exception& e) {
                    entry->preparationError = e.what();
                } catch (...) {
                    entry->preparationError = "raw_allocation_failed";
                }
                entry->prepared.store(true);
            });
        } catch (...) {
            entry->preparationError = "raw_preparation_thread_failed";
            entry->prepared.store(true);
        }
    }
    void configureRawSnapshot(uint32_t width, uint32_t height, uint64_t generation) {
        width_ = width;
        height_ = height;
        generation_ = generation;
        disabled_ = false;
        for (const auto& entry : entries()) prepare(entry);
    }
    void disableRawSnapshot(const std::string& reason) {
        disabled_ = true;
        cancelAcquisition(reason);
    }
    bool rawSnapshotConfigured() const noexcept { return !disabled_ && width_ && height_; }
    bool captureRequested() const noexcept {
        for (const auto& entry : entries()) {
            if (!entry->handedOff.load()) return true;
        }
        return false;
    }
    uint64_t requestRawStillCapture(encoding::dng::DngCaptureContext dng, rawrcam::capture::JpegCaptureRequest jpeg) {
        // Bound acquisition memory independently of processing speed. Never replace an
        // accepted job. A rejected request still closes the transferred descriptors.
        auto reservation = persistence::reserve(filesDir_, uint64_t(width_) * height_ * 2, true);
        if (disabled_ || stopped_ || !reservation) {
            if (diagnostic_)
                diagnostic_(
                    "RAW_STILL_CAPTURE_REQUEST_REJECTED reason=acquisition_memory_or_storage_budget_or_disabled");
            return 0;
        }
        auto entry = std::make_shared<Entry>();
        entry->jpegRequested = jpeg.output.outputFd.valid();
        entry->dngRequested = dng.outputFd.valid();
        entry->dngName = dng.displayName;
        entry->jpegName = jpeg.output.displayName;
        entry->shot = std::make_unique<SingleFrameCaptureJob>(vulkan_, submitMutex_, filesDir_, diagnostic_);
        entry->shot->setAdmission(persistence::CaptureReservation(std::move(reservation)), persistence::jobPath(filesDir_, dng.displayName), nextId_++);
        entry->shot->setAssetManager(assetManager_);
        entry->id = entry->shot->requestRawStillCapture(std::move(dng), std::move(jpeg));
        if (!entry->id) return 0;
        diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::StillAccepted, 0,
                                                             entry->id);
        prepare(entry);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(entry);
            markHeadResidentLocked();
        }
        changed_.notify_one();
        return entry->id;
    }
    uint64_t recover(const std::string& name, bool multiframe, int dngFd, int mergedFd, int jpegFd) {
        encoding::OutputFd dng(dngFd), merged(mergedFd), jpeg(jpegFd);
        auto entry = std::make_shared<Entry>();
        entry->id = nextId_++;
        entry->jpegRequested = jpegFd >= 0;
        entry->dngRequested = dngFd >= 0;
        entry->dngName = name;
        entry->recoveryPath = persistence::jobPath(filesDir_, name);
        entry->recoverMultiframe = multiframe;
        entry->recoveryDng = std::move(dng);
        entry->recoveryMerged = std::move(merged);
        entry->recoveryJpeg = std::move(jpeg);
        entry->prepared = true;
        entry->handedOff = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(entry);
        }
        diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::StillRecovered, 0,
                                                             entry->id, -1, 0, 0, multiframe ? 1u : 0u);
        changed_.notify_one();
        return entry->id;
    }
    void expectOptimizedFrame(uint64_t id, std::chrono::milliseconds extraWait) {
        for (const auto& entry : entries())
            if (entry->id == id) {
                entry->optimized = true;
                entry->optimizedWait = extraWait;
            }
    }
    void handOff(const std::shared_ptr<Entry>& entry) {
        entry->handedOff.store(true);
        changed_.notify_one();
    }
    void advance() {
        for (const auto& entry : entries()) {
            if (entry->handedOff.load() || !entry->prepared.load()) continue;
            auto& shot = *entry->shot;
            if (!entry->preparationError.empty() ||
                std::chrono::steady_clock::now() - entry->accepted > std::chrono::seconds(10) + entry->optimizedWait) {
                // No copy worker exists before handoff, so cancelling a missing frame
                // cannot race an AHB read. Every accepted request receives failure events.
                shot.cancelAcquisition(entry->preparationError.empty() ? "raw_acquisition_timeout"
                                                                       : entry->preparationError);
                handOff(entry);
            } else if (entry->optimized && !entry->optimizedApplied) {
                shot.expectOptimizedFrame(entry->id, entry->optimizedWait);
                entry->optimizedApplied = true;
            }
        }
    }
    void advanceDng() { advance(); }
    void advanceHq() { advance(); }
    SingleMatchedFrameResult processMatchedFrame(AImage* image, AHardwareBuffer* ahb, int fence,
                                                 const metadata::FrameMetadataSnapshot& metadata,
                                                 const color::FrameColorTransform& color,
                                                 const tonemap::TonemapParams& tone, float gain) {
        advance();
        SingleMatchedFrameResult result{};
        result.gpuAcquireFenceFd = fence;
        for (const auto& entry : entries()) {
            if (entry->handedOff.load() || !entry->prepared.load() || !entry->shot->captureRequested()) continue;
            result = entry->shot->processMatchedFrame(image, ahb, fence, metadata, color, tone, gain);
            if (!entry->shot->captureRequested()) {
                if (!entry->shot->deferredSnapshotBusy() && !entry->shot->hasDngCompletion())
                    entry->shot->failLensShadingTimeout("raw_snapshot_failed");
                handOff(entry);
            }
            break;
        }
        return result;
    }
    bool deferSubmittedFrame(const metadata::FrameMetadataSnapshot& metadata, const color::FrameColorTransform& color,
                             const tonemap::TonemapParams& tone, float gain, bool filmEnabled,
                             const spektrafilm_native::FilmLook& filmLook) {
        advance();
        for (const auto& entry : entries()) {
            if (entry->handedOff.load()) continue;
            // This is the first frame after the shutter (the caller checked the
            // cutoff). Its RAW destination may still be allocating (100 MB at
            // 50 MP): wait for it rather than letting this frame pass, or the
            // photo is taken from a later moment than the press.
            if (!entry->prepared.load()) waitPrepared(*entry, kPreparationWait);
            if (!entry->prepared.load() || !entry->shot->captureRequested()) continue;
            const bool claimed = entry->shot->deferSubmittedFrame(metadata, color, tone, gain, filmEnabled, filmLook);
            if (!claimed && !entry->shot->captureRequested()) {
                if (!entry->shot->hasDngCompletion()) entry->shot->failLensShadingTimeout("raw_snapshot_failed");
                handOff(entry);
            }
            return claimed;
        }
        return false;
    }
    // Bounded: a stuck preparation fails through advance()'s timeout instead of stalling ingress.
    static constexpr std::chrono::milliseconds kPreparationWait{1500};
    static void waitPrepared(const Entry& entry, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!entry.prepared.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    bool beginDeferredSnapshot(AImage* imageLease, AHardwareBuffer* ahb, uint64_t timestamp, int fence) {
        for (const auto& entry : entries()) {
            if (entry->handedOff.load() || !entry->prepared.load()) continue;
            if (!entry->shot->expectsTimestamp(timestamp)) continue;
            const bool owned = entry->shot->beginDeferredSnapshot(imageLease, ahb, timestamp, fence);
            if (owned) handOff(entry);
            return owned;
        }
        return false;
    }
    std::string pollDngCompletion() {
        advance();
        return dngBox_.poll();
    }
    std::string pollJpegCompletion() {
        advance();
        return jpegBox_.poll();
    }
    bool detachedHqWorkActive() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return !queue_.empty();
    }
    void cancelAcquisition(const std::string& reason) {
        for (const auto& entry : entries()) {
            if (entry->handedOff.load()) continue;
            if (entry->preparation.joinable()) entry->preparation.join();
            entry->shot->cancelAcquisition(reason);
            handOff(entry);
        }
    }
    void cancelPendingDngBeforeIngressDestroy() { cancelAcquisition("camera_ingress_destroyed"); }
    void resetHqProcessing() noexcept {
        // Session reconfiguration must not discard jobs already owning their RAW.
        // Device destruction uses shutdown(), which drains before Vulkan teardown.
    }
    void shutdown() noexcept {
        if (stopped_) return;
        cancelAcquisition("camera_shutdown_before_raw");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        changed_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    void run() {
        setpriority(PRIO_PROCESS, 0, 5);
        for (;;) {
            std::shared_ptr<Entry> entry;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] {
                    return (stopped_ && queue_.empty()) || (!queue_.empty() && queue_.front()->handedOff.load());
                });
                if (queue_.empty()) return;
                entry = queue_.front();
            }
            if (entry->preparation.joinable()) entry->preparation.join();
            if (diagnostic_)
                diagnostic_("STILL_QUEUE_JOB_STARTED requestId=" + std::to_string(entry->id) + " waitMs=" +
                            std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                                     entry->accepted)
                                               .count()));
            // Single and multiframe development share a memory-heavy lane; acquisition
            // and preview do not take this lock. Child GPU/encoder threads inherit nice.
            std::unique_lock<std::mutex> processing(vulkan_.stillProcessingMutex());
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::StillProcessingBegin,
                                                                 0, entry->id);
            struct TraceEnd {
                uint64_t id;
                ~TraceEnd() {
                    diagnostics::RuntimeTraceRecorder::instance().record(
                        diagnostics::RuntimeTraceStage::StillProcessingEnd, 0, id);
                }
            } traceEnd{entry->id};
            if (!entry->recoveryPath.empty()) {
                try {
                    if (entry->recoverMultiframe) {
                        auto work = persistence::loadBurst(entry->recoveryPath, vulkan_, submitMutex_);
                        work->requestId = entry->id;
                        work->jpegRequested = entry->jpegRequested;
                        work->baseDng.outputFd = std::move(entry->recoveryDng);
                        work->mergedDng.outputFd = std::move(entry->recoveryMerged);
                        work->mergedJpeg.output.outputFd = std::move(entry->recoveryJpeg);
                        const auto geometry = work->capture->referenceMetadata.cameraContext;
                        rawr::raw_gpu_pipeline::AndroidBurstCoordinator coordinator;
                        // Crash-recovery path: function-local processor, cold
                        // per capture (today's behavior verbatim), never the
                        // persistent member regardless of the toggle.
                        develop::rendered::StillImageRenderer recoveryRendered(filesDir_);
                        multiframe::MfsrCaptureJob::Context context{
                            const_cast<rawrcam::vulkan::VulkanContext&>(vulkan_),
                            submitMutex_,
                            filesDir_,
                            work->capture->frames.front().raw.ref.width,
                            work->capture->frames.front().raw.ref.height,
                            geometry->rawPreviewCfa,
                            coordinator,
                            recoveryRendered,
                            false,
                            dngBox_,
                            jpegBox_,
                            assetManager_};
                        multiframe::MfsrCaptureJob(context, std::move(work)).run();
                        std::lock_guard<std::mutex> lock(mutex_);
                        popFrontLocked();
                        continue;
                    }
                    auto saved = persistence::load(entry->recoveryPath);
                    entry->shot = std::make_unique<SingleFrameCaptureJob>(vulkan_, submitMutex_, filesDir_, diagnostic_);
                    auto& shot = *entry->shot;
                    shot.recover(std::move(saved), entry->recoveryPath, entry->id, std::move(entry->recoveryDng),
                                 std::move(entry->recoveryJpeg),
                                 entry->jpegRequested, assetManager_);
                } catch (const std::exception& e) {
                    if (entry->recoveryDng.valid())
                        dngBox_.push(std::to_string(entry->id) + "\t0\t" + entry->dngName + "\t" + e.what());
                    if (entry->recoveryMerged.valid())
                        dngBox_.push(std::to_string(entry->id) + "\t0\tmerged\t" + e.what());
                    if (entry->jpegRequested) jpegBox_.push(std::to_string(entry->id) + "\t0\t\t" + e.what() + "\t0");
                    entry->recoveryDng.reset();
                    entry->recoveryMerged.reset();
                    entry->recoveryJpeg.reset();
                    std::lock_guard<std::mutex> lock(mutex_);
                    popFrontLocked();
                    continue;
                }
            }
            auto& shot = *entry->shot;
            bool dngPublished = !entry->dngRequested, jpegPublished = !entry->jpegRequested;
            std::string failure;
            try {
                do {
                    shot.advance();
                    auto dng = shot.pollDngCompletion();
                    if (!dng.empty() && entry->dngRequested) {
                        dngBox_.push(std::move(dng));
                        dngPublished = true;
                    }
                    auto jpeg = shot.pollJpegCompletion();
                    if (!jpeg.empty()) {
                        jpegBox_.push(std::move(jpeg));
                        jpegPublished = true;
                    }
                    if (!shot.detachedHqWorkActive()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } while (true);
            } catch (const std::exception& error) {
                failure = error.what();
            } catch (...) {
                failure = "still_processing_failed";
            }
            if (failure.empty()) failure = "still_finished_without_completion";
            if (!dngPublished) dngBox_.push(std::to_string(entry->id) + "\t0\t" + entry->dngName + "\t" + failure);
            if (!jpegPublished)
                jpegBox_.push(std::to_string(entry->id) + "\t0\t" + entry->jpegName + "\t" + failure + "\t0");
            // Destroy allocations on this worker before allowing the next job to start.
            entry->shot.reset();
            if (diagnostic_) diagnostic_("STILL_QUEUE_JOB_FINISHED requestId=" + std::to_string(entry->id));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                popFrontLocked();
            }
        }
    }

    AAssetManager* assetManager_ = nullptr;
    const rawrcam::vulkan::VulkanContext& vulkan_;
    std::mutex& submitMutex_;
    std::string filesDir_;
    Diagnostic diagnostic_;
    uint32_t width_ = 0, height_ = 0;
    uint64_t generation_ = 0, nextId_ = 1;
    bool disabled_ = false;
    bool stopped_ = false;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<Entry>> queue_;
    multiframe::StringMailbox dngBox_, jpegBox_;
    std::thread worker_;
};

SingleFrameCoordinator::SingleFrameCoordinator(const rawrcam::vulkan::VulkanContext& vulkanContext,
                                               std::mutex& queueSubmitMutex, std::string filesDir,
                                               Diagnostic diagnostic)
    : impl_(std::make_unique<Impl>(vulkanContext, queueSubmitMutex, std::move(filesDir), std::move(diagnostic))) {}

void SingleFrameCoordinator::setFilmAssetManager(AAssetManager* assetManager) { impl_->assetManager_ = assetManager; }

SingleFrameCoordinator::~SingleFrameCoordinator() = default;

void SingleFrameCoordinator::configureRawSnapshot(uint32_t width, uint32_t height, uint64_t cameraGeneration) {
    impl_->configureRawSnapshot(width, height, cameraGeneration);
}
void SingleFrameCoordinator::disableRawSnapshot(const std::string& reason) { impl_->disableRawSnapshot(reason); }
bool SingleFrameCoordinator::rawSnapshotConfigured() const noexcept { return impl_->rawSnapshotConfigured(); }
bool SingleFrameCoordinator::captureRequested() const noexcept { return impl_->captureRequested(); }
uint64_t SingleFrameCoordinator::requestRawStillCapture(encoding::dng::DngCaptureContext dngContext,
                                                        rawrcam::capture::JpegCaptureRequest jpegContext) {
    return impl_->requestRawStillCapture(std::move(dngContext), std::move(jpegContext));
}

void SingleFrameCoordinator::expectOptimizedFrame(uint64_t requestId, std::chrono::milliseconds extraWait) {
    impl_->expectOptimizedFrame(requestId, extraWait);
}
SingleMatchedFrameResult SingleFrameCoordinator::processMatchedFrame(
    AImage* image, AHardwareBuffer* ahb, int acquireFenceFd, const metadata::FrameMetadataSnapshot& metadata,
    const color::FrameColorTransform& colorState, const tonemap::TonemapParams& tonemapParams, float aePostGain) {
    return impl_->processMatchedFrame(image, ahb, acquireFenceFd, metadata, colorState, tonemapParams, aePostGain);
}
bool SingleFrameCoordinator::deferSubmittedFrame(const metadata::FrameMetadataSnapshot& metadata,
                                                 const color::FrameColorTransform& colorState,
                                                 const tonemap::TonemapParams& tonemapParams, float aePostGain,
                                                 bool filmEnabled, const spektrafilm_native::FilmLook& filmLook) {
    return impl_->deferSubmittedFrame(metadata, colorState, tonemapParams, aePostGain, filmEnabled, filmLook);
}
bool SingleFrameCoordinator::beginDeferredSnapshot(AImage* imageLease, AHardwareBuffer* ahb, uint64_t timestampNs,
                                                   int fence) {
    return impl_->beginDeferredSnapshot(imageLease, ahb, timestampNs, fence);
}
void SingleFrameCoordinator::advance() { impl_->advance(); }
void SingleFrameCoordinator::advanceDng() { impl_->advanceDng(); }
void SingleFrameCoordinator::advanceHq() { impl_->advanceHq(); }
std::string SingleFrameCoordinator::pollDngCompletion() { return impl_->pollDngCompletion(); }
std::string SingleFrameCoordinator::pollJpegCompletion() { return impl_->pollJpegCompletion(); }
bool SingleFrameCoordinator::detachedHqWorkActive() const noexcept { return impl_->detachedHqWorkActive(); }
void SingleFrameCoordinator::cancelPendingDngBeforeIngressDestroy() { impl_->cancelPendingDngBeforeIngressDestroy(); }
void SingleFrameCoordinator::resetHqProcessing() noexcept { impl_->resetHqProcessing(); }
void SingleFrameCoordinator::shutdown() noexcept { impl_->shutdown(); }

}  // namespace rawrcam::capture

uint64_t rawrcam::capture::SingleFrameCoordinator::recover(const std::string& name, bool multiframe, int dng,
                                                           int merged, int jpeg) {
    return impl_->recover(name, multiframe, dng, merged, jpeg);
}
