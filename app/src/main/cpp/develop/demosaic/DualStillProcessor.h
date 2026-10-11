#pragma once

#include <array>
#include <cstdint>
#include <dual/Dual.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "develop/SharedHighlightRuntime.h"
#include "develop/demosaic/SingleFlightWorker.h"
#include "develop/demosaic/StillCompletion.h"
#include "imaging/RawSnapshot.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::develop::demosaic::dual {

// App adapter for the RCD+VNG4 / AMaZE+VNG4 dual-blend implementation.
// Consumes the app-owned shared-highlight packed CFA image and writes the
// full-resolution RGBA16F camera-linear output contract used by RCD.
// The app RCD and AMaZE libraries are reused by Dual.
class DualStillProcessor final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    explicit DualStillProcessor(Diagnostic diagnostic = {});
    ~DualStillProcessor();
    DualStillProcessor(const DualStillProcessor&) = delete;
    DualStillProcessor& operator=(const DualStillProcessor&) = delete;

    void configure(
        const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex, uint64_t cameraContextGeneration,
        const std::string& cameraId, uint32_t width, uint32_t height, uint32_t rawPreviewCfa,
        bool dualAutoContrast = true, float dualContrastPercent = 20.0f, bool diagnosticsEnabled = false,
        rawrcam::develop::StillDemosaicGeometry geometry = rawrcam::develop::StillDemosaicGeometry::SensorNative,
        VkQueue queueOverride = VK_NULL_HANDLE, std::mutex* queueSubmitMutexOverride = nullptr,
        bool externalPackedInput = false, ::dual::Primary primary = ::dual::Primary::Rcd);
    // raw holds the packed RAW16 bytes to demosaic (possibly shared with the
    // DNG writer); it is released as soon as the GPU upload is recorded.
    bool start(const rawrcam::imaging::RawSnapshot& frame, std::shared_ptr<const std::vector<uint8_t>> raw,
               bool lensShadingCorrectionEnabled = false, bool highlightReconstructionEnabled = true,
               const ::galosh::GaloshRawParams& galosh = ::galosh::GaloshRawParams{});
    // External packed CFA input (configure with externalPackedInput): the
    // caller owns a (width/2 x height/2) RGBA16F packed image in GENERAL
    // layout, already written and fence-waited (e.g. the multiframe merge
    // repacked by MultiframeOutputAdapter::preparePackedCfa). Clip state stays
    // with the caller; clipStateImage() is null in this mode.
    // whiteBalanceRggb: the shot's baseline WB (used by an AMaZE primary).
    bool startExternalPacked(VkImage packedImage, VkImageView packedView, uint64_t requestId, uint64_t timestampNs = 0,
                             const std::array<float, 4>& whiteBalanceRggb = {1.0f, 1.0f, 1.0f, 1.0f});
    bool startTaggedPackedReplay(std::vector<uint8_t> taggedPackedRgba16f, const std::array<float, 4>& whiteBalanceRggb,
                                 uint32_t variant, float anchorTolerance, float madLimit, float consensusLimit,
                                 uint32_t minSupport, uint64_t requestId = 1);
    std::optional<StillCompletion> pollCompletion();
    void releaseWorkspaceKeepOutput() noexcept;
    void releaseOutput() noexcept;
    void reset() noexcept;
    bool dumpOutputRgba16f(const std::string& path);
    bool dumpPackedCfaRgba16f(const std::string& path);
    bool dumpDualRcdRgba16f(const std::string& path);
    bool dumpDualVngRgba16f(const std::string& path);
    bool dumpDualPreBlurMaskR32f(const std::string& path);
    bool dumpDualBlendMaskR32f(const std::string& path);

    [[nodiscard]] bool configured() const noexcept { return runtime_.configured() && width_ && height_; }
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] VkImage outputImage() const noexcept { return runtime_.outputImage(); }
    [[nodiscard]] VkImageView outputView() const noexcept { return runtime_.outputView(); }
    [[nodiscard]] VkImage clipStateImage() const noexcept { return runtime_.clipStateImage(); }
    [[nodiscard]] VkImageView clipStateView() const noexcept { return runtime_.clipStateView(); }
    [[nodiscard]] uint64_t currentAllocatedBytes() const noexcept;
    [[nodiscard]] uint64_t peakAllocatedBytes() const noexcept;
    // "RCD+VNG4" or "AMaZE+VNG4" (the configured primary).
    [[nodiscard]] const char* algorithmName() const noexcept;

   private:
    void ensureRuntimeResources();
    void joinWorker() noexcept;
    void emit(const std::string& line) const;
    static ::dual::BayerPattern dualPattern(uint32_t cfa);
    // White balance and submission boundaries for one record() call.
    void prepareRecord(const std::array<float, 4>& whiteBalanceRggb, bool boundedSubmissions);

    SingleFlightWorker<StillCompletion> worker_;
    rawrcam::develop::SharedHighlightRuntime runtime_;
    uint64_t cameraContextGeneration_ = 0;
    std::string cameraId_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t cfa_ = 0;
    bool dualAutoContrast_ = true;
    float dualContrastPercent_ = 20.0f;
    bool diagnosticsEnabled_ = false;
    rawrcam::develop::StillDemosaicGeometry geometry_ = rawrcam::develop::StillDemosaicGeometry::SensorNative;
    ::dual::Primary primary_ = ::dual::Primary::Rcd;
    std::unique_ptr<::dual::DualDemosaicPipeline> dual_;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
};

}  // namespace rawrcam::develop::demosaic::dual
