#include "capture/multiframe/mfsr/MfsrCaptureService.h"

#include <android/log.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <raw_sharpness/raw_sharpness_types.hpp>
#include <stdexcept>

#include "capture/CaptureRequest.h"
#include "capture/multiframe/mfsr/MfsrCaptureJob.h"
#include "capture/persistence/CaptureJob.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"

namespace rawrcam::capture::multiframe {
namespace {
constexpr const char* kTag = "RawrCamNative";
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)
}  // namespace

MfsrCaptureService::MfsrCaptureService(rawrcam::vulkan::VulkanContext& vulkan, std::mutex& queue0Mutex,
                                       std::string filesDir)
    : vulkan_(vulkan),
      queue0Mutex_(queue0Mutex),
      filesDir_(std::move(filesDir)),
      renderedStill_(filesDir_),
      spoolThread_([this] { spoolLoop(); }) {
    // Multiframe+film captures take ~1min end-to-end; the 30s default would
    // evict between every two shots and reuse could never trigger.
    renderedStill_.setFilmIdleEvictSeconds(300);
}

MfsrCaptureService::~MfsrCaptureService() {
    stopWorker();
    stopWarm();
    {
        std::lock_guard<std::mutex> lock(spoolMutex_);
        spoolStopped_ = true;
    }
    spoolChanged_.notify_one();
    if (spoolThread_.joinable()) spoolThread_.join();
    // Cached render engines and the sharpness scorer contain VkDevice handles.
    // The owner destroys this service before destroying VulkanContext.
    renderedStill_.setEnginePersistenceEnabled(false);
    renderedStill_.reset();
    renderedStill_.shutdownFilm();
    sharpness_.reset();
    burstCoordinator_.reset();
}

void MfsrCaptureService::configure(const Geometry& geometry, bool warmEnabled) {
    stopWorker();
    stopWarm();
    burstCoordinator_.reset();
    geometry_ = geometry;
    if (vulkan_.device() == VK_NULL_HANDLE || !warmEnabled) return;
    stopWarm();
    try {
        // Delayed start: compiling 26 pipelines + allocating the arena
        // during preview spin-up blanks the viewfinder on cold start
        // (startup already compiles preview's own pipelines). Wait until
        // preview is up; abort early if reconfigured meanwhile. A shutter
        // before the delay simply falls back to cold init.
        const std::uint64_t generation = warmGeneration_.load() + 1u;
        warmGeneration_.store(generation);
        const std::uint32_t rawW = geometry_.rawWidth, rawH = geometry_.rawHeight;
        warmThread_ = std::thread([this, generation, rawW, rawH] {
            for (int i = 0; i < 25; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (generation != warmGeneration_.load()) return;
            }
            try {
                const auto begin = std::chrono::steady_clock::now();
                // Pipelines + arena (last merge algorithm/scale): first burst
                // fully warm when geometry matches. ~1.3GB (Wronski) or
                // ~0.5GB (HDR+) held while the camera is open; reset() releases it.
                burstCoordinator_.warmAll(vulkan_.physicalDevice(), vulkan_.device(), vulkan_.queueFamily(), rawW,
                                          rawH);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                LOGI("MULTIFRAME_WARM arena+pipelines ready in %.1f ms", ms);
                // Warm the sharpness scorer pipeline alongside the burst
                // pipelines so the first Sharpest capture pays no compile.
                // ~30KB buffer held while the camera is open; negligible.
                try {
                    std::lock_guard<std::mutex> sharpnessLock(sharpnessMutex_);
                    if (!sharpness_.ready() && generation == warmGeneration_.load()) {
                        raw_sharpness::RawSharpnessCreateInfo createInfo{};
                        createInfo.physicalDevice = vulkan_.physicalDevice();
                        createInfo.device = vulkan_.device();
                        createInfo.queueFamily = vulkan_.queueFamily();
                        sharpness_.initialize(createInfo, [&](const VkSubmitInfo& info, VkFence fence) {
                            std::lock_guard<std::mutex> queueLock(queue0Mutex_);
                            if (vkQueueSubmit(vulkan_.queue(), 1, &info, fence) != VK_SUCCESS) {
                                throw std::runtime_error("sharpness queue submit");
                            }
                        });
                        LOGI("MULTIFRAME_WARM sharpness scorer ready");
                    }
                } catch (const std::exception& e) {
                    LOGE("MULTIFRAME_SHARPNESS_WARM_FAIL %s", e.what());
                } catch (...) {
                    LOGE("MULTIFRAME_SHARPNESS_WARM_FAIL unknown");
                }
            } catch (const std::exception& e) {
                LOGE("MULTIFRAME_WARM_FAIL %s", e.what());
            } catch (...) {
                LOGE("MULTIFRAME_WARM_FAIL unknown");
            }
        });
    } catch (const std::exception& e) {
        // Warmup must never break camera configuration; capture falls back
        // to cold init.
        LOGE("MULTIFRAME_WARM_START_FAIL %s", e.what());
    }
}

