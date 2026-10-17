#include "diagnostics/replay/HighlightReplayRunner.h"
#include "session/SessionEngine.h"
namespace rawrcam::session {
std::string SessionEngine::runHighlightReplay(const std::string& inputPath) {
    diagnostics::replay::HighlightReplayRunner runner(
        vulkanContext_, queueSubmitMutex_, filesDir_, replayAssetManager_,
        [this](const std::string& line) { appendDiagnostic(line); },
        [this]() -> std::string {
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (!realtime_.configured()) return "HIGHLIGHT_REPLAY_FAIL camera_not_configured";
            }
            appendDiagnostic("HIGHLIGHT_REPLAY_STAGE stopping_camera");
            cameraControls_.setCameraActive(false);
            std::lock_guard<std::mutex> lock(mu_);
            // Stopping the camera does not wait for HAL or GPU retirement;
            // preview slots may still be running on the resources torn down below.
            if (vulkanContext_.device()) vulkanContext_.waitIdle();
            realtime_.coordinator().releaseCompletedSlots(true);
            destroyRawInputInternal();
            realtime_.releaseProcessingResourcesForFrozenReplay();
            swapchainRenderer_.destroySwapchain();
            if (!vulkanContext_.device() || !vulkanContext_.queue())
                return "HIGHLIGHT_REPLAY_FAIL vulkan_lost_after_preview_release";
            return {};
        });
    return runner.run(inputPath);
}
}  // namespace rawrcam::session
