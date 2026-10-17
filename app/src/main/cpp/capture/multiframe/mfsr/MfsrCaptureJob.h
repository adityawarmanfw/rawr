#pragma once

#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <mutex>
#include <string>

#include "capture/multiframe/MultiframeWorkItem.h"
#include "capture/multiframe/SharedCompletionMailbox.h"
#include "develop/render/StillImageRenderer.h"
#include "vulkan/VulkanContext.h"

struct AAssetManager;

namespace rawrcam::capture::multiframe {

// Background burst worker: base-DNG readback, GPU merge, projection,
// demosaic, render, JPEG/DNG writers, RZSL dump. Previously
// FrameSubmitCoordinator::processMultiframeWorkItem + writePostShutterRzsl.
// Runs on the service's worker thread; touches no preview slots.
class MfsrCaptureJob {
   public:
    struct Context {
        rawrcam::vulkan::VulkanContext& vulkan;
        std::mutex& queue0Mutex;
        std::string filesDir;
        std::uint32_t rawWidth = 0;
        std::uint32_t rawHeight = 0;
        std::uint32_t cfa = 0;
        rawr::raw_gpu_pipeline::AndroidBurstCoordinator& burstCoordinator;
        // Persistent render processor owned by the service (used only when
        // the Persistent Engine toggle is on; otherwise run() uses a local).
        develop::rendered::StillImageRenderer& renderedStill;
        bool persistentEngine = false;
        StringMailbox& dngBox;
        StringMailbox& jpegBox;
        AAssetManager* assetManager = nullptr;
    };

    MfsrCaptureJob(Context context, std::unique_ptr<MultiframeWorkItem> work)
        : context_(std::move(context)), work_(std::move(work)) {}
    // Every requested output gets exactly one completion, even when a stage
    // throws outside its own error handling.
    void run() noexcept;

   private:
    struct Published {
        bool base = false, merged = false, jpeg = false;
    };
    void runStages(Published& published);
    Context context_;
    std::unique_ptr<MultiframeWorkItem> work_;
};

}  // namespace rawrcam::capture::multiframe
