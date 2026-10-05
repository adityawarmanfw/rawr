#include "capture/multiframe/mfsr/MergeProcessor.h"

#include <stdexcept>

#include "capture/multiframe/MultiframeQueueHelpers.h"
#include "diagnostics/logging/NativeLog.h"
namespace rawrcam::capture::multiframe {
rawr::raw_gpu_pipeline::BurstRunResult MergeProcessor::run(
    vulkan::VulkanContext& context, rawr::raw_gpu_pipeline::AndroidBurstCoordinator& coordinator, FrozenBurst& burst,
    uint32_t width, uint32_t height, uint32_t cfa, const MultiframeTuning& tuning, bool dumpRzslRequested,
    const rawr::raw_gpu_pipeline::AndroidBurstCoordinator::Submit& queueSubmit,
    const rawr::raw_gpu_pipeline::AndroidBurstCoordinator::Submit& mergeSubmit) {
    const bool useMfQueue = context.hasMultiframeQueue();
    rawr::raw_alignment_gpu::Config alignmentConfig{};
    alignmentConfig.lkIterations = tuning.lkIterations;
    alignmentConfig.hessianEpsilon = tuning.hessianEpsilon;
    rawr::raw_merge_wronski_gpu::Config mergeConfig{};
    mergeConfig.cfa = static_cast<rawr::raw_merge_wronski_gpu::CfaPattern>(cfa);
    mergeConfig.kDetail = tuning.kDetail;
    mergeConfig.kDenoise = tuning.kDenoise;
    mergeConfig.dThreshold = tuning.dThreshold;
    mergeConfig.dTransition = tuning.dTransition;
    mergeConfig.kStretch = tuning.kStretch;
    mergeConfig.kShrink = tuning.kShrink;
    mergeConfig.flatSigma = tuning.flatSigma;
    mergeConfig.detailFloorSigma = tuning.detailFloorSigma;
    mergeConfig.scaleBandwidthGain = tuning.scaleBandwidthGain;
    mergeConfig.coverageNeffLo = tuning.coverageNeffLo;
    mergeConfig.coverageNeffHi = tuning.coverageNeffHi;
    mergeConfig.coverageMassLo = tuning.coverageMassLo;
    mergeConfig.coverageMassHi = tuning.coverageMassHi;
    mergeConfig.robustnessT = tuning.robustnessT;
    mergeConfig.robustnessS1 = tuning.robustnessS1;
    mergeConfig.robustnessS2 = tuning.robustnessS2;
    mergeConfig.motionThreshold = tuning.motionThreshold;
    mergeConfig.estimateNoiseFromBurst = true;
    mergeConfig.hotPixels = burst.referenceMetadata.hotPixelMap;
    mergeConfig.fallbackChromaGain = tuning.fallbackChromaGain;
    mergeConfig.fallbackLumaGain = tuning.fallbackLumaGain;
    mergeConfig.fallbackChromaMaxSigma = 16.0f;
    auto stageSink = rawrcam::capture::multiframe::makeTraceStageSink();
    const auto algorithm = tuning.mergeAlgorithm == 2u   ? rawr::raw_gpu_pipeline::MergeAlgorithm::HdrPlusFrequency
                           : tuning.mergeAlgorithm == 1u ? rawr::raw_gpu_pipeline::MergeAlgorithm::HdrPlusSpatial
                                                         : rawr::raw_gpu_pipeline::MergeAlgorithm::Wronski;
    rawr::raw_merge_hdrplus_gpu::Config hdrplusConfig{};
    hdrplusConfig.strength = tuning.hdrplusStrength;
    hdrplusConfig.tileSize = tuning.hdrplusTileSize;
    coordinator.initialize(context.physicalDevice(), context.device(), context.queueFamily(), mergeSubmit, width,
                           height, tuning.outputScale, stageSink, alignmentConfig, mergeConfig, algorithm,
                           hdrplusConfig);
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
    auto result = coordinator.run(burst.frames, burst.referenceIndex, [&burst, dumpRzslRequested](std::uint32_t index) {
        if (!dumpRzslRequested && burst.snapshot && index < burst.frames.size() && index != burst.referenceIndex) {
            burst.snapshot->release(burst.frames[index].raw.ref.frameId);
        }
    });

    return result;
}
}  // namespace rawrcam::capture::multiframe
