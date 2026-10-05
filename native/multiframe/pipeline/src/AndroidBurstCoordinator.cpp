#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>
#include <rawr/raw_gpu_pipeline/BurstNoiseEstimate.h>
#include <rawr/raw_gpu_pipeline/HdrPlusRecorder.h>
#include <rawr/raw_gpu_pipeline/MergeMath.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
namespace rawr::raw_gpu_pipeline {
namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(what);
}
struct NoiseEstimatePc {
    float referenceBlack[4];
    float neighbourBlack[4];
    float referenceWhite, neighbourWhite;
    std::int32_t width, height, blocksX;
};
}  // namespace
void AndroidBurstCoordinator::ensureHostBuffer(HostBuffer& hb, VkDeviceSize bytes) {
    if (hb.buffer && hb.bytes >= bytes) return;
    destroyHostBuffer(hb);
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &bi, nullptr, &hb.buffer), "multiframe host buffer");
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, hb.buffer, &mr);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &props);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) {
        destroyHostBuffer(hb);
        throw std::runtime_error("multiframe host buffer: no host-visible memory");
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    check(vkAllocateMemory(device_, &ai, nullptr, &hb.memory), "multiframe host buffer memory");
    check(vkBindBufferMemory(device_, hb.buffer, hb.memory, 0), "multiframe host buffer bind");
    check(vkMapMemory(device_, hb.memory, 0, VK_WHOLE_SIZE, 0, &hb.mapped), "multiframe host buffer map");
    hb.bytes = bytes;
}
void AndroidBurstCoordinator::destroyHostBuffer(HostBuffer& hb) noexcept {
    if (!device_) return;
    if (hb.mapped) vkUnmapMemory(device_, hb.memory);
    if (hb.buffer) vkDestroyBuffer(device_, hb.buffer, nullptr);
    if (hb.memory) vkFreeMemory(device_, hb.memory, nullptr);
    hb = {};
}
void AndroidBurstCoordinator::destroyHostBuffers() noexcept {
    destroyHostBuffer(noiseStats_);
    destroyHostBuffer(hotPixels_);
}
void AndroidBurstCoordinator::warmPipelines(VkPhysicalDevice physical, VkDevice device, std::uint32_t qf) {
    if (!physical || !device) throw std::invalid_argument("multiframe burst: invalid warmup handles");
    std::lock_guard<std::mutex> lock(initMutex_);
    if (!(executionReady_ && device_ == device && queueFamily_ == qf)) {
        // New (or no) device: everything device-bound, including any arena, is stale.
        resetLocked();
    }
    ensureExecutionInfra(physical, device, qf);
}

void AndroidBurstCoordinator::warmAll(VkPhysicalDevice physical, VkDevice device, std::uint32_t qf, std::uint32_t w,
                                      std::uint32_t h) {
    if (!physical || !device || !w || !h) throw std::invalid_argument("multiframe burst: invalid warmup handles");
    std::lock_guard<std::mutex> lock(initMutex_);
    if (!(executionReady_ && device_ == device && queueFamily_ == qf)) {
        resetLocked();
    }
    ensureExecutionInfra(physical, device, qf);
    algorithm_ = lastAlgorithm_;
    hdrplus_ = lastHdrPlus_;
    const float scale = algorithm_ != MergeAlgorithm::Wronski ? 1.0f : lastScale_;
    if (arenaReady_ && width_ == w && height_ == h && std::abs(outputScale_ - scale) <= 1.0e-6f &&
        arenaMatchesSelection())
        return;
    width_ = w;
    height_ = h;
    outputScale_ = scale;
    initializeArenaLocked(physical, device);
    arenaReady_ = true;
    layoutsInitialized_ = false;
}

void AndroidBurstCoordinator::resetInfraLocked() noexcept {
    destroyHostBuffers();
    executor_.reset();
    if (device_ && timestampPool_) vkDestroyQueryPool(device_, timestampPool_, nullptr);
    if (device_ && fence_) vkDestroyFence(device_, fence_, nullptr);
    if (device_ && commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
    commandPool_ = VK_NULL_HANDLE;
    command_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    initialWait_ = VK_NULL_HANDLE;
    executionReady_ = false;
    timestampPool_ = VK_NULL_HANDLE;
    timestampPeriodNs_ = 0.0f;
    timestampsSupported_ = false;
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queueFamily_ = 0;
}

void AndroidBurstCoordinator::ensureExecutionInfra(VkPhysicalDevice physical, VkDevice device, std::uint32_t qf) {
    if (executionReady_ && device_ == device && queueFamily_ == qf) return;
    resetInfraLocked();
    physical_ = physical;
    device_ = device;
    queueFamily_ = qf;
    executor_.initialize(device, false);
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = qf;
    check(vkCreateCommandPool(device, &pi, nullptr, &commandPool_), "multiframe command pool");
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &ai, &command_), "multiframe command buffer");
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device, &fi, nullptr, &fence_), "multiframe fence");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    timestampsSupported_ = qf < familyCount && families[qf].timestampValidBits != 0u;
    timestampPeriodNs_ = properties.limits.timestampPeriod;
    if (timestampsSupported_) {
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 8u;
        check(vkCreateQueryPool(device, &qi, nullptr, &timestampPool_), "multiframe timestamp query pool");
    }
    executionReady_ = true;
}

