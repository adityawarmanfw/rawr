#pragma once

#include <amaze/Amaze.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "develop/SharedHighlightRuntime.h"
#include "develop/demosaic/SingleFlightWorker.h"
#include "develop/demosaic/StillCompletion.h"
#include "imaging/RawSnapshot.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::develop::demosaic::amaze {

// Algorithm-specific HQ still adapter for AMaZE, mirroring RcdStillProcessor on
// the shared packed path only: SharedHighlightRuntime owns upload, packing,
// clip state, command/fence mechanics and the RGBA16F output; this class owns
// the AMaZE pipeline, its record call and the single-flight worker.
// AMaZE runs on the packed CFA white-balanced with the shot's baseline WB
// (green = 1) and exports camera-linear RGB like RCD.
class AmazeStillProcessor final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;

    explicit AmazeStillProcessor(Diagnostic diagnostic = {});
    ~AmazeStillProcessor();

    AmazeStillProcessor(const AmazeStillProcessor&) = delete;
    AmazeStillProcessor& operator=(const AmazeStillProcessor&) = delete;

    // Records one production geometry/context only; heavy Vulkan/AMaZE
    // workspace creation is deferred to the HQ worker. Caller must establish
    // GPU idleness before reconfiguration/reset.
    void configure(const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex,
                   uint64_t cameraContextGeneration, const std::string& cameraId, uint32_t width, uint32_t height,
                   uint32_t rawPreviewCfa,
                   rawrcam::develop::StillDemosaicGeometry geometry = rawrcam::develop::StillDemosaicGeometry::SensorNative,
                   VkQueue queueOverride = VK_NULL_HANDLE, std::mutex* queueSubmitMutexOverride = nullptr,
                   bool externalPackedInput = false);

    void reset() noexcept;
    bool dumpPackedCfaRgba16f(const std::string& path);

    [[nodiscard]] bool configured() const noexcept { return runtime_.configured() && width_ != 0 && height_ != 0; }
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] VkImage outputImage() const noexcept { return runtime_.outputImage(); }
    [[nodiscard]] VkImageView outputView() const noexcept { return runtime_.outputView(); }
    [[nodiscard]] VkImage clipStateImage() const noexcept { return runtime_.clipStateImage(); }
    [[nodiscard]] VkImageView clipStateView() const noexcept { return runtime_.clipStateView(); }
    bool dumpOutputRgba16f(const std::string& path);
    void releaseWorkspaceKeepOutput() noexcept;
    void releaseOutput() noexcept;
    [[nodiscard]] uint64_t currentAllocatedBytes() const noexcept;
    [[nodiscard]] uint64_t peakAllocatedBytes() const noexcept;

    bool start(const rawrcam::imaging::RawSnapshot& frame, std::shared_ptr<const std::vector<uint8_t>> raw,
               bool lensShadingCorrectionEnabled = false, bool highlightReconstructionEnabled = true);
    // Demosaics a caller-owned packed CFA (same contract as the shared
    // highlight packing), e.g. the multiframe merge output. Requires
    // externalPackedInput; whiteBalanceRggb is the shot's baseline WB.
    bool startExternalPacked(VkImage packedImage, VkImageView packedView,
                             const std::array<float, 4>& whiteBalanceRggb, uint64_t requestId,
                             uint64_t timestampNs = 0);
    std::optional<StillCompletion> pollCompletion();

    // Baseline WB (physical RGGB gains) as AMaZE RGB gains with green = 1.
    static std::array<float, 3> amazeWhiteBalance(const std::array<float, 4>& whiteBalanceRggb) noexcept;

   private:
    void joinWorker() noexcept;
    void ensureRuntimeResources();
    void emit(const std::string& line) const;

    rawrcam::develop::demosaic::SingleFlightWorker<StillCompletion> worker_;
    rawrcam::develop::SharedHighlightRuntime runtime_;
    uint64_t cameraContextGeneration_ = 0;
    std::string cameraId_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    ::amaze::BayerPattern pattern_ = ::amaze::BayerPattern::RGGB;
    rawrcam::develop::StillDemosaicGeometry geometry_ = rawrcam::develop::StillDemosaicGeometry::SensorNative;

    std::unique_ptr<::amaze::AmazePipeline> pipeline_;
};

}  // namespace rawrcam::develop::demosaic::amaze
