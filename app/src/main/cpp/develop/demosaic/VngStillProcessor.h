#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <vng4/Vng4.hpp>

#include "develop/SharedHighlightRuntime.h"
#include "develop/demosaic/SingleFlightWorker.h"
#include "develop/demosaic/StillCompletion.h"
#include "imaging/RawSnapshot.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::develop::demosaic::vng4 {

// App adapter for the frozen standalone VNG4 implementation. Consumes the
// app-owned shared-highlight packed CFA image and writes the full-resolution
// RGBA16F camera-linear output contract used by RCD.
class VngStillProcessor final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    explicit VngStillProcessor(Diagnostic diagnostic = {});
    ~VngStillProcessor();
    VngStillProcessor(const VngStillProcessor&) = delete;
    VngStillProcessor& operator=(const VngStillProcessor&) = delete;

    void configure(
        const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex, uint64_t cameraContextGeneration,
        const std::string& cameraId, uint32_t width, uint32_t height, uint32_t rawPreviewCfa,
        bool diagnosticsEnabled = false,
        rawrcam::develop::StillDemosaicGeometry geometry = rawrcam::develop::StillDemosaicGeometry::SensorNative,
        VkQueue queueOverride = VK_NULL_HANDLE, std::mutex* queueSubmitMutexOverride = nullptr);
    // raw holds the packed RAW16 bytes to demosaic (possibly shared with the
    // DNG writer); it is released as soon as the GPU upload is recorded.
    bool start(const rawrcam::imaging::RawSnapshot& frame, std::shared_ptr<const std::vector<uint8_t>> raw,
               bool lensShadingCorrectionEnabled = false, bool highlightReconstructionEnabled = true);
    std::optional<StillCompletion> pollCompletion();
    void releaseWorkspaceKeepOutput() noexcept;
    void releaseOutput() noexcept;
    void reset() noexcept;
    bool dumpOutputRgba16f(const std::string& path);
    bool dumpPackedCfaRgba16f(const std::string& path);

    [[nodiscard]] bool configured() const noexcept { return runtime_.configured() && width_ && height_; }
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] VkImage outputImage() const noexcept { return runtime_.outputImage(); }
    [[nodiscard]] VkImageView outputView() const noexcept { return runtime_.outputView(); }
    [[nodiscard]] VkImage clipStateImage() const noexcept { return runtime_.clipStateImage(); }
    [[nodiscard]] VkImageView clipStateView() const noexcept { return runtime_.clipStateView(); }
    [[nodiscard]] uint64_t currentAllocatedBytes() const noexcept;
    [[nodiscard]] uint64_t peakAllocatedBytes() const noexcept;

   private:
    void ensureRuntimeResources();
    void joinWorker() noexcept;
    void emit(const std::string& line) const;
    static ::vng4::BayerPattern vngPattern(uint32_t cfa);
    static const char* algorithmName() noexcept;

    SingleFlightWorker<StillCompletion> worker_;
    rawrcam::develop::SharedHighlightRuntime runtime_;
    uint64_t cameraContextGeneration_ = 0;
    std::string cameraId_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t cfa_ = 0;
    // Accepted for configure-symmetry with Dual; only surfaces in the
    // diagnostics emit and the reconfiguration guard. Unused by VNG4.
    bool diagnosticsEnabled_ = false;
    rawrcam::develop::StillDemosaicGeometry geometry_ = rawrcam::develop::StillDemosaicGeometry::SensorNative;
    std::unique_ptr<::vng4::Vng4Pipeline> vng4_;
};

}  // namespace rawrcam::develop::demosaic::vng4
