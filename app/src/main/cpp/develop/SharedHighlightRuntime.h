#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "develop/highlight/LensShadingMapSnapshot.h"
#include "galosh/GaloshRawPipeline.hpp"
#include "vulkan/ImageResources.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::develop {

enum class StillDemosaicGeometry : std::uint8_t {
    SensorNative = 0,
    ReconstructedCfa = 1,
};

class SharedHighlightRuntime final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;

    SharedHighlightRuntime(std::string label, Diagnostic diagnostic = {});
    ~SharedHighlightRuntime();

    SharedHighlightRuntime(const SharedHighlightRuntime&) = delete;
    SharedHighlightRuntime& operator=(const SharedHighlightRuntime&) = delete;

    void configure(const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex, uint32_t width,
                   uint32_t height, uint32_t rawPreviewCfa, bool sharedHighlightPacked,
                   // Optional submit-queue override (same device + family).
                   // Lets still chains run demosaic on the multiframe queue
                   // so preview submits on queue0 don't wait behind it.
                   VkQueue queueOverride = VK_NULL_HANDLE, std::mutex* queueSubmitMutexOverride = nullptr,
                   // The caller supplies the packed CFA input (beginExternal):
                   // no RAW upload, packing or replay resources, output only.
                   bool externalInput = false);
    void ensureResources();
    void reset() noexcept;
    void releaseWorkspaceKeepOutput() noexcept;
    void releaseOutput() noexcept;

    [[nodiscard]] bool configured() const noexcept { return device_ != VK_NULL_HANDLE && width_ != 0 && height_ != 0; }
    [[nodiscard]] bool resourcesReady() const noexcept {
        if (externalInput_) return command_ && fence_ && output_.image;
        return uploadMapped_ && command_ && fence_ && input_.image && output_.image;
    }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return physicalDevice_; }
    [[nodiscard]] VkDevice device() const noexcept { return device_; }
    [[nodiscard]] uint32_t queueFamily() const noexcept { return queueFamily_; }
    [[nodiscard]] VkImage inputImage() const noexcept { return input_.image; }
    [[nodiscard]] VkImageView inputView() const noexcept { return input_.view; }
    [[nodiscard]] VkImage packedImage() const noexcept { return packed_.image; }
    [[nodiscard]] VkImageView packedView() const noexcept { return packed_.view; }
    [[nodiscard]] VkImage clipStateImage() const noexcept { return clipState_.image; }
    [[nodiscard]] VkImageView clipStateView() const noexcept { return clipState_.view; }
    [[nodiscard]] VkImage outputImage() const noexcept { return output_.image; }
    [[nodiscard]] VkImageView outputView() const noexcept { return output_.view; }
    [[nodiscard]] VkFormat outputFormat() const noexcept { return output_.format; }

    // External-input mode: begins the command buffer and prepares the output
    // image; the caller records the demosaic from its own packed CFA image.
    VkCommandBuffer beginExternal();
    VkCommandBuffer beginTaggedPackedReplay(const std::vector<uint8_t>& taggedPackedRgba16f,
                                            const std::array<float, 4>& whiteBalanceRggb, uint32_t variant,
                                            float anchorTolerance, float madLimit, float consensusLimit,
                                            uint32_t minSupport);
    VkCommandBuffer beginFrame(const std::vector<uint8_t>& rawCopy, const std::array<float, 4>& blackPhysical,
                               float whiteLevel, const std::array<float, 4>& whiteBalanceRggb,
                               const rawrcam::develop::highlight::LensShadingMapSnapshot& lensShading = {},
                               bool highlightReconstructionEnabled = true,
                               // GALOSH-RAW Bayer denoise (P1a): runs on
                               // input_ in place right after upload, before
                               // highlight packing. Off = legacy path untouched.
                               const ::galosh::GaloshRawParams& galosh = ::galosh::GaloshRawParams{});
    // Submits what is recorded so far, waits for it, and re-begins the same
    // command buffer. Splitting a full-resolution still into several bounded
    // submissions lets the preview queue run in between: queues of equal
    // priority are not preempted mid-submission on Adreno.
    void flush();
    // Same bounded submission as flush(), but without the CPU round trip: the
    // batch is submitted and recording continues in a second command buffer,
    // waiting only for the submission before it. At most two submissions are
    // in flight, so the queue never drains between batches and the preview
    // still gets a turn at each boundary. Returns the buffer to record into.
    VkCommandBuffer flushPipelined();
    // The command buffer currently being recorded (changes after flushPipelined()).
    [[nodiscard]] VkCommandBuffer currentCommand() const noexcept { return command_; }
    void submit(VkCommandBuffer command);
    void waitForCompletion();
    void discardWorkspace() noexcept;
    bool dumpOutputRgba16f(const std::string& path);
    bool dumpPackedCfaRgba16f(const std::string& path);
    bool dumpExternalImage(const std::string& path, VkImage image, uint32_t width, uint32_t height,
                           uint32_t bytesPerPixel, VkAccessFlags producerAccess = VK_ACCESS_SHADER_WRITE_BIT);

   private:
    void createSharedHighlightPipeline();
    void createReplayHighlightPipeline();
    void destroyReplayHighlightPipeline() noexcept;
    void destroySharedHighlightPipeline() noexcept;
    void recordSharedHighlight(VkCommandBuffer command, const std::array<float, 4>& blackPhysical, float whiteLevel,
                               const std::array<float, 4>& whiteBalanceRggb,
                               const rawrcam::develop::highlight::LensShadingMapSnapshot& lensShading);
    void ensureLensShadingBuffer(VkDeviceSize requiredBytes);
    void uploadLensShadingMap(const rawrcam::develop::highlight::LensShadingMapSnapshot& lensShading);
    void destroyLensShadingBuffer() noexcept;
    void destroyWorkspaceResources(bool keepOutput) noexcept;
    void emit(const std::string& line) const;

    std::string label_;
    Diagnostic diagnostic_;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    std::mutex* queueSubmitMutex_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t rawPreviewCfa_ = 0;
    bool sharedHighlightPacked_ = false;
    bool externalInput_ = false;
    // GALOSH f16 gate (VulkanContext::float16ComputeEnabled, latched at
    // configure): without f16 SSBO/arithmetic the vendored shaders cannot
    // legally execute, so the tap stays off and Wavelet serves.
    bool float16Compute_ = false;

    rawrcam::vulkan::OwnedImage input_{};
    rawrcam::vulkan::OwnedImage packed_{};
    rawrcam::vulkan::OwnedImage clipState_{};
    rawrcam::vulkan::OwnedImage replayInput_{};
    rawrcam::vulkan::OwnedImage output_{};
    VkBuffer uploadBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory uploadMemory_ = VK_NULL_HANDLE;
    void* uploadMapped_ = nullptr;
    VkDeviceSize uploadBytes_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    // flushPipelined() double buffer: the previous, possibly still running submission.
    VkCommandBuffer spareCommand_ = VK_NULL_HANDLE;
    VkFence spareFence_ = VK_NULL_HANDLE;
    bool sparePending_ = false;
    void waitSpare() noexcept;

    VkDescriptorSetLayout highlightDsl_ = VK_NULL_HANDLE;
    VkPipelineLayout highlightLayout_ = VK_NULL_HANDLE;
    VkPipeline highlightPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool highlightPool_ = VK_NULL_HANDLE;
    VkDescriptorSet highlightSet_ = VK_NULL_HANDLE;
    VkBuffer lscBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory lscMemory_ = VK_NULL_HANDLE;
    void* lscMapped_ = nullptr;
    VkDeviceSize lscBytes_ = 0;

    VkDescriptorSetLayout replayDsl_ = VK_NULL_HANDLE;
    VkPipelineLayout replayLayout_ = VK_NULL_HANDLE;
    VkPipeline replayPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool replayPool_ = VK_NULL_HANDLE;
    VkDescriptorSet replaySet_ = VK_NULL_HANDLE;

    // Lazy GALOSH-RAW stage (owns its transient graph; geometry epochs
    // reallocate inside process()). Null until first enabled shot.
    std::unique_ptr<::galosh::GaloshRawPipeline> galosh_;
    // Device the galosh pipeline was built on (see StillImageRenderer:
    // rebuild on device change instead of submitting to a dead VkDevice).
    VkDevice galoshDevice_ = VK_NULL_HANDLE;
    void ensureGalosh();
};

}  // namespace rawrcam::develop
