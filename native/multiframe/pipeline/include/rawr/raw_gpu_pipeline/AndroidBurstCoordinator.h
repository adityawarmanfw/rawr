#pragma once
#include <rawr/raw_gpu_pipeline/MultiframeRecorder.h>
#include <rawr/raw_merge_hdrplus_gpu/RawMergeHdrPlusGpu.h>
#include <rawr/zsl_ring/RawImageRing.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <mutex>
#include <vector>

namespace rawr::raw_gpu_pipeline {
// Wronski: kernel-regression super-resolution merge (RGB output, optional
// upscale). HdrPlusSpatial: HDR+ tile-aligned robust average (Bayer values
// written into the RGBA16F output, 1x only).
enum class MergeAlgorithm : std::uint32_t { Wronski = 0, HdrPlusSpatial = 1 };
struct BurstFrame {
    rawr::zsl_ring::GpuRawImageView raw{};
    MultiframeFrameParameters parameters{};
};
struct BurstStageTimings {
    double initializeMs = 0.0;
    double referencePrepareMs = 0.0;
    double referenceStatsMs = 0.0;
    double companionPrepareMs = 0.0;
    double alignmentMs = 0.0;
    double kernelStatsMs = 0.0;
    double robustnessMs = 0.0;
    double accumulationMs = 0.0;
    double finalizeMs = 0.0;
    double noiseEstimateMs = 0.0;
    double totalMs = 0.0;
    [[nodiscard]] double alignmentTotalMs() const noexcept {
        return referencePrepareMs + companionPrepareMs + alignmentMs;
    }
    [[nodiscard]] double mergeTotalMs() const noexcept {
        return referenceStatsMs + kernelStatsMs + robustnessMs + accumulationMs + finalizeMs;
    }
};
struct BurstRunResult {
    std::uint32_t referenceIndex = 0;
    std::uint32_t frameCount = 0;
    VkImage output = VK_NULL_HANDLE;
    VkImageView outputView = VK_NULL_HANDLE;
    VkFormat outputFormat = VK_FORMAT_UNDEFINED;
    Extent2D outputExtent{};
    VkDeviceSize physicalImageBytes = 0;
    VkDeviceSize physicalBufferBytes = 0;
    std::uint32_t submissionCount = 0;
    bool resourcesReused = false;
    bool pipelineCacheReused = false;
    BurstStageTimings timings{};
    // Set when estimateNoiseFromBurst produced the profile this run merged with.
    bool noiseEstimated = false;
    rawr::raw_merge_wronski_gpu::CfaNoiseProfile estimatedNoise{};
};
// Synchronous correctness executor. The app runs it on a background worker while
// borrowing pinned RAW16 images from RawImageRing. It uses the app's existing Vulkan queue via Submit.
class AndroidBurstCoordinator final {
   public:
    enum class Stage : std::uint32_t {
        ArenaInitializeBegin = 1,
        ArenaInitializeEnd = 2,
        RecordBegin = 3,
        RecordEnd = 4,
        SubmitBegin = 5,
        SubmitEnd = 6,
        FenceWaitBegin = 7,
        FenceWaitEnd = 8,
        ChunkBegin = 9,
        ChunkEnd = 10,
    };
    using Submit = std::function<void(const VkSubmitInfo&, VkFence)>;
    using StageSink = std::function<void(Stage, VkDeviceSize, VkDeviceSize)>;
    using FrameConsumed = std::function<void(std::uint32_t)>;
    AndroidBurstCoordinator() = default;
    ~AndroidBurstCoordinator() { reset(); }
    AndroidBurstCoordinator(const AndroidBurstCoordinator&) = delete;
    AndroidBurstCoordinator& operator=(const AndroidBurstCoordinator&) = delete;
    void initialize(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily, Submit submit,
                    std::uint32_t width, std::uint32_t height, float outputScale = 1.0f, StageSink stageSink = {},
                    rawr::raw_alignment_gpu::Config alignment = {}, rawr::raw_merge_wronski_gpu::Config merge = {},
                    MergeAlgorithm algorithm = MergeAlgorithm::Wronski,
                    rawr::raw_merge_hdrplus_gpu::Config hdrplus = {});
    // Pre-creates pipelines, command pool, fence and timestamp pool on a
    // background thread so the first burst skips cold init. Arena images
    // (~1.3GB) stay lazy. Safe to call before initialize(); initialize()
    // reuses the warmed infrastructure. Never throws-fatal: callers log.
    void warmPipelines(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily);
    // warmPipelines plus arena creation at the last-used output scale
    // (default 1x). Makes even the first burst fully warm when geometry
    // matches. Holds ~1.3GB while the camera is open; reset()/resetConfiguration
    // release it. Same reuse rules as initialize().
    void warmAll(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily, std::uint32_t width,
                 std::uint32_t height);
    BurstRunResult run(const std::vector<BurstFrame>& frames, std::uint32_t referenceIndex,
                       FrameConsumed frameConsumed = {});
    // One-shot wait inserted on the next submitted chunk (cross-queue memory
    // edge, e.g. prior-queue writes to borrowed inputs). The coordinator does
    // not own the semaphore; the caller signals it before run() and destroys
    // it after. Cleared after first use; no-op when VK_NULL_HANDLE.
    void setInitialWaitSemaphore(VkSemaphore semaphore) noexcept { initialWait_ = semaphore; }
    // Frees the large per-geometry image/buffer arena after the merged output
    // has been projected, while retaining pipelines and command infrastructure.
    void releaseScratch() noexcept;
    void reset() noexcept;

