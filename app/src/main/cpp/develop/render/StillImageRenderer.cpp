#include "develop/render/StillImageRenderer.h"

#include "develop/render/StillRenderSequence.h"
namespace rawrcam::develop::rendered {

StillImageRenderer::StillImageRenderer(std::string filesDir, Diagnostic diagnostic)
    : filesDir_(std::move(filesDir)), diagnostic_(std::move(diagnostic)), resources_(filesDir_, diagnostic_) {}
StillImageRenderer::~StillImageRenderer() { reset(); }

void StillImageRenderer::setAssetManager(AAssetManager* assetManager) noexcept {
    resources_.setAssetManager(assetManager);
}

void StillImageRenderer::shutdownFilm() noexcept {
    joinWorker();
    resources_.destroyFilm();
}

bool StillImageRenderer::busy() const noexcept {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return busy_;
}

bool StillImageRenderer::start(const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex,
                               const RenderedStillContext& rendered, VkImage sourceImage, VkImageView sourceView,
                               VkImage clipStateImage, VkImageView clipStateView, VkQueue queueOverride,
                               std::mutex* queueSubmitMutexOverride) {
    if (!context.device() || !context.physicalDevice() || !context.queue() || !sourceImage || !sourceView ||
        rendered.requestId == 0 || rendered.width == 0 || rendered.height == 0)
        return false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (busy_ || completion_ || resources_.holdsOutput()) return false;
        busy_ = true;
    }
    joinWorker();
    const RenderDeviceContext device{context.physicalDevice(),
                                     context.device(),
                                     queueOverride ? queueOverride : context.queue(),
                                     context.queueFamily(),
                                     queueSubmitMutexOverride ? queueSubmitMutexOverride : &queueSubmitMutex};
    resources_.bind(device);

    try {
        worker_ = std::thread([this, device, rendered, sourceImage, sourceView, clipStateImage, clipStateView]() {
            auto done =
                executeStillRender(resources_, commands_, device, rendered, sourceImage, sourceView, clipStateImage,
                                   clipStateView, filesDir_, [this](const std::string& line) { emit(line); });
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                completion_ = done;
                busy_ = false;
            }
        });
    } catch (...) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        busy_ = false;
        return false;
    }
    return true;
}

std::optional<RenderedStillCompletion> StillImageRenderer::pollCompletion() {
    std::optional<RenderedStillCompletion> out;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!completion_) return std::nullopt;
        out = completion_;
        completion_.reset();
    }
    joinWorker();
    return out;
}
void StillImageRenderer::joinWorker() noexcept {
    if (worker_.joinable()) worker_.join();
}
void StillImageRenderer::releasePixels() noexcept {
    joinWorker();
    commands_.reset();
    resources_.releasePixels();
}

void StillImageRenderer::reset() noexcept {
    joinWorker();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        busy_ = false;
        completion_.reset();
    }
    commands_.reset();
    resources_.reset();
}

void StillImageRenderer::emit(const std::string& line) const {
    if (diagnostic_) diagnostic_(line);
}

}  // namespace rawrcam::develop::rendered
