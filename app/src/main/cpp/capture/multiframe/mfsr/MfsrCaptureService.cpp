#include "capture/multiframe/mfsr/MfsrCaptureService.h"

#include <android/log.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <raw_sharpness/raw_sharpness_types.hpp>
#include <rawr/raw_merge_rexture_gpu/BaseSelection.h>
#include <stdexcept>

#include "capture/CaptureRequest.h"
#include "capture/multiframe/BracketCollector.h"
#include "capture/multiframe/BracketExposure.h"
#include "capture/multiframe/MergeArenaEstimate.h"
#include "capture/multiframe/mfsr/MfsrCaptureJob.h"
#include "capture/persistence/AcquiredRawJournal.h"
#include "capture/persistence/CaptureJob.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"

namespace rawrcam::capture::multiframe {
namespace {
constexpr const char* kTag = "RawrCamNative";
// Post-shutter bracket frames normally land within ~150 ms; past this the
// burst merges without (the spool keeps the ZSL pins meanwhile).
// 16 after-shutter frames at 1/15 s take ~1.1 s plus pipeline latency.
constexpr auto kBracketWait = std::chrono::milliseconds(2500);
constexpr std::size_t kMaxBurstFrames = 30u;  // capture journal limit
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
    // An arena is laid out for the whole frame, not for the tuned 12.5 MP
    // geometry: Wronski is ~1.26 GiB at 12.5 MP but ~5.05 GiB at 50 MP and
    // ~20.2 GiB at 200 MP (HDR+ Quality: ~0.77 / ~3.08 / ~12.3 GiB). Reserving
    // that while the camera is open starves the still render and gets the
    // process killed, so only warm geometries whose worst-case arena fits.
    // A shutter before/without warm falls back to cold initialization.
    constexpr std::uint64_t kMaxWarmArenaBytes = 1536ull * 1024u * 1024u;  // 1.5 GiB
    const std::uint64_t arenaBytes = estimateMergeArenaBytes(geometry.rawWidth, geometry.rawHeight,
                                                                 burstCoordinator_.nextAlgorithmId());
    LOGI("MULTIFRAME_WARM_PLAN raw=%ux%u arena1xMaxMB=%.0f budgetMB=%.0f", geometry.rawWidth, geometry.rawHeight,
         double(arenaBytes) / (1024.0 * 1024.0), double(kMaxWarmArenaBytes) / (1024.0 * 1024.0));
    if (arenaBytes == 0 || arenaBytes > kMaxWarmArenaBytes) {
        LOGI("MULTIFRAME_WARM_SKIP raw=%ux%u arena1xMaxMB=%.0f reason=%s", geometry.rawWidth, geometry.rawHeight,
             double(arenaBytes) / (1024.0 * 1024.0), arenaBytes ? "arena_too_large" : "estimate_unavailable");
        return;
    }
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
    // A rejected request closes its destinations as the contexts go out of scope.
    const bool jpegRequested = mergedJpeg.output.outputFd.valid();
    if (!capture || (!baseDng.outputFd.valid() && !mergedDng.outputFd.valid() && !jpegRequested) ||
        (jpegRequested && (mergedJpeg.output.quality < 95 || mergedJpeg.output.quality > 100))) {
        return 0u;
    }