void MfsrCaptureService::reset() noexcept {
    stopWorker();
    stopWarm();
    burstCoordinator_.reset();
    // Clears per-capture leftovers; engines (including film) survive subject
    // to their match keys, mirroring single-frame resetHqProcessing.
    renderedStill_.reset();
    geometry_ = {};
}

std::uint64_t MfsrCaptureService::start(std::unique_ptr<PendingMultiframeCapture> capture,
                                        rawrcam::encoding::dng::DngCaptureContext baseDng,
                                        rawrcam::encoding::dng::DngCaptureContext mergedDng,
                                        rawrcam::capture::JpegCaptureRequest mergedJpeg, bool dumpRzslRequested,
                                        tonemap::TonemapParams frozenTonemap,
                                        rawrcam::capture::multiframe::MultiframeTuning tuning,
                                        rawrcam::capture::multiframe::MultiframeBaseFrameMode baseFrameMode,
                                        bool filmEnabled, const spektrafilm_native::FilmLook& filmLook) {
    const bool jpegRequested = mergedJpeg.output.outputFd >= 0;
    const int baseFd = baseDng.outputFd;
    const int mergedFd = mergedDng.outputFd;
    const int jpegFd = mergedJpeg.output.outputFd;
    auto closeOutputs = [&]() noexcept {
        if (baseFd >= 0) close(baseFd);
        if (mergedFd >= 0) close(mergedFd);
        if (jpegFd >= 0) close(jpegFd);
    };
    if (!capture || (baseFd < 0 && mergedFd < 0 && jpegFd < 0) ||
        (jpegRequested && (mergedJpeg.output.quality < 95 || mergedJpeg.output.quality > 100))) {
        closeOutputs();
        return 0u;
    }

    auto reservation = persistence::reserve(filesDir_,
                                            uint64_t(capture->frames.front().raw.ref.width) *
                                                capture->frames.front().raw.ref.height * 2 * capture->frames.size(),
                                            false);
    if (!reservation) {
        closeOutputs();
        return 0;
    }
    const uint64_t requestId = nextRequestId_++;
    auto work = std::make_unique<MultiframeWorkItem>();
    work->requestId = requestId;
    work->capture = std::move(capture);
    work->baseDng = std::move(baseDng);
    work->mergedDng = std::move(mergedDng);
    work->mergedJpeg = std::move(mergedJpeg);
    work->jpegRequested = jpegRequested;
    work->dumpRzslRequested = dumpRzslRequested;
    work->frozenTonemap = frozenTonemap;
    work->filmEnabled = filmEnabled;
    work->filmLook = filmLook;
    work->tuning = tuning;
    work->baseFrameMode = baseFrameMode;

    diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::StillAccepted, 0, requestId,
                                                         -1, 0, 0, 1u);
    auto pending = std::make_shared<SpoolingWork>();
    pending->path = persistence::jobPath(filesDir_, work->baseDng.displayName);
    pending->work = std::move(work);
    pending->reservation = std::move(reservation);
    {
        std::lock_guard<std::mutex> lock(spoolMutex_);
        spoolQueue_.push_back(pending);
    }
    spoolChanged_.notify_one();
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (workerActive_) {
            queuedWork_.push_back(pending);
            return requestId;
        }
        workerActive_ = true;
    }
    if (workerThread_.joinable()) workerThread_.join();
    try {
        workerThread_ = std::thread([this, current = pending]() mutable {
            setpriority(PRIO_PROCESS, 0, 5);
            while (current) {
                {
                    std::unique_lock<std::mutex> lock(current->mutex);
                    current->readyCondition.wait(lock, [&] { return current->ready; });
                }
                auto& original = *current->work;
                try {
                    if (!current->error.empty()) throw std::runtime_error(current->error);
                    std::lock_guard<std::mutex> processing(vulkan_.stillProcessingMutex());
                    diagnostics::RuntimeTraceRecorder::instance().record(
                        diagnostics::RuntimeTraceStage::StillProcessingBegin, 0, original.requestId, -1, 0, 0, 1u);
                    struct TraceEnd {
                        uint64_t id;
                        ~TraceEnd() {
                            diagnostics::RuntimeTraceRecorder::instance().record(
                                diagnostics::RuntimeTraceStage::StillProcessingEnd, 0, id, -1, 0, 0, 1u);
                        }
                    } traceEnd{original.requestId};
                    auto restored = persistence::loadBurst(current->path, vulkan_, queue0Mutex_);
                    restored->baseDng.outputFd = original.baseDng.outputFd;
                    restored->mergedDng.outputFd = original.mergedDng.outputFd;
                    restored->mergedJpeg.output.outputFd = original.mergedJpeg.output.outputFd;
                    restored->dumpRzslRequested = original.dumpRzslRequested;
                    original.baseDng.outputFd = original.mergedDng.outputFd = original.mergedJpeg.output.outputFd = -1;
                    MfsrCaptureJob::Context ctx{vulkan_,
                                                queue0Mutex_,
                                                filesDir_,
                                                restored->capture->frames.front().raw.ref.width,
                                                restored->capture->frames.front().raw.ref.height,
                                                restored->capture->referenceMetadata.cameraContext->rawPreviewCfa,
                                                burstCoordinator_,
                                                renderedStill_,
                                                persistentEngineEnabled_,
                                                dngBox_,
                                                jpegBox_,
                                                filmAssetManager_};
                    MfsrCaptureJob(ctx, std::move(restored)).run();
                } catch (const std::exception& e) {
                    for (int fd :
                         {original.baseDng.outputFd, original.mergedDng.outputFd, original.mergedJpeg.output.outputFd})
                        if (fd >= 0) close(fd);
                    auto prefix = std::to_string(original.requestId) + "\t0\t";
                    if (original.baseDng.outputFd >= 0)
                        dngBox_.push(prefix + original.baseDng.displayName + "\t" + e.what());
                    if (original.mergedDng.outputFd >= 0)
                        dngBox_.push(prefix + original.mergedDng.displayName + "\t" + e.what());
                    if (original.jpegRequested)
                        jpegBox_.push(prefix + original.mergedJpeg.output.displayName + "\t" + e.what() + "\t0");
                }
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (!queuedWork_.empty()) {
                    current = std::move(queuedWork_.front());
                    queuedWork_.pop_front();
                } else {
                    current.reset();
                    workerActive_ = false;
                }
            }
        });
        return requestId;
    } catch (...) {
        // Spooling was already accepted: wait before reclaiming its inputs/FDs.
        {
            std::unique_lock<std::mutex> lock(pending->mutex);
            pending->readyCondition.wait(lock, [&] { return pending->ready; });
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            workerActive_ = false;
        }
        closeOutputs();
        return 0;
    }
}