   private:
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0, width_ = 0, height_ = 0;
    float outputScale_ = 0.0f;
    Submit submit_{};
    StageSink stageSink_{};
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore initialWait_ = VK_NULL_HANDLE;
    // Last output scale seen (survives reset: self-tuning warmup heuristic).
    float lastScale_ = 1.0f;
    // Guards initialize/warmPipelines/reset (pipelines + pools + handles).
    // run() is called after initialize() returns on the same worker.
    std::mutex initMutex_;
    bool executionReady_ = false;
    void resetLocked() noexcept;
    void resetInfraLocked() noexcept;
    void ensureExecutionInfra(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily);
    VkQueryPool timestampPool_ = VK_NULL_HANDLE;
    float timestampPeriodNs_ = 0.0f;
    bool timestampsSupported_ = false;
    ResourceArena arena_{};
    VulkanExecutor executor_{};
    rawr::raw_alignment_gpu::Config alignment_{};
    rawr::raw_merge_wronski_gpu::Config merge_{};
    MergeAlgorithm algorithm_ = MergeAlgorithm::Wronski;
    rawr::raw_merge_hdrplus_gpu::Config hdrplus_{};
    // Which layout arena_ currently holds (HDR+ geometry depends on its config).
    MergeAlgorithm arenaAlgorithm_ = MergeAlgorithm::Wronski;
    rawr::raw_merge_hdrplus_gpu::Config arenaHdrPlus_{};
    [[nodiscard]] bool arenaMatchesSelection() const noexcept;
    void initializeArenaLocked(VkPhysicalDevice physical, VkDevice device);
    // One recorded submission, fence-waited; returns wall ms and GPU timestamp intervals.
    using ChunkTiming = std::pair<double, std::vector<double>>;
    ChunkTiming executeChunk(std::uint32_t chunkCode, std::uint32_t frameIndex, std::uint32_t queryCount,
                             const std::function<void()>& record);
    void recordTimestamp(std::uint32_t query);
    BurstRunResult runHdrPlus(const std::vector<BurstFrame>& frames, std::uint32_t referenceIndex,
                              const FrameConsumed& frameConsumed);
    bool arenaReady_ = false;
    bool layoutsInitialized_ = false;
    bool submissionInFlight_ = false;
    bool resourcesReused_ = false;
    bool pipelineCacheReused_ = false;
    double pendingInitializeMs_ = 0.0;
    // Small host-visible coherent buffers: burst noise block statistics
    // (GPU -> host) and the hot-pixel list (host -> GPU).
    struct HostBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
        void* mapped = nullptr;
    };
    HostBuffer noiseStats_{};
    HostBuffer hotPixels_{};
    void ensureHostBuffer(HostBuffer& buffer, VkDeviceSize bytes);
    void destroyHostBuffer(HostBuffer& buffer) noexcept;
    void destroyHostBuffers() noexcept;
};
}  // namespace rawr::raw_gpu_pipeline
