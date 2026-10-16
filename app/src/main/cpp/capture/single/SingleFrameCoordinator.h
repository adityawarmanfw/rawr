#pragma once
#include <chrono>
#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <tonemap/TonemapEngine.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "capture/CaptureRequest.h"
#include "capture/single/SingleCaptureResult.h"
#include "color/FrameColorTransform.h"
#include "encoding/dng/DngCaptureContext.h"
#include "encoding/jpeg/JpegCaptureContext.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::vulkan {
class VulkanContext;
}

struct AAssetManager;

namespace spektrafilm_native {
struct FilmLook;
}

namespace rawrcam::capture {

// Application-owned still-capture state machine.
//
// Owns a resource-bounded RAW acquisition queue and durable backlog, DNG/JPEG output branches, demosaic selection,
// post-demosaic WB/FCC/Tonemap stage, artifact A/B diagnostics, asynchronous completion
// progression, and still-capture request IDs. It deliberately does not own
// Camera2, AImageReader acquisition, preview frame slots, the presentation
// surface, UI state, or exposure-controller policy.
class SingleFrameCoordinator final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;

    SingleFrameCoordinator(const rawrcam::vulkan::VulkanContext& vulkanContext, std::mutex& queueSubmitMutex,
                           std::string filesDir, Diagnostic diagnostic = {});
    ~SingleFrameCoordinator();

    SingleFrameCoordinator(const SingleFrameCoordinator&) = delete;
    SingleFrameCoordinator& operator=(const SingleFrameCoordinator&) = delete;

    void configureRawSnapshot(uint32_t width, uint32_t height, uint64_t cameraGeneration);
    void disableRawSnapshot(const std::string& reason);
    [[nodiscard]] bool rawSnapshotConfigured() const noexcept;
    [[nodiscard]] bool captureRequested() const noexcept;

    // Camera2 metadata arming remains SessionEngine/NativeCameraController-owned.
    // Call only after that arm has succeeded.
    uint64_t requestRawStillCapture(encoding::dng::DngCaptureContext dngContext,
                                    rawrcam::capture::JpegCaptureRequest jpegContext);
    uint64_t recover(const std::string& name, bool multiframe, int dng, int merged, int jpeg);
    // The capture waits for the tagged one-shot frame (extraWait: its exposure).
    void expectOptimizedFrame(uint64_t requestId, std::chrono::milliseconds extraWait = {});

    // Processes only the still-capture branch of an already timestamp-paired RAW
    // frame. The caller remains responsible for the AImage lease and for replacing
    // its acquire fence with gpuAcquireFenceFd on return.
    SingleMatchedFrameResult processMatchedFrame(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd,
                                                 const metadata::FrameMetadataSnapshot& metadata,
                                                 const color::FrameColorTransform& colorState,
                                                 const tonemap::TonemapParams& tonemapParams, float aePostGain);

    // Claims one submitted frame without touching its AHardwareBuffer. Once the
    // preview fence retires, beginDeferredSnapshot() retains and copies it on a
    // worker so camera/preview submission never waits on CPU RAW access.
    bool deferSubmittedFrame(const metadata::FrameMetadataSnapshot& metadata,
                             const color::FrameColorTransform& colorState, const tonemap::TonemapParams& tonemapParams,
                             float aePostGain, bool filmEnabled, const spektrafilm_native::FilmLook& filmLook);
    // Returns true when the worker consumes imageLease; fence remains caller-owned.
    bool beginDeferredSnapshot(AImage* imageLease, AHardwareBuffer* ahb, uint64_t timestampNs, int acquireFenceFd = -1);

    // Advances acquisition timeouts only; processing progresses on an independent
    // worker, including when preview frames and application polling stop.
    void advance();
    void advanceDng();
    void advanceHq();

    std::string pollDngCompletion();
    std::string pollJpegCompletion();

    [[nodiscard]] bool detachedHqWorkActive() const noexcept;

    // Mirrors the previous SessionEngine lifecycle semantics.
    void cancelPendingDngBeforeIngressDestroy();
    void resetHqProcessing() noexcept;
    // APK asset access for the still film engine's lookup tables. Safe any time.
    void setFilmAssetManager(AAssetManager* assetManager);
    void shutdown() noexcept;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rawrcam::capture
