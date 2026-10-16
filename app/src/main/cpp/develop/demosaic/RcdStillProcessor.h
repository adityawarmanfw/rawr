#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <rcd/Rcd.hpp>
#include <string>

#include "develop/SharedHighlightRuntime.h"
#include "develop/demosaic/SingleFlightWorker.h"
#include "develop/demosaic/StillCompletion.h"
#include "imaging/RawSnapshot.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::develop::demosaic::rcd {

enum class RcdStillInputMode : uint32_t { RawLegacy = 0, SharedHighlightPacked = 1 };

// Algorithm-specific HQ still adapter. The RCD library remains record-only;
// shared app-owned Vulkan images/upload/highlight/command/fence mechanics live in
// SharedHighlightRuntime, while this class owns RCD-specific configuration, pipeline,
// record calls, and asynchronous single-flight state.
//
// It deliberately does not own Camera2, preview frame slots, DNG serialization,
// rendered-still encoding, or UI state. The supplied queue mutex must be the
// same mutex used by every RawrCam vkQueueSubmit on the shared queue.
class RcdStillProcessor final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;

    explicit RcdStillProcessor(Diagnostic diagnostic = {});
    ~RcdStillProcessor();

    RcdStillProcessor(const RcdStillProcessor&) = delete;
    RcdStillProcessor& operator=(const RcdStillProcessor&) = delete;

    // Records one production geometry/context only. Heavy Vulkan/RCD workspace
    // creation is deferred to the HQ worker so the camera/preview frame thread
    // is never blocked by first-use pipeline construction. Caller must establish
    // GPU idleness before reconfiguration/reset.
    void configure(
        const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex, uint64_t cameraContextGeneration,
        const std::string& cameraId, uint32_t width, uint32_t height, uint32_t rawPreviewCfa,
        RcdStillInputMode inputMode = RcdStillInputMode::RawLegacy,
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
    // Diagnostic only: exact byte-for-byte readback of the post-RCD scene-linear
    // VK_FORMAT_R16G16B16A16_SFLOAT image. Imaging math is not modified.
    bool dumpOutputRgba16f(const std::string& path);
    void releaseWorkspaceKeepOutput() noexcept;
    void releaseOutput() noexcept;
    [[nodiscard]] uint64_t currentAllocatedBytes() const noexcept;
    [[nodiscard]] uint64_t peakAllocatedBytes() const noexcept;

    // Vulkan allocation, upload-buffer mapping/copy, command recording,
    // submission and RCD execution all happen asynchronously on the HQ worker.
    // raw (the packed RAW16 bytes, possibly shared with the DNG writer) is held
    // until the upload is recorded; the rawrcam::imaging::RawSnapshot itself may be released
    // immediately after start() succeeds.
    bool start(const rawrcam::imaging::RawSnapshot& frame, std::shared_ptr<const std::vector<uint8_t>> raw,
               bool lensShadingCorrectionEnabled = false, bool highlightReconstructionEnabled = true);
    // Demosaics a caller-owned packed CFA (SharedHighlightPacked contract),
    // e.g. the multiframe merge output. Requires externalPackedInput.
    bool startExternalPacked(VkImage packedImage, VkImageView packedView, uint64_t requestId, uint64_t timestampNs = 0);
    std::optional<StillCompletion> pollCompletion();

   private:
    static ::rcd::BayerPattern mapCfa(uint32_t rawPreviewCfa);
    static std::array<float, 4> parityBlackLevels(::rcd::BayerPattern pattern,
                                                  const std::array<float, 4>& physicalRggb) noexcept;
    void joinWorker() noexcept;
    void ensureRuntimeResources();
    void emit(const std::string& line) const;

    rawrcam::develop::demosaic::SingleFlightWorker<StillCompletion> worker_;
    rawrcam::develop::SharedHighlightRuntime runtime_;
    uint64_t cameraContextGeneration_ = 0;
    std::string cameraId_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    ::rcd::BayerPattern pattern_ = ::rcd::BayerPattern::RGGB;
    RcdStillInputMode inputMode_ = RcdStillInputMode::RawLegacy;
    rawrcam::develop::StillDemosaicGeometry geometry_ = rawrcam::develop::StillDemosaicGeometry::SensorNative;

    std::unique_ptr<::rcd::RcdPipeline> pipeline_;
};

}  // namespace rawrcam::develop::demosaic::rcd
