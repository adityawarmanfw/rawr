#pragma once
#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>
#include <rawr/raw_gpu_pipeline/RextureFrameScorer.h>
#include <spektrafilm/SpektraFilm.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <raw_sharpness/raw_sharpness.hpp>
#include <string>
#include <thread>

#include "capture/CaptureRequest.h"
#include "capture/multiframe/MultiframeBaseFrame.h"
#include "capture/multiframe/MultiframeTuning.h"
#include "capture/multiframe/MultiframeWorkItem.h"
#include "capture/multiframe/SharedCompletionMailbox.h"
#include "develop/render/StillImageRenderer.h"
#include "encoding/dng/DngCaptureContext.h"
#include "encoding/jpeg/JpegCaptureContext.h"
#include "vulkan/VulkanContext.h"

struct AAssetManager;

namespace rawrcam::capture::multiframe {

// Owns the background burst lifecycle: warmup, worker thread + queue,
// completion mailboxes, and the GPU burst coordinator. Previously scattered
// across FrameSubmitCoordinator (threads, mutexes, deques, warmup).
// The realtime coordinator keeps only frame pairing/submit and the live
// MultiframeFrameRing; shutter calls below preserve the two-phase prepare→start
// contract (prepare returns the exact frozen count for MediaStore naming).
class MfsrCaptureService {
   public:
    MfsrCaptureService(rawrcam::vulkan::VulkanContext& vulkan, std::mutex& queue0Mutex, std::string filesDir);
    ~MfsrCaptureService();

    MfsrCaptureService(const MfsrCaptureService&) = delete;
    MfsrCaptureService& operator=(const MfsrCaptureService&) = delete;

    struct Geometry {
        std::uint32_t rawWidth = 0;
        std::uint32_t rawHeight = 0;
        std::uint32_t cfa = 0;
    };

    // (Re)configure for a camera session: reset worker state and kick a
    // delayed warmup of burst pipelines + arena. Safe to call repeatedly.
    void configure(const Geometry& geometry, bool warmEnabled);
    void reset() noexcept;

    bool isWorkerActive() const noexcept { return workerActive_; }
    // Bursts not yet saved: they may still pin camera images (full-size bursts) or wait for
    // post-shutter bracket frames. Once saved, merging needs no camera.
    bool spoolingActive() const noexcept { return spooling_.load(std::memory_order_acquire) != 0; }

    // Enqueue frozen capture for background processing. Returns request id,
    // or 0 when storage admission fails. The service closes FDs on rejection.
    std::uint64_t start(std::unique_ptr<PendingMultiframeCapture> capture,
                        rawrcam::encoding::dng::DngCaptureContext baseDng,
                        rawrcam::encoding::dng::DngCaptureContext mergedDng,
                        rawrcam::capture::JpegCaptureRequest mergedJpeg, bool dumpRzslRequested,
                        tonemap::TonemapParams frozenTonemap, rawrcam::capture::multiframe::MultiframeTuning tuning,
                        rawrcam::capture::multiframe::MultiframeBaseFrameMode baseFrameMode, bool filmEnabled,
                        const spektrafilm_native::FilmLook& filmLook);

    std::string pollDngCompletion();
    std::string pollJpegCompletion();
    // APK asset access for the merged-still film engine's tables. Safe any time.
    void setFilmAssetManager(AAssetManager* assetManager) noexcept { filmAssetManager_ = assetManager; }
    // Persistent render engines toggle (Settings > Experimental > Persistent
    // Engine). Safe any time; takes effect on the next capture at the latest.
    // Default off preserves per-capture rebuilds exactly.
    void setPersistentEngineEnabled(bool enabled) noexcept {
        persistentEngineEnabled_ = enabled;
        renderedStill_.setEnginePersistenceEnabled(enabled);
    }

   private:
    void stopWorker() noexcept;
    void stopWarm() noexcept;

    rawrcam::vulkan::VulkanContext& vulkan_;
    std::mutex& queue0Mutex_;
    std::string filesDir_;
    Geometry geometry_{};

    rawr::raw_gpu_pipeline::AndroidBurstCoordinator burstCoordinator_;
    // Sharpest-reference scorer (native/multiframe/sharpness backend):
    // geometry-independent (fixed partial buffer), valid for the service
    // lifetime (device outlives it). Initialized either by the delayed
    // warmup or lazily on first Sharpest use; sharpnessMutex_ guards the
    // check+init window only. score() itself needs no lock (no mutation,
    // reset is never called). Destroyed after all threads join.
    raw_sharpness::RawSharpness sharpness_;
    std::mutex sharpnessMutex_;
    // Rexture base selection at the press (own pipeline, independent of the merge coordinator); guarded by sharpnessMutex_.
    rawr::raw_gpu_pipeline::RextureFrameScorer rextureScorer_;
    std::mutex queueMutex_;
    AAssetManager* filmAssetManager_ = nullptr;
    bool persistentEngineEnabled_ = false;
    // Persistent render processor for the live path (toggle-gated). When the
    // toggle is off, run() uses a function-local instead, preserving today's
    // per-capture lifecycle bit-for-bit. Declared after filesDir_ for init
    // order; destroyed after the worker/spool threads join.
    develop::rendered::StillImageRenderer renderedStill_;
    struct SpoolingWork {
        std::unique_ptr<MultiframeWorkItem> work;
        std::shared_ptr<void> reservation;
        std::string path, error;
        std::mutex mutex;
        std::condition_variable readyCondition;
        bool ready = false;  // the spool thread is done with this capture (journal durable, or failed)
        // Live merge (Rexture): the base is chosen and the header copied, so the worker may start on the live
        // frames while the journal is still being written. durable: the journal no longer reads the frames.
        bool selected = false;
        bool live = false;
        std::atomic<bool> durable{false};
    };
    void spoolLoop();
    std::deque<std::shared_ptr<SpoolingWork>> queuedWork_, spoolQueue_;
    std::mutex spoolMutex_;
    std::condition_variable spoolChanged_;
    bool spoolStopped_ = false;
    std::atomic<std::uint32_t> spooling_{0};
    std::thread spoolThread_;
    StringMailbox dngBox_;
    StringMailbox jpegBox_;
    // Guards submits to the background multiframe queue (queue 0 keeps its
    // own queueSubmitMutex_). Still and preview-worker bursts share this queue.
    std::thread workerThread_;
    std::thread warmThread_;
    std::atomic<std::uint64_t> warmGeneration_{0};
    std::atomic<bool> workerActive_{false};
    std::uint64_t nextRequestId_ = (std::uint64_t{1} << 62u);
};

}  // namespace rawrcam::capture::multiframe
