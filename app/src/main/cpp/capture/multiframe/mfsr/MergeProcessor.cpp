#include "capture/multiframe/mfsr/MergeProcessor.h"

#include <stdexcept>

#include "capture/multiframe/MergeExposure.h"
#include "capture/multiframe/MultiframeQueueHelpers.h"
#include "diagnostics/logging/NativeLog.h"
namespace rawrcam::capture::multiframe {
rawr::raw_gpu_pipeline::BurstRunResult MergeProcessor::run(
    vulkan::VulkanContext& context, rawr::raw_gpu_pipeline::AndroidBurstCoordinator& coordinator, FrozenBurst& burst,
    uint32_t width, uint32_t height, uint32_t cfa, const MultiframeTuning& tuning, bool dumpRzslRequested,
    const rawr::raw_gpu_pipeline::AndroidBurstCoordinator::Submit& queueSubmit,
    const rawr::raw_gpu_pipeline::AndroidBurstCoordinator::Submit& mergeSubmit) {
    const bool useMfQueue = context.hasMultiframeQueue();
    // The noise model is fitted from the burst; the merges read only the CFA, the burst fit switch and the hot pixels.
    rawr::raw_merge_common::Config mergeConfig{};
    mergeConfig.cfa = static_cast<rawr::raw_merge_common::CfaPattern>(cfa);
    mergeConfig.estimateNoiseFromBurst = true;
    mergeConfig.hotPixels = burst.referenceMetadata.hotPixelMap;
    auto stageSink = rawrcam::capture::multiframe::makeTraceStageSink();
    // One Rexture window holds ~72 bytes of arena per pixel. The job splits larger frames into strips of at most
    // kRextureMaxPixels; a window above it is refused, which takes the existing failure path (base frame out, capture kept).
    if (tuning.mergeAlgorithm == kMergeAlgorithmRexture && std::uint64_t(width) * height > kRextureMaxPixels)
        throw std::runtime_error("Rexture: merge window above kRextureMaxPixels");
    // Captures journaled by older builds with a removed merge algorithm (super-resolution, HDR+, HDR+ Bracketed) are not
    // merged: the failure path outputs the base frame and keeps the capture.
    if (tuning.mergeAlgorithm != kMergeAlgorithmRexture && tuning.mergeAlgorithm != kMergeAlgorithmHdrPlusQuality)
        throw std::runtime_error("unsupported merge algorithm " + std::to_string(tuning.mergeAlgorithm));
    const auto algorithm = tuning.mergeAlgorithm == kMergeAlgorithmRexture
                               ? rawr::raw_gpu_pipeline::MergeAlgorithm::Rexture
                               : rawr::raw_gpu_pipeline::MergeAlgorithm::HdrPlusFrequency;
    // Per-frame exposure from the (journaled) metadata: both merges scale every companion to the reference exposure
    // (AE may still be settling across the ring).
    if (burst.metadata.size() == burst.frames.size())
        for (std::size_t i = 0; i < burst.frames.size(); ++i)
            burst.frames[i].exposure = float(linearExposure(burst.metadata[i]) * 1.0e-6);
    rawr::raw_merge_hdrplus_gpu::Config hdrplusConfig{};
    hdrplusConfig.strength = tuning.hdrplusStrength;
    hdrplusConfig.tileSize = tuning.hdrplusTileSize;
    coordinator.initialize(context.physicalDevice(), context.device(), context.queueFamily(), mergeSubmit, width,
                           height, stageSink, mergeConfig, algorithm, hdrplusConfig);
    ScopedSemaphore mfStartSem(context.device(), useMfQueue);
    if (useMfQueue) {
        // Empty signal ordered after all prior queue-0 work (decodes, base
        // readback); the first merge chunk waits on it. No host wait needed.
        VkSemaphore bridgeSem = mfStartSem.get();
        VkSubmitInfo bridge{};
        bridge.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        bridge.signalSemaphoreCount = 1;
        bridge.pSignalSemaphores = &bridgeSem;
        queueSubmit(bridge, VK_NULL_HANDLE);
        coordinator.setInitialWaitSemaphore(bridgeSem);
    }
    LOGI("MULTIFRAME_QUEUE mode=%s lowpri=%d", useMfQueue ? "dualq" : "singleq",
         context.multiframeQueueLowPriority() ? 1 : 0);
    auto result = coordinator.run(
        burst.frames, burst.referenceIndex,
        [&burst, dumpRzslRequested](std::uint32_t index) {
            if (!dumpRzslRequested && burst.snapshot && index < burst.frames.size() && index != burst.referenceIndex &&
                (!burst.pinsMayRelease || burst.pinsMayRelease())) {
                burst.snapshot->release(burst.frames[index].raw.ref.frameId);
            }
            // Journal-backed: the companion's image goes once merged.
            if (burst.releaseFrame && index != burst.referenceIndex) burst.releaseFrame(index);
        },
        burst.loadFrame ? rawr::raw_gpu_pipeline::AndroidBurstCoordinator::FrameLoad(
                              [&burst](std::uint32_t index) { burst.loadFrame(index); })
                        : rawr::raw_gpu_pipeline::AndroidBurstCoordinator::FrameLoad{});

    return result;
}
}  // namespace rawrcam::capture::multiframe