bool AndroidBurstCoordinator::arenaMatchesSelection() const noexcept {
    if (arenaAlgorithm_ != algorithm_) return false;
    return algorithm_ == MergeAlgorithm::Wronski || (arenaHdrPlus_.tileSize == hdrplus_.tileSize &&
                                                     arenaHdrPlus_.searchDistance == hdrplus_.searchDistance);
}
void AndroidBurstCoordinator::initializeArenaLocked(VkPhysicalDevice physical, VkDevice device) {
    if (algorithm_ == MergeAlgorithm::HdrPlusSpatial) {
        arena_.initialize(physical, device,
                          makeHdrPlusScratchLayout(rawr::raw_merge_hdrplus_gpu::makeGeometry(width_, height_, hdrplus_)));
        arenaHdrPlus_ = hdrplus_;
    } else if (algorithm_ == MergeAlgorithm::HdrPlusFrequency) {
        arena_.initialize(physical, device,
                          makeHdrPlusFrequencyScratchLayout(
                              rawr::raw_merge_hdrplus_gpu::makeFrequencyGeometry(width_, height_, hdrplus_), width_, height_));
        arenaHdrPlus_ = hdrplus_;
    } else {
        arena_.initialize(physical, device, makeMultiframeScratchLayout(makeMultiframeGeometry(width_, height_, outputScale_)));
    }
    arenaAlgorithm_ = algorithm_;
}
void AndroidBurstCoordinator::initialize(VkPhysicalDevice physical, VkDevice device, std::uint32_t qf, Submit submit,
                                         std::uint32_t w, std::uint32_t h, float scale, StageSink stageSink,
                                         rawr::raw_alignment_gpu::Config alignment,
                                         rawr::raw_merge_wronski_gpu::Config merge, MergeAlgorithm algorithm,
                                         rawr::raw_merge_hdrplus_gpu::Config hdrplus) {
    if (!physical || !device || !submit || !w || !h)
        throw std::invalid_argument("multiframe burst: invalid initialize");
    // HDR+ merges on the RAW grid only.
    if (algorithm != MergeAlgorithm::Wronski) {
        scale = 1.0f;
        if (!rawr::raw_merge_hdrplus_gpu::valid(hdrplus))
            throw std::invalid_argument("multiframe burst: invalid HDR+ tuning");
    }
    merge.outputScale = scale;
    if (!rawr::raw_alignment_gpu::valid(alignment) ||
        !(std::isfinite(alignment.hessianEpsilon) && alignment.hessianEpsilon > 0.0f) ||
        !rawr::raw_merge_wronski_gpu::valid(merge))
        throw std::invalid_argument("multiframe burst: invalid tuning");
    std::lock_guard<std::mutex> lock(initMutex_);
    if (algorithm == MergeAlgorithm::Wronski) lastScale_ = scale;
    lastAlgorithm_ = algorithm;
    lastHdrPlus_ = hdrplus;
    if (device_ == device && physical_ == physical && queueFamily_ == qf && width_ == w && height_ == h &&
        std::abs(outputScale_ - scale) <= 1.0e-6f) {
        alignment_ = alignment;
        merge_ = merge;
        algorithm_ = algorithm;
        hdrplus_ = hdrplus;
        submit_ = std::move(submit);
        stageSink_ = std::move(stageSink);
        pipelineCacheReused_ = executionReady_;
        if (arenaReady_ && !arenaMatchesSelection()) {
            arena_.reset();
            arenaReady_ = false;
            layoutsInitialized_ = false;
        }
        resourcesReused_ = arenaReady_;
        if (!arenaReady_) {
            const auto initializeBegin = std::chrono::steady_clock::now();
            if (stageSink_) stageSink_(Stage::ArenaInitializeBegin, 0, 0);
            initializeArenaLocked(physical, device);
            arenaReady_ = true;
            layoutsInitialized_ = false;
            if (stageSink_)
                stageSink_(Stage::ArenaInitializeEnd, arena_.physicalImageBytes(), arena_.physicalBufferBytes());
            pendingInitializeMs_ = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - initializeBegin)
                                       .count();
        } else {
            pendingInitializeMs_ = 0.0;
        }
        return;
    }
    // Full path: geometry (arena) always rebuilt. Warmed pipelines/pools on the
    // same device are preserved; everything else is torn down. Stale semaphore
    // arms are always cleared (a previous failed run must not inject a wait).
    if (device_ && fence_ && submissionInFlight_) {
        (void)vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        submissionInFlight_ = false;
    }
    const bool warmedInfra = executionReady_ && device_ == device && physical_ == physical && queueFamily_ == qf;
    if (!warmedInfra) resetInfraLocked();
    initialWait_ = VK_NULL_HANDLE;
    arena_.reset();
    arenaReady_ = false;
    layoutsInitialized_ = false;
    submissionInFlight_ = false;
    resourcesReused_ = false;
    pipelineCacheReused_ = false;
    pendingInitializeMs_ = 0.0;
    submit_ = {};
    stageSink_ = {};
    const auto initializeBegin = std::chrono::steady_clock::now();
    alignment_ = alignment;
    merge_ = merge;
    algorithm_ = algorithm;
    hdrplus_ = hdrplus;
    submit_ = std::move(submit);
    stageSink_ = std::move(stageSink);
    width_ = w;
    height_ = h;
    outputScale_ = scale;
    if (stageSink_) stageSink_(Stage::ArenaInitializeBegin, 0, 0);
    ensureExecutionInfra(physical, device, qf);
    initializeArenaLocked(physical, device);
    arenaReady_ = true;
    if (stageSink_) stageSink_(Stage::ArenaInitializeEnd, arena_.physicalImageBytes(), arena_.physicalBufferBytes());
    resourcesReused_ = false;
    pipelineCacheReused_ = warmedInfra;
    pendingInitializeMs_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - initializeBegin).count();
}
AndroidBurstCoordinator::ChunkTiming AndroidBurstCoordinator::executeChunk(std::uint32_t chunkCode,
                                                                          std::uint32_t frameIndex,
                                                                          std::uint32_t queryCount,
                                                                          const std::function<void()>& record) {
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    if (stageSink_) stageSink_(Stage::ChunkBegin, chunkCode, frameIndex);
    try {
        if (submissionInFlight_) {
            const VkResult prior = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
            submissionInFlight_ = false;
            check(prior, "multiframe prior fence");
        }
        check(vkResetCommandBuffer(command_, 0), "multiframe reset command buffer");
        executor_.beginBatch();
        if (stageSink_) stageSink_(Stage::RecordBegin, arena_.physicalImageBytes(), arena_.physicalBufferBytes());
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &bi), "multiframe begin");
        if (timestampsSupported_) vkCmdResetQueryPool(command_, timestampPool_, 0u, queryCount);
        record();
        check(vkEndCommandBuffer(command_), "multiframe end");
        if (stageSink_) stageSink_(Stage::RecordEnd, arena_.physicalImageBytes(), arena_.physicalBufferBytes());
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &command_;
        // One-shot cross-queue edge (e.g. prior-queue writes to borrowed
        // inputs before a multiframe queue takes over). Consumed once.
        // NB: stage/semaphore must be locals: pWaitSemaphores must stay
        // valid through submit_, so they cannot alias the cleared member.
        VkSemaphore initialWait = initialWait_;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        initialWait_ = VK_NULL_HANDLE;
        if (initialWait != VK_NULL_HANDLE) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &initialWait;
            si.pWaitDstStageMask = &waitStage;
        }
        check(vkResetFences(device_, 1, &fence_), "multiframe reset fence");
        if (stageSink_) stageSink_(Stage::SubmitBegin, arena_.physicalImageBytes(), arena_.physicalBufferBytes());
        submit_(si, fence_);
        submissionInFlight_ = true;
        if (stageSink_) stageSink_(Stage::SubmitEnd, arena_.physicalImageBytes(), arena_.physicalBufferBytes());
        if (stageSink_)
            stageSink_(Stage::FenceWaitBegin, arena_.physicalImageBytes(), arena_.physicalBufferBytes());
        const VkResult executed = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        submissionInFlight_ = false;
        if (stageSink_) stageSink_(Stage::FenceWaitEnd, arena_.physicalImageBytes(), arena_.physicalBufferBytes());
        check(executed, "multiframe execute");
    } catch (const std::exception& e) {
        throw std::runtime_error("multiframe chunk=" + std::to_string(chunkCode) +
                                 " frame=" + std::to_string(frameIndex) + ": " + e.what());
    }
    if (stageSink_) stageSink_(Stage::ChunkEnd, chunkCode, frameIndex);
    std::vector<double> gpuIntervals;
    if (timestampsSupported_ && queryCount > 1u) {
        std::array<std::uint64_t, 8> values{};
        check(vkGetQueryPoolResults(device_, timestampPool_, 0u, queryCount, queryCount * sizeof(values[0]),
                                    values.data(), sizeof(values[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "multiframe timestamp results");
        gpuIntervals.reserve(queryCount - 1u);
        for (std::uint32_t i = 1u; i < queryCount; ++i) {
            gpuIntervals.push_back(double(values[i] - values[i - 1u]) * double(timestampPeriodNs_) / 1.0e6);
        }
    }
    const double wall = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    return std::pair<double, std::vector<double>>(wall, std::move(gpuIntervals));
}
void AndroidBurstCoordinator::recordTimestamp(std::uint32_t query) {
    if (timestampsSupported_)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool_, query);
}
BurstRunResult AndroidBurstCoordinator::run(const std::vector<BurstFrame>& frames, std::uint32_t ref,
                                            FrameConsumed frameConsumed) {
    if (!device_ || !arenaReady_ || frames.size() < 2 || ref >= frames.size())
        throw std::invalid_argument("multiframe burst: invalid run");
    if (frames[ref].raw.ref.width != width_ || frames[ref].raw.ref.height != height_)
        throw std::invalid_argument("multiframe burst: RAW geometry mismatch");
    // Chunk codes are stable trace ABI for device diagnostics. Companion work
    // deliberately remains split at scheduling boundaries so preview submits can
    // interleave on the shared graphics queue. Alignment is additionally split
    // per coarse-to-fine level (chunk 11 repeats): a whole companion's
    // alignment holds the GPU for ~35ms, starving 30/60fps preview, while a
    // single level holds it for ~1-20ms. Recorded commands are unchanged.
    // 1=reference, 11=prepare/alignment, 12=kernel/stats,
    // 13=robustness, 14=accumulate, 20=finalize.
    using Clock = std::chrono::steady_clock;
    const auto runBegin = Clock::now();
    BurstStageTimings timings{};
    timings.initializeMs = pendingInitializeMs_;
    const auto timestamp = [&](std::uint32_t query) { recordTimestamp(query); };
    if (algorithm_ == MergeAlgorithm::HdrPlusSpatial) return runHdrPlus(frames, ref, frameConsumed);
    if (algorithm_ == MergeAlgorithm::HdrPlusFrequency) return runHdrPlusFrequency(frames, ref, frameConsumed);
    // Burst noise estimation: the profile it fits drives everything below
    // (robustness LUT, kernel GAT, aperture gate), so it runs first. The
    // configured profile is restored after the run (also on failure).
    struct RestoreMerge {
        rawr::raw_merge_wronski_gpu::Config& target;
        rawr::raw_merge_wronski_gpu::Config saved;
        ~RestoreMerge() { target = saved; }
    } restoreMerge{merge_, merge_};
    bool noiseEstimated = false;
    if (merge_.estimateNoiseFromBurst) {
        const std::uint32_t neighbour = ref + 1u < frames.size() ? ref + 1u : ref - 1u;
        const std::uint32_t blocksX = (width_ + kNoiseBlockSize - 1u) / kNoiseBlockSize;
        const std::uint32_t blocksY = (height_ + kNoiseBlockSize - 1u) / kNoiseBlockSize;
        ensureHostBuffer(noiseStats_, VkDeviceSize(blocksX) * blocksY * kNoiseBlockFloats * sizeof(float));
        NoiseEstimatePc pc{};
        for (std::size_t s = 0; s < 4u; ++s) {
            pc.referenceBlack[s] = frames[ref].parameters.normalization.blackByPhase[s];
            pc.neighbourBlack[s] = frames[neighbour].parameters.normalization.blackByPhase[s];
        }
        pc.referenceWhite = frames[ref].parameters.normalization.whiteLevel;
        pc.neighbourWhite = frames[neighbour].parameters.normalization.whiteLevel;
        pc.width = std::int32_t(width_);
        pc.height = std::int32_t(height_);
        pc.blocksX = std::int32_t(blocksX);
        const auto noiseTiming = executeChunk(2u, neighbour, 2u, [&] {
            timestamp(0u);
            executor_.record(command_, ShaderId::NoiseEstimate,
                             {{{0u, frames[ref].raw.view}, {1u, frames[neighbour].raw.view}},
                              {{2u, noiseStats_.buffer}}},
                             &pc, sizeof(pc), blocksX, blocksY, 1u);
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                 &toHost, 0, nullptr, 0, nullptr);
            timestamp(1u);
        });
        timings.noiseEstimateMs = noiseTiming.second.empty() ? noiseTiming.first : noiseTiming.second.front();
        const auto fit = fitBurstNoise(static_cast<const float*>(noiseStats_.mapped), blocksX * blocksY);
        if (fit) {
            merge_.sensorNoiseProfile = fit->profile;
            merge_.highSignalNoiseProfile.reset();
            merge_.noiseKneeBySite.fill(0.0f);
            noiseEstimated = true;
        }
    }
    std::array<float, 3u * 1001u> stdc{}, diffc{};
    auto siteVariance = [&](std::uint32_t site, float x) {
        if (!merge_.sensorNoiseProfile) return merge_.noiseAlpha * x + merge_.noiseBeta;
        const auto* profile = &*merge_.sensorNoiseProfile;
        if (merge_.highSignalNoiseProfile && merge_.noiseKneeBySite[site] > 0.0f && x >= merge_.noiseKneeBySite[site])
            profile = &*merge_.highSignalNoiseProfile;
        return profile->slopeBySite[site] * x + profile->offsetBySite[site];
    };
    std::array<std::uint32_t, 4> physical{{0u, 1u, 2u, 3u}};
    if (merge_.sensorNoiseProfile) {
        switch (merge_.cfa) {
            case rawr::raw_merge_wronski_gpu::CfaPattern::RGGB:
                physical = {0u, 1u, 2u, 3u};
                break;
            case rawr::raw_merge_wronski_gpu::CfaPattern::GRBG:
                physical = {1u, 0u, 3u, 2u};
                break;
            case rawr::raw_merge_wronski_gpu::CfaPattern::GBRG:
                physical = {2u, 3u, 0u, 1u};
                break;
            case rawr::raw_merge_wronski_gpu::CfaPattern::BGGR:
                physical = {3u, 2u, 1u, 0u};
                break;
        }
    }
    for (std::size_t i = 0; i < 1001u; ++i) {
        const float x = float(i) / 1000.f;
        const std::array<float, 3> variance{{siteVariance(physical[0], x),
                                             .25f * (siteVariance(physical[1], x) + siteVariance(physical[2], x)),
                                             siteVariance(physical[3], x)}};
        const float sqrtBrightness = std::sqrt(x);
        const auto noise = mergemath::aggregateRobustnessNoise(
            mergemath::sqrtDomainNoiseVariance(sqrtBrightness, variance[0]),
            mergemath::sqrtDomainNoiseVariance(sqrtBrightness, variance[1]),
            mergemath::sqrtDomainNoiseVariance(sqrtBrightness, variance[2]));
        // Aggregate brightness-indexed LUT contract. Only the first plane is
        // consumed; repeating it keeps the fixed descriptor/buffer layout.
        for (std::size_t c = 0; c < 3; ++c) {
            stdc[c * 1001u + i] = noise.sigmaSquared;
            diffc[c * 1001u + i] = noise.distanceSquared;
        }
    }
    // The merge path owns a full-height output accumulator. At 1x this happened
    // to equal the RAW height; scaled output must use the reconstructed extent or
    // the recorder incorrectly classifies the full allocation as a stripe.
    const std::uint32_t reconstructionHeight = arena_.image("linear_output").extent.height;
    MultiframeRecorder recorder(arena_, executor_, alignment_, merge_, reconstructionHeight);
    {
        // Sensor hot-pixel list, shared by every frame of the burst (one
        // sensor, one map). Capped defensively; Camera2 maps are small.
        const std::size_t pairs = std::min<std::size_t>(merge_.hotPixels.size() / 2u, 1u << 20);
        if (pairs > 0u) {
            ensureHostBuffer(hotPixels_, VkDeviceSize(pairs) * 2u * sizeof(std::int32_t));
            std::memcpy(hotPixels_.mapped, merge_.hotPixels.data(), pairs * 2u * sizeof(std::int32_t));
            recorder.setHotPixels(hotPixels_.buffer, std::uint32_t(pairs));
        }
    }
    const bool initializeLayouts = !layoutsInitialized_;
    const auto referenceTiming = executeChunk(1u, ref, 3u, [&] {
        timestamp(0u);
        if (initializeLayouts) arena_.recordInitializeLayouts(command_);
        arena_.recordClearBurstState(command_);
        recorder.recordUploadNoiseCurves(command_, stdc.data(), diffc.data());
        recorder.recordReferencePrepare(command_, frames[ref].raw.view, frames[ref].parameters);
        timestamp(1u);
        recorder.recordReferenceStats(command_, frames[ref].parameters);
        timestamp(2u);
    });
    if (initializeLayouts) layoutsInitialized_ = true;
    if (referenceTiming.second.size() == 2u) {
        timings.referencePrepareMs = referenceTiming.second[0];
        timings.referenceStatsMs = referenceTiming.second[1];
    } else {
        timings.referencePrepareMs = referenceTiming.first;
    }
    std::uint32_t companions = 0;
    for (std::uint32_t i = 0; i < frames.size(); ++i) {
        if (i == ref) continue;
        if (frames[i].raw.ref.width != width_ || frames[i].raw.ref.height != height_)
            throw std::invalid_argument("multiframe burst: RAW geometry mismatch");
        const auto prepareTiming = executeChunk(11u, i, 2u, [&] {
            timestamp(0u);
            recorder.recordCompanionPrepare(command_, frames[i].raw.view, frames[i].parameters);
            timestamp(1u);
        });
        if (prepareTiming.second.size() == 1u) {
            timings.companionPrepareMs += prepareTiming.second.front();
        } else {
            timings.companionPrepareMs += prepareTiming.first;
        }
        for (std::uint32_t l = 0; l < 4; ++l) {
            const auto levelTiming = executeChunk(11u, i, 2u, [&] {
                timestamp(0u);
                recorder.recordCompanionAlignmentLevel(command_, l);
                timestamp(1u);
            });
            if (levelTiming.second.size() == 1u) {
                timings.alignmentMs += levelTiming.second.front();
            } else {
                timings.alignmentMs += levelTiming.first;
            }
        }
        const auto statsTiming = executeChunk(12u, i, 2u, [&] {
            timestamp(0u);
            recorder.recordCompanionStats(command_, frames[i].parameters);
            timestamp(1u);
        });
        timings.kernelStatsMs += statsTiming.second.empty() ? statsTiming.first : statsTiming.second.front();
        const auto robustnessTiming = executeChunk(13u, i, 2u, [&] {
            timestamp(0u);
            recorder.recordCompanionRobustness(command_);
            timestamp(1u);
        });
        timings.robustnessMs +=
            robustnessTiming.second.empty() ? robustnessTiming.first : robustnessTiming.second.front();
        const auto accumulationTiming = executeChunk(14u, i, 2u, [&] {
            timestamp(0u);
            recorder.recordCompanionAccumulate(command_);
            timestamp(1u);
        });
        timings.accumulationMs +=
            accumulationTiming.second.empty() ? accumulationTiming.first : accumulationTiming.second.front();
        ++companions;
        if (frameConsumed) frameConsumed(i);
    }
    const auto finalizeTiming = executeChunk(20u, ref, 2u, [&] {
        timestamp(0u);
        recorder.recordFinalize(command_, companions);
        timestamp(1u);
    });
    timings.finalizeMs = finalizeTiming.second.empty() ? finalizeTiming.first : finalizeTiming.second.front();
    timings.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - runBegin).count();
    const auto& out = arena_.image("linear_output");
    BurstRunResult result{ref,
            std::uint32_t(frames.size()),
            out.image,
            out.view,
            out.format,
            out.extent,
            arena_.physicalImageBytes(),
            arena_.physicalBufferBytes(),
            static_cast<std::uint32_t>(2u + companions * 8u),
            resourcesReused_,
            pipelineCacheReused_,
            timings};
    result.noiseEstimated = noiseEstimated;
    if (noiseEstimated) result.estimatedNoise = *merge_.sensorNoiseProfile;
    return result;
}
BurstRunResult AndroidBurstCoordinator::runHdrPlus(const std::vector<BurstFrame>& frames, std::uint32_t ref,
                                                   const FrameConsumed& frameConsumed) {
    // Chunks reuse the Wronski trace codes: 1=reference, 11=prepare and
    // alignment (two submissions), 14=merge/accumulate, 20=finalize. Splitting
    // each companion lets preview work interleave on a shared queue.
    using Clock = std::chrono::steady_clock;
    const auto runBegin = Clock::now();
    BurstStageTimings timings{};
    timings.initializeMs = pendingInitializeMs_;
    const auto gpuOrWall = [](const ChunkTiming& t) { return t.second.empty() ? t.first : t.second.front(); };
    const auto geometry = rawr::raw_merge_hdrplus_gpu::makeGeometry(width_, height_, hdrplus_);
    HdrPlusRecorder recorder(arena_, executor_, hdrplus_, geometry);
    const std::size_t pairs = std::min<std::size_t>(merge_.hotPixels.size() / 2u, 1u << 20);
    if (pairs > 0u) {
        ensureHostBuffer(hotPixels_, VkDeviceSize(pairs) * 2u * sizeof(std::int32_t));
        std::memcpy(hotPixels_.mapped, merge_.hotPixels.data(), pairs * 2u * sizeof(std::int32_t));
        recorder.setHotPixels(hotPixels_.buffer, std::uint32_t(pairs));
    }
    const auto frameCount = std::uint32_t(frames.size());
    const bool initializeLayouts = !layoutsInitialized_;
    timings.referencePrepareMs = gpuOrWall(executeChunk(1u, ref, 2u, [&] {
        recordTimestamp(0u);
        if (initializeLayouts) arena_.recordInitializeLayouts(command_);
        recorder.recordReference(command_, frames[ref].raw.view, frames[ref].parameters.normalization, frameCount);
        recordTimestamp(1u);
    }));
    if (initializeLayouts) layoutsInitialized_ = true;
    std::uint32_t companions = 0;
    for (std::uint32_t i = 0; i < frameCount; ++i) {
        if (i == ref) continue;
        if (frames[i].raw.ref.width != width_ || frames[i].raw.ref.height != height_)
            throw std::invalid_argument("multiframe burst: RAW geometry mismatch");
        timings.companionPrepareMs += gpuOrWall(executeChunk(11u, i, 2u, [&] {
            recordTimestamp(0u);
            recorder.recordCompanionPrepare(command_, frames[i].raw.view, frames[i].parameters.normalization);
            recordTimestamp(1u);
        }));
        for (std::uint32_t level = recorder.levelCount(); level-- > 0;) {
            const auto levelTiming = executeChunk(11u, i, 4u, [&] {
                recordTimestamp(0u);
                recorder.recordCompanionAlignLevel(command_, level, [&](std::uint32_t k) { recordTimestamp(k); });
                recordTimestamp(3u);
            });
            double ms = levelTiming.first;
            if (!levelTiming.second.empty()) {
                ms = 0.0;
                for (const double part : levelTiming.second) ms += part;
            }
            timings.alignmentMs += ms;
            if (std::getenv("RAWR_HDRP_PROFILE") && levelTiming.second.size() == 3u)
                std::fprintf(stderr, "hdrp_profile frame=%u level=%u ms=%.3f (correct=%.3f cost=%.3f best=%.3f)\n", i,
                             level, ms, levelTiming.second[0], levelTiming.second[1], levelTiming.second[2]);
        }
        const auto mergeTiming = executeChunk(14u, i, 5u, [&] {
            recordTimestamp(0u);
            recorder.recordCompanionMerge(command_, frameCount, [&](std::uint32_t k) { recordTimestamp(k); });
            recordTimestamp(4u);
        });
        double mergeMs = mergeTiming.first;
        if (!mergeTiming.second.empty()) {
            mergeMs = 0.0;
            for (const double ms : mergeTiming.second) mergeMs += ms;
            if (std::getenv("RAWR_HDRP_PROFILE") && mergeTiming.second.size() == 4u)
                std::fprintf(stderr, "hdrp_profile frame=%u warp=%.3f blur=%.3f weight=%.3f accumulate=%.3f\n", i,
                             mergeTiming.second[0], mergeTiming.second[1], mergeTiming.second[2], mergeTiming.second[3]);
        }
        timings.accumulationMs += mergeMs;
        ++companions;
        if (frameConsumed) frameConsumed(i);
    }
    timings.finalizeMs = gpuOrWall(executeChunk(20u, ref, 2u, [&] {
        recordTimestamp(0u);
        recorder.recordFinalize(command_);
        recordTimestamp(1u);
    }));
    timings.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - runBegin).count();
    const auto& out = arena_.image("hdrp_output");
    const auto& cfa = arena_.image("hdrp_cfa");
    BurstRunResult result{ref,
                          frameCount,
                          out.image,
                          out.view,
                          out.format,
                          out.extent,
                          arena_.physicalImageBytes(),
                          arena_.physicalBufferBytes(),
                          2u + companions * (2u + recorder.levelCount()),
                          resourcesReused_,
                          pipelineCacheReused_,
                          timings};
    result.cfaImage = cfa.image;
    result.cfaView = cfa.view;
    return result;
}
BurstRunResult AndroidBurstCoordinator::runHdrPlusFrequency(const std::vector<BurstFrame>& frames, std::uint32_t ref,
                                                            const FrameConsumed& frameConsumed) {
    // Four half-tile-shifted passes. Companions are aligned in pass 0 only
    // (align-once, default) or re-aligned on each pass's padding (upstream).
    // Chunk codes: 1=pass reference, 11=companion prepare/alignment,
    // 14=frequency merge, 15=pass finish, 20=finalize. Ring frames are
    // consumed only after the last pass.
    using Clock = std::chrono::steady_clock;
    const auto runBegin = Clock::now();
    BurstStageTimings timings{};
    timings.initializeMs = pendingInitializeMs_;
    const auto gpuOrWall = [](const ChunkTiming& t) { return t.second.empty() ? t.first : t.second.front(); };
    const auto geometry = rawr::raw_merge_hdrplus_gpu::makeFrequencyGeometry(width_, height_, hdrplus_);
    HdrPlusFrequencyRecorder recorder(arena_, executor_, hdrplus_, geometry);
    const std::size_t pairs = std::min<std::size_t>(merge_.hotPixels.size() / 2u, 1u << 20);
    if (pairs > 0u) {
        ensureHostBuffer(hotPixels_, VkDeviceSize(pairs) * 2u * sizeof(std::int32_t));
        std::memcpy(hotPixels_.mapped, merge_.hotPixels.data(), pairs * 2u * sizeof(std::int32_t));
        recorder.alignment().setHotPixels(hotPixels_.buffer, std::uint32_t(pairs));
    }
    const auto frameCount = std::uint32_t(frames.size());
    for (std::uint32_t i = 0; i < frameCount; ++i)
        if (frames[i].raw.ref.width != width_ || frames[i].raw.ref.height != height_)
            throw std::invalid_argument("multiframe burst: RAW geometry mismatch");
    std::uint32_t companions = 0;
    for (std::uint32_t pass = 0; pass < 4u; ++pass) {
        recorder.beginPass(pass);
        const bool initializeLayouts = !layoutsInitialized_;
        timings.referencePrepareMs += gpuOrWall(executeChunk(1u, ref, 2u, [&] {
            recordTimestamp(0u);
            if (initializeLayouts) arena_.recordInitializeLayouts(command_);
            recorder.recordReference(command_, frames[ref].raw.view, frames[ref].parameters.normalization);
            recordTimestamp(1u);
        }));
        if (initializeLayouts) layoutsInitialized_ = true;
        for (std::uint32_t i = 0; i < frameCount; ++i) {
            if (i == ref) continue;
            // One submission: prepare + all alignment levels (+ shift store) when
            // this pass aligns, otherwise only the padded frame.
            const bool aligns = recorder.alignsThisPass();
            const double alignMs = gpuOrWall(executeChunk(11u, i, 2u, [&] {
                recordTimestamp(0u);
                recorder.recordCompanionAlign(command_, frames[i].raw.view, frames[i].parameters.normalization, i);
                recordTimestamp(1u);
            }));
            (aligns ? timings.alignmentMs : timings.companionPrepareMs) += alignMs;
            const bool profile = std::getenv("RAWR_HDRP_PROFILE") != nullptr;
            // Query order: 0 start, 1 warp, [2 mismatch when profiling], then fft, norm, merge.
            const auto mergeTiming = executeChunk(14u, i, profile ? 6u : 5u, [&] {
                recordTimestamp(0u);
                recorder.recordCompanionMerge(command_, frameCount, i, [&](std::uint32_t k) {
                    if (k == 5u) {
                        if (profile) recordTimestamp(2u);  // mismatch boundary, profiling only
                    } else {
                        recordTimestamp(profile && k >= 2u ? k + 1u : k);
                    }
                });
                recordTimestamp(profile ? 5u : 4u);
            });
            double mergeMs = mergeTiming.first;
            if (!mergeTiming.second.empty()) {
                mergeMs = 0.0;
                for (const double ms : mergeTiming.second) mergeMs += ms;
            }
            timings.accumulationMs += mergeMs;
            if (profile && mergeTiming.second.size() == 5u)
                std::fprintf(stderr, "hdrq_profile pass=%u frame=%u warp=%.3f mismatch=%.3f fft=%.3f norm=%.3f merge=%.3f\n",
                             pass, i, mergeTiming.second[0], mergeTiming.second[1], mergeTiming.second[2],
                             mergeTiming.second[3], mergeTiming.second[4]);
            if (pass == 3u) {
                ++companions;
                if (frameConsumed) frameConsumed(i);
            }
        }
        timings.finalizeMs += gpuOrWall(executeChunk(15u, ref, 2u, [&] {
            recordTimestamp(0u);
            recorder.recordPassFinish(command_, frameCount);
            recordTimestamp(1u);
        }));
    }
    timings.finalizeMs += gpuOrWall(executeChunk(20u, ref, 2u, [&] {
        recordTimestamp(0u);
        recorder.recordFinalize(command_);
        recordTimestamp(1u);
    }));
    timings.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - runBegin).count();
    const auto& out = arena_.image("hdrp_output");
    const auto& cfa = arena_.image("hdrp_cfa");
    BurstRunResult result{ref,
                          frameCount,
                          out.image,
                          out.view,
                          out.format,
                          out.extent,
                          arena_.physicalImageBytes(),
                          arena_.physicalBufferBytes(),
                          4u * (2u + companions * 2u) + 1u,
                          resourcesReused_,
                          pipelineCacheReused_,
                          timings};
    result.cfaImage = cfa.image;
    result.cfaView = cfa.view;
    return result;
}
void AndroidBurstCoordinator::releaseScratch() noexcept {
    if (device_ && fence_ && submissionInFlight_) {
        (void)vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        submissionInFlight_ = false;
    }
    destroyHostBuffers();
    arena_.reset();
    arenaReady_ = false;
    layoutsInitialized_ = false;
    resourcesReused_ = false;
    pendingInitializeMs_ = 0.0;
}
void AndroidBurstCoordinator::reset() noexcept {
    std::lock_guard<std::mutex> lock(initMutex_);
    resetLocked();
}
void AndroidBurstCoordinator::resetLocked() noexcept {
    if (device_ && fence_ && submissionInFlight_) {
        (void)vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        submissionInFlight_ = false;
    }
    resetInfraLocked();
    arena_.reset();
    queueFamily_ = width_ = height_ = 0;
    outputScale_ = 0.0f;
    arenaReady_ = false;
    layoutsInitialized_ = false;
    submissionInFlight_ = false;
    resourcesReused_ = false;
    pipelineCacheReused_ = false;
    pendingInitializeMs_ = 0.0;
    submit_ = {};
    stageSink_ = {};
}
}  // namespace rawr::raw_gpu_pipeline
