#pragma once
#include <rawr/raw_gpu_pipeline/FrameParameters.h>
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
// HdrPlusFrequency: HDR+ per-frequency Wiener merge (upstream "Higher quality"). Rexture: kernel-regression Bayer merge
// (module merge_rexture). Both write Bayer values into the RGBA16F output at 1x.
enum class MergeAlgorithm : std::uint32_t { HdrPlusFrequency = 2, Rexture = 4 };
struct BurstFrame {
    rawr::zsl_ring::GpuRawImageView raw{};
    MultiframeFrameParameters parameters{};
    // Linear exposure (exposure time x gain, any consistent unit); only ratios
    // to the reference are used, by the HDR+ merges. <= 0 = unknown, treated
    // as the reference exposure.
    float exposure = 0.0f;
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
    // HDR+ only: normalized full-precision R32F CFA plane (same extent as
    // output) for the 16-bit merged DNG projection. HDR+ Quality only (null for Rexture).
    VkImage cfaImage = VK_NULL_HANDLE;
    VkImageView cfaView = VK_NULL_HANDLE;
    // Set when estimateNoiseFromBurst produced the profile this run merged with.
    bool noiseEstimated = false;
    rawr::raw_merge_common::CfaNoiseProfile estimatedNoise{};
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
    // Makes frames[i].raw usable (idempotent): run() calls it right before a
    // frame's first use, so a single-pass merge holds only the reference and
    // the companion in flight (FrameConsumed then frees it). Multi-pass merges
    // load every frame first. Unset: frames are resident.
    using FrameLoad = std::function<void(std::uint32_t)>;
    // Debug/verification hook for the Rexture merge: called after each stage group has finished on the GPU (the
    // arena's images then hold that stage's results). Unset in production.
    using RextureObserver = std::function<void(const char* stage, std::uint32_t frame, const ResourceArena&)>;
    // Set before initialize(): an observer also makes the arena include the verification-only debug images.
    void setRextureObserver(RextureObserver observer) {
        rextureObserver_ = std::move(observer);
        rextureDebug_ = static_cast<bool>(rextureObserver_);
    }
    AndroidBurstCoordinator() = default;
    ~AndroidBurstCoordinator() { reset(); }
    AndroidBurstCoordinator(const AndroidBurstCoordinator&) = delete;
    AndroidBurstCoordinator& operator=(const AndroidBurstCoordinator&) = delete;
    void initialize(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily, Submit submit,
                    std::uint32_t width, std::uint32_t height, StageSink stageSink = {},
                    rawr::raw_merge_common::Config merge = {}, MergeAlgorithm algorithm = MergeAlgorithm::Rexture,
                    rawr::raw_merge_hdrplus_gpu::Config hdrplus = {});
    // Pre-creates pipelines, command pool, fence and timestamp pool on a
    // background thread so the first burst skips cold init. Arena images
    // (~1.3GB) stay lazy. Safe to call before initialize(); initialize()
    // reuses the warmed infrastructure. Never throws-fatal: callers log.
    void warmPipelines(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily);
    // warmPipelines plus arena creation for the last-used merge algorithm and
    // output scale . Makes even the first burst fully warm when geometry
    // matches. Holds ~1.3GB while the camera is open; reset()/resetConfiguration
    // release it. Same reuse rules as initialize().
    // Merge algorithm id (MergeAlgorithm) the next warmAll / burst will lay its arena out for.
    std::uint32_t nextAlgorithmId() const noexcept { return static_cast<std::uint32_t>(lastAlgorithm_); }
    void warmAll(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily, std::uint32_t width,
                 std::uint32_t height);
    BurstRunResult run(const std::vector<BurstFrame>& frames, std::uint32_t referenceIndex,
                       FrameConsumed frameConsumed = {}, FrameLoad frameLoad = {});
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
    Submit submit_{};
    StageSink stageSink_{};
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore initialWait_ = VK_NULL_HANDLE;
    // Last merge selection seen (survives reset: self-tuning warmup heuristic, so warmAll builds the arena the next burst
    // will use).
    MergeAlgorithm lastAlgorithm_ = MergeAlgorithm::Rexture;
    rawr::raw_merge_hdrplus_gpu::Config lastHdrPlus_{};
    // Guards initialize/warmPipelines/reset (pipelines + pools + handles).
    // run() is called after initialize() returns on the same worker.
    std::mutex initMutex_;
    std::mutex runMutex_;  // held for a whole run(): the warm-up must not replace the arena under a merge
    bool executionReady_ = false;
    void resetLocked() noexcept;
    void resetInfraLocked() noexcept;
    void ensureExecutionInfra(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily);
    VkQueryPool timestampPool_ = VK_NULL_HANDLE;
    float timestampPeriodNs_ = 0.0f;
    bool timestampsSupported_ = false;
    ResourceArena arena_{};
    VulkanExecutor executor_{};
    rawr::raw_merge_common::Config merge_{};
    MergeAlgorithm algorithm_ = MergeAlgorithm::Rexture;
    rawr::raw_merge_hdrplus_gpu::Config hdrplus_{};
    // Which layout arena_ currently holds (HDR+ geometry depends on its config).
    MergeAlgorithm arenaAlgorithm_ = MergeAlgorithm::Rexture;
    rawr::raw_merge_hdrplus_gpu::Config arenaHdrPlus_{};
    [[nodiscard]] bool arenaMatchesSelection() const noexcept;
    void initializeArenaLocked(VkPhysicalDevice physical, VkDevice device);
    // One recorded submission, fence-waited; returns wall ms and GPU timestamp intervals.
    using ChunkTiming = std::pair<double, std::vector<double>>;
    ChunkTiming executeChunk(std::uint32_t chunkCode, std::uint32_t frameIndex, std::uint32_t queryCount,
                             const std::function<void()>& record);
    void recordTimestamp(std::uint32_t query);
    bool estimateNoiseFromBurst(const std::vector<BurstFrame>& frames, std::uint32_t ref, const FrameLoad& frameLoad,
                                BurstStageTimings& timings);
    RextureObserver rextureObserver_{};
    bool rextureDebug_ = false, arenaRextureDebug_ = false;
    BurstRunResult runRexture(const std::vector<BurstFrame>& frames, std::uint32_t referenceIndex,
                              const FrameConsumed& frameConsumed, const FrameLoad& frameLoad);
    BurstRunResult runHdrPlusFrequency(const std::vector<BurstFrame>& frames, std::uint32_t referenceIndex,
                                       const FrameConsumed& frameConsumed, const FrameLoad& frameLoad);
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