void MfsrCaptureService::spoolLoop() {
    setpriority(PRIO_PROCESS, 0, 5);
    for (;;) {
        std::shared_ptr<SpoolingWork> pending;
        {
            std::unique_lock<std::mutex> lock(spoolMutex_);
            spoolChanged_.wait(lock, [&] { return spoolStopped_ || !spoolQueue_.empty(); });
            if (spoolQueue_.empty()) return;
            pending = spoolQueue_.front();
            spoolQueue_.pop_front();
        }
        try {
            // Deferred sharpest-reference selection, entirely on the GPU: one
            // small compute scoring pass over the pinned burst images, then a
            // ~30KB partial readback. No bulk RAW copies; the save below keeps
            // its single read-back pass. Middle mode skips this with zero cost.
            // Any failure keeps the frozen middle reference.
            auto& burst = *pending->work->capture;
            if (pending->work->baseFrameMode == MultiframeBaseFrameMode::Sharpest && burst.frames.size() >= 2u &&
                burst.frames.size() <= raw_sharpness::RawSharpness::kMaxFrames) {
                try {
                    {
                        std::lock_guard<std::mutex> sharpnessLock(sharpnessMutex_);
                        if (!sharpness_.ready()) {
                            const auto begin = std::chrono::steady_clock::now();
                            raw_sharpness::RawSharpnessCreateInfo createInfo{};
                            createInfo.physicalDevice = vulkan_.physicalDevice();
                            createInfo.device = vulkan_.device();
                            createInfo.queueFamily = vulkan_.queueFamily();
                            sharpness_.initialize(createInfo, [&](const VkSubmitInfo& info, VkFence fence) {
                                std::lock_guard<std::mutex> queueLock(queue0Mutex_);
                                if (vkQueueSubmit(vulkan_.queue(), 1, &info, fence) != VK_SUCCESS) {
                                    throw std::runtime_error("sharpness queue submit");
                                }
                            });
                            const double ms =
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                                    .count();
                            LOGI("MULTIFRAME_SHARPNESS scorer ready in %.1f ms", ms);
                        }
                    }
                    std::vector<raw_sharpness::SharpnessFrame> sharpFrames;
                    sharpFrames.reserve(burst.frames.size());
                    for (std::size_t i = 0; i < burst.frames.size(); ++i) {
                        raw_sharpness::SharpnessFrame frame{};
                        frame.image = burst.frames[i].raw.image;
                        frame.view = burst.frames[i].raw.view;
                        frame.width = burst.frames[i].raw.ref.width;
                        frame.height = burst.frames[i].raw.ref.height;
                        std::uint32_t cfa = 0u;
                        float white = 65535.0f;
                        if (i < burst.metadata.size()) {
                            if (burst.metadata[i].cameraContext) {
                                cfa = burst.metadata[i].cameraContext->rawPreviewCfa;
                            }
                            white = burst.metadata[i].effectiveWhiteLevel;
                        }
                        frame.pattern = cfa <= 3u ? static_cast<raw_sharpness::BayerPattern>(cfa)
                                                  : raw_sharpness::BayerPattern::RGGB;
                        frame.whiteLevel = white;
                        sharpFrames.push_back(frame);
                    }
                    const auto scoreBegin = std::chrono::steady_clock::now();
                    const auto scores = sharpness_.score(sharpFrames);
                    pending->work->sharpnessMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scoreBegin)
                            .count();
                    // Retain for RZSL audit + exact replay, even if the
                    // selection below keeps middle on a degenerate result.
                    pending->work->sharpnessScores = scores;
                    const auto middle = static_cast<std::uint32_t>(burst.frames.size() / 2u);
                    const std::uint32_t best = raw_sharpness::reference::selectSharpest(scores, middle);
                    if (best < burst.frames.size() && best < burst.metadata.size() && best < burst.colors.size()) {
                        burst.referenceIndex = best;
                        burst.referenceMetadata = burst.metadata[best];
                        burst.referenceColor = burst.colors[best];
                        LOGI("MULTIFRAME_BASEFRAME mode=sharpest selected=%u/%zu middle=%u", best, burst.frames.size(),
                             middle);
                    }
                } catch (const std::exception& e) {
                    LOGE("MULTIFRAME_SHARPNESS_FALLBACK %s", e.what());
                } catch (...) {
                    LOGE("MULTIFRAME_SHARPNESS_FALLBACK unknown");
                }
            }
            persistence::saveBurst(pending->path, *pending->work, vulkan_, queue0Mutex_);
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::StillDurable, 0,
                                                                 pending->work->requestId, -1, 0, 0, 1u);
        } catch (const std::exception& e) {
            pending->error = e.what();
        }
        pending->work->capture.reset();  // Release live ring pins immediately after durable readback.
        pending->reservation.reset();
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->ready = true;
        }
        pending->readyCondition.notify_one();
    }
}

std::string MfsrCaptureService::pollDngCompletion() { return dngBox_.poll(); }
std::string MfsrCaptureService::pollJpegCompletion() { return jpegBox_.poll(); }

void MfsrCaptureService::stopWorker() noexcept {
    if (workerThread_.joinable()) workerThread_.join();
    std::lock_guard<std::mutex> lock(queueMutex_);
    queuedWork_.clear();
    workerActive_ = false;
}

void MfsrCaptureService::stopWarm() noexcept {
    ++warmGeneration_;
    if (warmThread_.joinable()) warmThread_.join();
}

}  // namespace rawrcam::capture::multiframe