    const std::size_t reservedFrames =
        capture->frames.size() + (capture->pendingBracket ? capture->pendingBracket->expected() : 0u);
    auto reservation = capture->acquiredJournal
                           ? capture->acquiredJournal->reservation()
                           : persistence::reserve(filesDir_,
                                                  uint64_t(capture->frames.front().raw.ref.width) *
                                                      capture->frames.front().raw.ref.height * 2 * reservedFrames,
                                                  false);
    if (!reservation) return 0;
    const uint64_t requestId = nextRequestId_++;
    auto work = std::make_unique<MultiframeWorkItem>();
    work->requestId = requestId;
    work->acceptedAt = std::chrono::steady_clock::now();
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
        spooling_.fetch_add(1, std::memory_order_acq_rel);
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
                // Rexture merges the live frames as soon as the base is chosen, while the spool thread still writes
                // the journal; every other merge waits for the durable journal and reloads from it.
                bool live = false;
                {
                    std::unique_lock<std::mutex> lock(current->mutex);
                    current->readyCondition.wait(lock, [&] { return current->ready || current->selected; });
                    live = current->live;
                }
                MultiframeWorkItem* originalItem = current->work.get();
                auto& original = *originalItem;
                // Kept for the failure report once the work item belongs to the job.
                const std::uint64_t liveRequestId = original.requestId;
                const bool liveJpegRequested = original.jpegRequested;
                const std::string liveJpegName = original.mergedJpeg.output.displayName;
                try {
                    if (live) {
                        SpoolingWork* spool = current.get();
                        original.awaitDurable = [spool] {
                            std::unique_lock<std::mutex> lock(spool->mutex);
                            spool->readyCondition.wait(lock, [&] { return spool->ready; });
                        };
                        original.capture->pinsMayRelease = [spool] {
                            return spool->durable.load(std::memory_order_acquire);
                        };
                        std::lock_guard<std::mutex> processing(vulkan_.stillProcessingMutex());
                        diagnostics::RuntimeTraceRecorder::instance().record(
                            diagnostics::RuntimeTraceStage::StillProcessingBegin, 0, liveRequestId, -1, 0, 0, 1u);
                        struct TraceEnd {
                            uint64_t id;
                            ~TraceEnd() {
                                diagnostics::RuntimeTraceRecorder::instance().record(
                                    diagnostics::RuntimeTraceStage::StillProcessingEnd, 0, id, -1, 0, 0, 1u);
                            }
                        } traceEnd{liveRequestId};
                        LOGI("BURST_LIVE requestId=%llu acceptedToStartMs=%.1f",
                             static_cast<unsigned long long>(liveRequestId),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - original.acceptedAt)
                                 .count());
                        MfsrCaptureJob::Context ctx{vulkan_,
                                                    queue0Mutex_,
                                                    filesDir_,
                                                    original.capture->frames.front().raw.ref.width,
                                                    original.capture->frames.front().raw.ref.height,
                                                    original.capture->referenceMetadata.cameraContext->rawPreviewCfa,
                                                    burstCoordinator_,
                                                    renderedStill_,
                                                    persistentEngineEnabled_,
                                                    dngBox_,
                                                    jpegBox_,
                                                    filmAssetManager_};
                        const auto acceptedAt = original.acceptedAt;
                        originalItem = nullptr;  // the job owns the work item from here
                        MfsrCaptureJob(ctx, std::move(current->work)).run();
                        LOGI("BURST_FINISHED requestId=%llu acceptedToFinishedMs=%.1f live=1",
                             static_cast<unsigned long long>(liveRequestId),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acceptedAt).count());
                    } else {
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
                    // Frames load from the journal as the merge reaches them: single-pass
                    // merges (super-resolution, HDR+) hold the reference plus one
                    // companion and drop each once merged; HDR+ Quality's four passes
                    // load the whole burst first. The RZSL dump reads every frame.
                    auto restored =
                        persistence::loadBurst(current->path, vulkan_, queue0Mutex_, !original.dumpRzslRequested);
                    restored->acceptedAt = original.acceptedAt;
                    LOGI("BURST_READY requestId=%llu acceptedToReadyMs=%.1f",
                         static_cast<unsigned long long>(original.requestId),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - original.acceptedAt).count());
                    restored->baseDng.outputFd = std::move(original.baseDng.outputFd);
                    restored->mergedDng.outputFd = std::move(original.mergedDng.outputFd);
                    restored->mergedJpeg.output.outputFd = std::move(original.mergedJpeg.output.outputFd);
                    restored->dumpRzslRequested = original.dumpRzslRequested;
                    // Not part of the journal: carried over from the live request.
                    restored->mergedJpeg.develop.multiframeNativeRgb = original.mergedJpeg.develop.multiframeNativeRgb;
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
                    LOGI("BURST_FINISHED requestId=%llu acceptedToFinishedMs=%.1f",
                         static_cast<unsigned long long>(original.requestId),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - original.acceptedAt).count());
                    }
                } catch (const std::exception& e) {
                    if (!originalItem) {
                        // The live job owns the work item (and any outputs it still holds): report the JPEG only.
                        if (liveJpegRequested)
                            jpegBox_.push(std::to_string(liveRequestId) + "\t0\t" + liveJpegName + "\t" + e.what() + "\t0");
                    } else {
                    // Outputs still held here were never handed to the job, which publishes its own.
                    const bool baseHeld = original.baseDng.outputFd.valid();
                    const bool mergedHeld = original.mergedDng.outputFd.valid();
                    original.baseDng.outputFd.reset();
                    original.mergedDng.outputFd.reset();
                    original.mergedJpeg.output.outputFd.reset();
                    auto prefix = std::to_string(original.requestId) + "\t0\t";
                    if (baseHeld) dngBox_.push(prefix + original.baseDng.displayName + "\t" + e.what());
                    if (mergedHeld)
                        dngBox_.push(prefix + original.mergedDng.displayName + "\t" + e.what());
                    if (original.jpegRequested)
                        jpegBox_.push(prefix + original.mergedJpeg.output.displayName + "\t" + e.what() + "\t0");
                    }
                }
                {
                    // The spool thread may still be finishing this capture's journal (live merge): it owns `current`'s
                    // storage reservation, so do not drop the last reference before it is done.
                    std::unique_lock<std::mutex> lock(current->mutex);
                    current->readyCondition.wait(lock, [&] { return current->ready; });
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
        pending->work->baseDng.outputFd.reset();
        pending->work->mergedDng.outputFd.reset();
        pending->work->mergedJpeg.output.outputFd.reset();
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
        bool liveRun = false;
        try {
            // Deferred sharpest-reference selection, entirely on the GPU: one
            // small compute scoring pass over the pinned burst images, then a
            // ~30KB partial readback. No bulk RAW copies; the save below keeps
            // its single read-back pass. Middle mode skips this with zero cost.
            // Any failure keeps the frozen middle reference.
            auto& burst = *pending->work->capture;
            // HDR+ bracketed: append the post-shutter long frames (whatever
            // arrived by the deadline; none = a uniform merge of the
            // underexposed burst, still lifted back).
            if (auto collector = std::move(burst.pendingBracket)) {
                const auto begin = std::chrono::steady_clock::now();
                auto taken = collector->take(begin + kBracketWait);
                std::size_t appended = 0;
                for (auto& f : taken) {
                    if (burst.frames.size() >= kMaxBurstFrames || f.frames.size() != 1u || f.refs.size() != 1u ||
                        f.metadata.size() != 1u || f.colors.size() != 1u || !f.snapshot ||
                        f.frames.front().raw.ref.width != burst.frames.front().raw.ref.width ||
                        f.frames.front().raw.ref.height != burst.frames.front().raw.ref.height)
                        continue;
                    burst.extraSnapshots.push_back(std::move(f.snapshot));
                    burst.refs.push_back(f.refs.front());
                    burst.frames.push_back(f.frames.front());
                    burst.metadata.push_back(std::move(f.metadata.front()));
                    burst.colors.push_back(std::move(f.colors.front()));
                    ++appended;
                }
                LOGI("MULTIFRAME_BRACKET_COLLECT request=%llu expected=%u appended=%zu waitMs=%.1f",
                     static_cast<unsigned long long>(collector->requestId()), collector->expected(), appended,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
            }
            // CPU-held frames (full-size bursts) have nothing on the GPU to score:
            // they keep the middle reference.
            if (pending->work->baseFrameMode == MultiframeBaseFrameMode::Sharpest &&
                pending->work->tuning.mergeAlgorithm != kMergeAlgorithmRexture && !burst.cpuFrame &&
                !burst.acquiredJournal && burst.frames.size() >= 2u &&
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
            // Rexture: the base is chosen at the press, from the frames of the last quarter second: one batched scoring
            // dispatch (sharpness above the noise expectation, clipped share), then the best score penalised for clipping and
            // age. Any failure keeps the frozen middle reference.
            if (pending->work->tuning.mergeAlgorithm == kMergeAlgorithmRexture && !burst.cpuFrame && !burst.acquiredJournal &&
                burst.frames.size() >= 2u && burst.frames.size() <= 32u && burst.metadata.size() == burst.frames.size() &&
                burst.colors.size() == burst.frames.size()) {
                try {
                    const auto begin = std::chrono::steady_clock::now();
                    std::vector<rawr::raw_gpu_pipeline::RextureFrameScorer::Score> scores;
                    {
                        std::lock_guard<std::mutex> scoreLock(sharpnessMutex_);
                        if (!rextureScorer_.ready())
                            rextureScorer_.initialize(vulkan_.physicalDevice(), vulkan_.device(), vulkan_.queueFamily(),
                                                      [&](const VkSubmitInfo& info, VkFence fence) {
                                                          std::lock_guard<std::mutex> queueLock(queue0Mutex_);
                                                          if (vkQueueSubmit(vulkan_.queue(), 1, &info, fence) != VK_SUCCESS)
                                                              throw std::runtime_error("rexture score queue submit");
                                                      });
                        const auto& ref0 = burst.metadata.front();
                        // Normalised noise model from Camera2 (S, O per CFA site): a = S / range, b = (S black + O) / range^2.
                        float alpha = 1.80710882e-4f, beta = 3.1937599182128e-6f;
                        if (ref0.sensorNoiseProfile.size() >= 8u && ref0.effectiveWhiteLevel > 0.0f) {
                            double a = 0, b = 0;
                            for (int site = 0; site < 4; ++site) {
                                const double black = ref0.blackLevelPhysicalRggb[site];
                                const double range = double(ref0.effectiveWhiteLevel) - black;
                                a += ref0.sensorNoiseProfile[2 * site] / range;
                                b += (ref0.sensorNoiseProfile[2 * site] * black + ref0.sensorNoiseProfile[2 * site + 1]) /
                                     (range * range);
                            }
                            if (a > 0.0 && b >= 0.0) alpha = float(a / 4.0), beta = float(b / 4.0);
                        }
                        const std::uint32_t cfa = ref0.cameraContext ? ref0.cameraContext->rawPreviewCfa : 0u;
                        scores = rextureScorer_.score(burst.frames, burst.frames.front().raw.ref.width,
                                                      burst.frames.front().raw.ref.height, cfa <= 3u ? cfa : 0u, alpha, beta);
                    }
                    std::vector<rawr::raw_merge_rexture_gpu::FrameScore> frameScores;
                    std::vector<double> times;
                    for (std::size_t i = 0; i < scores.size(); ++i) {
                        frameScores.push_back({scores[i].sharp, scores[i].sat});
                        times.push_back(double(burst.metadata[i].timestampNs) * 1.0e-9);
                    }
                    // The burst is frozen at the press: the newest frame is the press time.
                    const double press = *std::max_element(times.begin(), times.end());
                    const auto best = static_cast<std::uint32_t>(rawr::raw_merge_rexture_gpu::chooseBaseAtPress(frameScores, times, press));
                    for (std::size_t i = 0; i < scores.size(); ++i) {
                        const double age = std::max(press - times[i], 0.0);
                        const bool inWindow = age <= rawr::raw_merge_rexture_gpu::kBaseWindowSeconds;
                        const double value = scores[i].sharp *
                                             std::max(1.0 - rawr::raw_merge_rexture_gpu::kBaseSaturationPenalty * scores[i].sat, 0.0) *
                                             std::max(1.0 - rawr::raw_merge_rexture_gpu::kBaseTimePenalty * age /
                                                                 rawr::raw_merge_rexture_gpu::kBaseWindowSeconds, 0.0);
                        LOGI("MULTIFRAME_REXTURE_SCORE frame=%zu sharp=%.6g sat=%.5f ageMs=%.1f inWindow=%d value=%.6g%s", i,
                             scores[i].sharp, scores[i].sat, age * 1000.0, inWindow ? 1 : 0, value,
                             i == best ? " <- base" : "");
                    }
                    burst.referenceIndex = best;
                    burst.referenceMetadata = burst.metadata[best];
                    burst.referenceColor = burst.colors[best];
                    LOGI("MULTIFRAME_BASEFRAME mode=rexture-press selected=%u/%zu scoreMs=%.1f", best, burst.frames.size(),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
                } catch (const std::exception& e) {
                    LOGE("MULTIFRAME_REXTURE_SCORE_FALLBACK %s", e.what());
                } catch (...) {
                    LOGE("MULTIFRAME_REXTURE_SCORE_FALLBACK unknown");
                }
            }
            // Bracketed bursts merge onto the darkest frame (sharpest among
            // those when scored), whatever the base frame mode: the
            // underexposed ZSL burst, not the long frames.
            if (pending->work->tuning.mergeAlgorithm == kMergeAlgorithmHdrPlusBracketed &&
                burst.metadata.size() == burst.frames.size() && burst.colors.size() == burst.frames.size() &&
                isExposureBracketed(burst.metadata)) {
                const auto reference =
                    bracketReference(burst.metadata, pending->work->sharpnessScores, burst.referenceIndex);
                burst.referenceIndex = reference;
                burst.referenceMetadata = burst.metadata[reference];
                burst.referenceColor = burst.colors[reference];
                LOGI("MULTIFRAME_BASEFRAME mode=bracket-darkest selected=%u/%zu", reference, burst.frames.size());
            }
            const auto requestId = pending->work->requestId;
            const bool live = pending->work->tuning.mergeAlgorithm == kMergeAlgorithmRexture && burst.snapshot &&
                              !burst.cpuFrame && !burst.acquiredJournal && !burst.pendingBracket &&
                              !pending->work->dumpRzslRequested;
            if (live) {
                // Everything the journal header needs is copied out of the work item first. From the moment the
                // worker is released it owns the work item and the live frames; this thread then only reads the
                // frames (GPU images the ring pins keep alive until the journal is durable) and never touches
                // pending->work again.
                auto journalJob = persistence::makeBurstJob(*pending->work);
                auto* liveBurst = pending->work->capture.get();
                {
                    std::lock_guard<std::mutex> lock(pending->mutex);
                    pending->live = true;
                    pending->selected = true;
                }
                liveRun = true;
                pending->readyCondition.notify_all();
                persistence::saveBurstFrames(pending->path, journalJob, *liveBurst, requestId, vulkan_, queue0Mutex_);
                pending->durable.store(true, std::memory_order_release);
            } else {
                persistence::saveBurst(pending->path, *pending->work, vulkan_, queue0Mutex_);
            }
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::StillDurable, 0,
                                                                 requestId, -1, 0, 0, 1u);
        } catch (const std::exception& e) {
            pending->error = e.what();
        }
        // Release live ring pins immediately after durable readback; a live merge's job owns them until it ends.
        if (!liveRun) pending->work->capture.reset();
        pending->reservation.reset();
        spooling_.fetch_sub(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->ready = true;
        }
        pending->readyCondition.notify_all();
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
