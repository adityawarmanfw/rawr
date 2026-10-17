#pragma once
#include <array>
#include <optional>
#include <string>

#include "develop/render/RenderDeviceContext.h"
#include "vulkan/CommandResources.h"
namespace rawrcam::develop::rendered {
class RenderCommandSession final {
   public:
    static constexpr uint32_t timingQueryCount = 18;
    ~RenderCommandSession() { reset(); }
    RenderCommandSession() = default;
    RenderCommandSession(const RenderCommandSession&) = delete;
    RenderCommandSession& operator=(const RenderCommandSession&) = delete;
    void create(const RenderDeviceContext& context);
    void begin();
    void submitAndRestart(const std::string& stage);
    void finish(uint64_t requestId);
    void reset() noexcept;
    VkCommandBuffer command() const noexcept { return command_; }
    // False once a submit, wait or re-begin has failed: the command buffer
    // is ended or still pending and must not be recorded into.
    bool recording() const noexcept { return recording_; }
    VkQueryPool timingPool() const noexcept { return timingPool_; }
    double chunkWaitMs() const noexcept { return chunkWaitMs_; }
    std::optional<std::array<uint64_t, timingQueryCount>> timestamps();
    double milliseconds(const std::array<uint64_t, timingQueryCount>& values, int a, int b) const noexcept;

   private:
    RenderDeviceContext context_{};
    vulkan::CommandResources commands_;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool timingPool_ = VK_NULL_HANDLE;
    float timestampPeriodNs_ = 0.0f;
    uint32_t timestampValidBits_ = 0;
    double chunkWaitMs_ = 0.0;
    bool recording_ = false;
};
}  // namespace rawrcam::develop::rendered
