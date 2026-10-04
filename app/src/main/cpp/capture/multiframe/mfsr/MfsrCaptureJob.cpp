#include "capture/multiframe/mfsr/MfsrCaptureJob.h"

#include <android/log.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "capture/multiframe/MultiframeDescription.h"
#include "capture/multiframe/MultiframeQueueHelpers.h"
#include "capture/multiframe/RzslCaptureWriter.h"
#include "capture/multiframe/mfsr/MergeProcessor.h"
#include "capture/persistence/CaptureJob.h"
#include "color/ColorMath.h"
#include "color/FilmExposure.h"
#include "color/WhiteBalance.h"
#include "develop/DevelopContextBuilder.h"
#include "develop/demosaic/DualStillProcessor.h"
#include "develop/highlight/LensShadingMapSnapshot.h"
#include "develop/render/StillImageRenderer.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "encoding/dng/DngCaptureWriter.h"
#include "encoding/jpeg/JpegCaptureWriter.h"
#include "encoding/jpeg/JpegTimingsFormat.h"
#include "encoding/rzsl/RzslBundleSink.h"
#include "geometry/CfaPattern.h"
#include "geometry/OrientationTransform.h"
#include "geometry/RawGeometry.h"
#include "imaging/RawSnapshot.h"
#include "rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h"
#include "rawr/raw_gpu_pipeline/ReplayMetadata.h"
#include "rawr/raw_multiframe_output/MultiframeOutputAdapter.h"
#include "rawr/zsl_codec/ZslCodec.h"
#include "rawr/zsl_ring/RawImageRing.h"
#include "rawr/zsl_ring/ZslRing.h"

namespace rawrcam::capture::multiframe {
namespace {
constexpr const char* kTag = "RawrCamNative";
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)

void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
    }
}

}  // namespace

void MfsrCaptureJob::run() {
    auto& ctx = context_;
    auto& work = work_;
    const std::uint64_t requestId = work->requestId;
    auto job = std::move(work->capture);
    auto baseDng = std::move(work->baseDng);
    auto mergedDng = std::move(work->mergedDng);
    auto mergedJpeg = std::move(work->mergedJpeg);
    const bool jpegRequested = work->jpegRequested;
    const bool dumpRzslRequested = work->dumpRzslRequested;
    const auto frozenTonemap = work->frozenTonemap;
    const auto tuning = [&] {
        // Super-resolution output must stay within the GPU's 2D image limit
        // (large sensors at the 1.386x maximum would exceed it).
        auto t = work->tuning;
        const float maxDimension = static_cast<float>(geometry::RawGeometryLimits{}.maxDimension);
        const float largestSide = static_cast<float>(std::max(ctx.rawWidth, ctx.rawHeight));
        if (largestSide > 0.0f)
            t.outputScale = std::clamp(t.outputScale, 1.0f, std::max(1.0f, maxDimension / largestSide));
        return t;
    }();
    const auto baseFrameMode = work->baseFrameMode;
    const auto sharpnessScores = work->sharpnessScores;
    const double sharpnessMs = work->sharpnessMs;
    double baseReadMs = 0.0, projectMs = 0.0, rgbPrepMs = 0.0;
    auto wallMs = [](const std::chrono::steady_clock::time_point& t0) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    // Shared EXIF inputs (capture settings; timings attach per file horizon).
    MultiframeTuningView tuningView{};
    tuningView.outputScale = tuning.outputScale;
    tuningView.lkIterations = tuning.lkIterations;
    tuningView.hessianEpsilon = tuning.hessianEpsilon;
    tuningView.kDetail = tuning.kDetail;
    tuningView.kDenoise = tuning.kDenoise;
    tuningView.dThreshold = tuning.dThreshold;
    tuningView.dTransition = tuning.dTransition;
    tuningView.kStretch = tuning.kStretch;
    tuningView.kShrink = tuning.kShrink;
    tuningView.flatSigma = tuning.flatSigma;
    tuningView.detailFloorSigma = tuning.detailFloorSigma;
    tuningView.scaleBandwidthGain = tuning.scaleBandwidthGain;
    tuningView.coverageNeffLo = tuning.coverageNeffLo;
    tuningView.coverageNeffHi = tuning.coverageNeffHi;
    tuningView.coverageMassLo = tuning.coverageMassLo;
    tuningView.coverageMassHi = tuning.coverageMassHi;
    tuningView.robustnessT = tuning.robustnessT;
    tuningView.robustnessS1 = tuning.robustnessS1;
    tuningView.robustnessS2 = tuning.robustnessS2;
    tuningView.motionThreshold = tuning.motionThreshold;
    tuningView.fallbackChromaGain = tuning.fallbackChromaGain;
    tuningView.fallbackLumaGain = tuning.fallbackLumaGain;
    tuningView.baseFrameMode = baseFrameMode;
    MultiframeOutputInfo outputInfo{};
    outputInfo.noiseProfile = "Burst (fitted per capture)";
    outputInfo.demosaicLabel = "n/a";
    outputInfo.dualLabel = "n/a";
    outputInfo.fccSteps = mergedJpeg.develop.fccSteps;
    outputInfo.lscEnabled = mergedJpeg.develop.lensShadingCorrectionEnabled;
    outputInfo.highlightReconstructionEnabled = mergedJpeg.develop.highlightReconstructionEnabled;
    outputInfo.jpegQuality = mergedJpeg.output.quality;
    outputInfo.subsampling =
        mergedJpeg.output.subsampling == rawrcam::encoding::jpeg::ChromaSubsampling::Yuv444
            ? "4:4:4"
            : (mergedJpeg.output.subsampling == rawrcam::encoding::jpeg::ChromaSubsampling::Yuv422 ? "4:2:2" : "4:2:0");
    outputInfo.colorProfile = mergedJpeg.output.rendererDisplayName;
    TonemapUiValues toneUi{};
    toneUi.exposureEV = frozenTonemap.exposureEV;
    toneUi.blacks = frozenTonemap.blackPointEV;
    toneUi.shadows = frozenTonemap.shadowLiftEV;
    toneUi.midtones = frozenTonemap.midtoneLiftEV;
    toneUi.contrast = frozenTonemap.contrast;
    toneUi.whites = frozenTonemap.whitePointEV;
    toneUi.highlights = frozenTonemap.highlightBiasEV;
    toneUi.saturation = frozenTonemap.saturation;
    toneUi.vibrance = frozenTonemap.vibrance;
    toneUi.aePostGain = frozenTonemap.aePostGain;
    MultiframeStageTimings mfStages{};
    mfStages.sharpnessMs = sharpnessMs;
    using rawr::raw_multiframe_output::MultiframeOutputAdapter;
    using rawrcam::develop::rendered::RenderedStillCompletion;
    using rawrcam::develop::rendered::RenderedStillContext;
    using rawrcam::develop::rendered::StillImageRenderer;
    using rawrcam::encoding::dng::DngCaptureWriter;
    using rawrcam::encoding::dng::DngWriteCompletion;
    using rawrcam::encoding::jpeg::JpegCaptureWriter;
    using rawrcam::encoding::jpeg::JpegWriteCompletion;
    using rawrcam::imaging::RawSnapshot;

    const std::string baseDisplayName = baseDng.displayName;
    const std::string mergedDisplayName = mergedDng.displayName;
    const std::string jpegDisplayName = mergedJpeg.output.displayName;
    auto publishDng = [&](const DngWriteCompletion& done) {
        std::ostringstream out;
        out << done.requestId << '\t' << (done.success ? 1 : 0) << '\t' << done.displayName << '\t' << done.error;
        ctx.dngBox.push(out.str());
    };
    const bool wantBase = baseDng.outputFd >= 0, wantMerged = mergedDng.outputFd >= 0;
    auto failDng = [&](const std::string& displayName, const std::string& error) {
        if ((displayName == baseDisplayName && !wantBase) || (displayName == mergedDisplayName && !wantMerged)) return;
        DngWriteCompletion done{};
        done.requestId = requestId;
        done.displayName = displayName;
        done.error = error;
        publishDng(done);
    };
    auto publishJpeg = [&](const JpegWriteCompletion& done) {
        std::ostringstream out;
        out << done.requestId << '\t' << (done.success ? 1 : 0) << '\t' << done.displayName << '\t' << done.error
            << '\t' << (done.filmFallbackMemory ? 1 : 0);
        ctx.jpegBox.push(out.str());
    };
    auto failJpeg = [&](const std::string& error) {
        JpegWriteCompletion done{};
        done.requestId = requestId;
        done.displayName = jpegDisplayName;
        done.error = error;
        publishJpeg(done);
    };
    auto makeCaptured = [&](rawr::raw_multiframe_output::PackedRaw16 packed) {
        auto captured = std::make_shared<rawrcam::imaging::RawSnapshot>();
        captured->requestId = requestId;
        captured->timestampNs = job->referenceMetadata.timestampNs;
        captured->width = packed.width;
        captured->height = packed.height;
        captured->packedRowStrideBytes = packed.rowStrideBytes;
        captured->sourceRowStrideBytes = static_cast<int32_t>(packed.rowStrideBytes);
        captured->sourcePixelStrideBytes = static_cast<int32_t>(sizeof(std::uint16_t));
        captured->metadata = job->referenceMetadata;
        captured->colorState = job->referenceColor;
        captured->raw16 = std::move(packed.pixels);
        return captured;
    };
    auto queueSubmit = [&](const VkSubmitInfo& info, VkFence fence) {
        vkCheck(ctx.vulkan.primaryQueue().submit(info, fence), "multiframe output queue submit");
    };
    // Merge compute targets the second same-family queue when present so
    // preview submits no longer wait behind whole merge chunks in one FIFO.
    // Same family => no queue-ownership transfers for any image.
    const bool useMfQueue = ctx.vulkan.hasMultiframeQueue();
    const VkQueue mfQueue = ctx.vulkan.multiframeQueue();
    auto mfQueueSubmit = [&](const VkSubmitInfo& info, VkFence fence) {
        vkCheck(ctx.vulkan.multiframeSubmission().submit(info, fence), "multiframe queue submit");
    };
    rawr::raw_gpu_pipeline::AndroidBurstCoordinator::Submit mergeSubmit = queueSubmit;
    if (useMfQueue) mergeSubmit = mfQueueSubmit;

    DngCaptureWriter baseWriter;
    DngCaptureWriter mergedWriter;
    bool baseStarted = false;
    bool mergedStarted = false;
    const bool baseRequested = baseDng.outputFd >= 0;
    const bool mergedRequested = mergedDng.outputFd >= 0;
    bool basePublished = !baseRequested;
    bool mergedPublished = !mergedRequested;
    bool jpegPublished = !jpegRequested;
    bool rzslDumpAttempted = false;
    // Noise is fitted from the burst at merge time; offline replay re-fits it
    // (--noise-source burst), so the replay record carries the legacy model.
    rawr::raw_merge_wronski_gpu::Config resolvedNoise{};
    std::ostringstream replayMetadata;
    replayMetadata << rawr::raw_gpu_pipeline::serializeReplayNoise(resolvedNoise) << "rawrReferenceIndex\t"
                   << job->referenceIndex << '\n';
    baseDng.mergeReplayMetadata = replayMetadata.str();
    mergedDng.mergeReplayMetadata = replayMetadata.str();
    auto dumpRzsl = [&]() {
        if (!dumpRzslRequested || rzslDumpAttempted) return;
        rzslDumpAttempted = true;
        const std::filesystem::path failedPath =
            std::filesystem::path(ctx.filesDir) / "rawrcam_zsl_bundle_latest.failed";
        std::error_code removeError;
        std::filesystem::remove(failedPath, removeError);
        try {
            writePostShutterRzsl(ctx.vulkan, ctx.queue0Mutex, ctx.filesDir, ctx.cfa, job->frames, job->metadata,
                                 sharpnessScores, replayMetadata.str());
            rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
                rawrcam::diagnostics::RuntimeTraceStage::ZslDumpComplete, 0, 0, -1, 0, 0,
                static_cast<uint32_t>(job->frames.size()), 0);
        } catch (const std::exception& e) {
            std::ofstream failed(failedPath, std::ios::trunc);
            failed << e.what() << '\n';
            rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
                rawrcam::diagnostics::RuntimeTraceStage::ZslDumpFail, 0, 0, -1, 0, 0, 1u, 0);
            LOGE("POST_SHUTTER_RZSL_FAIL %s", e.what());
        }
    };
    MultiframeOutputAdapter outputAdapter;
    bool outputAdapterReady = false;
    if (baseRequested) try {
            outputAdapter.initialize(ctx.vulkan.physicalDevice(), ctx.vulkan.device(), ctx.vulkan.queueFamily(),
                                     queueSubmit, ctx.rawWidth, ctx.rawHeight);
            outputAdapterReady = true;
            const auto baseReadBegin = std::chrono::steady_clock::now();
            auto basePacked = outputAdapter.readBase(job->frames[job->referenceIndex].raw.image);
            baseReadMs = wallMs(baseReadBegin);
            outputAdapter.reset();
            outputAdapterReady = false;
            auto baseFrame = makeCaptured(std::move(basePacked));
            baseDng.imageDescription = "Captured with Rawr (multiframe base frame)";
            auto context = std::move(baseDng);
            baseDng.outputFd = -1;
            baseStarted = baseWriter.start(baseFrame, std::move(context));
            if (!baseStarted) {
                failDng(baseDisplayName, "base_dng_start_rejected");
                basePublished = true;
            }
        } catch (const std::exception& e) {
            if (baseDng.outputFd >= 0) {
                close(baseDng.outputFd);
                baseDng.outputFd = -1;
            }
            failDng(baseDisplayName, std::string("base_output_failed: ") + e.what());
            basePublished = true;
        }

    std::optional<rawr::raw_gpu_pipeline::BurstRunResult> mergeResult;
    std::shared_ptr<rawrcam::imaging::RawSnapshot> mergedFrame;
    try {
        auto& coordinator = ctx.burstCoordinator;
        mergeResult = MergeProcessor::run(ctx.vulkan, coordinator, *job, ctx.rawWidth, ctx.rawHeight, ctx.cfa, tuning,
                                          dumpRzslRequested, queueSubmit, mergeSubmit);
        // Merge timings feed both horizons (merged DNG and JPEG). Populate
        // here — not inside the merged-DNG gate below — so JPEG-only captures
        // (no merged DNG requested) still report the merge/alignment stages
        // instead of an all-zero block.
        {
            mfStages.baseReadMs = baseReadMs;
            mfStages.initMs = mergeResult->timings.initializeMs;
            mfStages.noiseMs = mergeResult->timings.noiseEstimateMs;
            mfStages.refPrepareMs = mergeResult->timings.referencePrepareMs;
            mfStages.refStatsMs = mergeResult->timings.referenceStatsMs;
            mfStages.companionPrepareMs = mergeResult->timings.companionPrepareMs;
            mfStages.alignMs = mergeResult->timings.alignmentMs;
            mfStages.kernelMs = mergeResult->timings.kernelStatsMs;
            mfStages.robustnessMs = mergeResult->timings.robustnessMs;
            mfStages.accumMs = mergeResult->timings.accumulationMs;
            mfStages.finalizeMs = mergeResult->timings.finalizeMs;
            const auto& t = mergeResult->timings;
            const auto& n = mergeResult->estimatedNoise;
            LOGI(
                "MULTIFRAME_MERGE_TIMING frames=%u noiseMs=%.1f refMs=%.1f prepMs=%.1f alignMs=%.1f "
                "kernelMs=%.1f robustMs=%.1f accumMs=%.1f finalizeMs=%.1f totalMs=%.1f noiseBurst=%d "
                "slope=%g offset=%g",
                mergeResult->frameCount, t.noiseEstimateMs, t.referencePrepareMs + t.referenceStatsMs,
                t.companionPrepareMs, t.alignmentMs, t.kernelStatsMs, t.robustnessMs, t.accumulationMs, t.finalizeMs,
                t.totalMs, mergeResult->noiseEstimated ? 1 : 0,
                .25f * (n.slopeBySite[0] + n.slopeBySite[1] + n.slopeBySite[2] + n.slopeBySite[3]),
                .25f * (n.offsetBySite[0] + n.offsetBySite[1] + n.offsetBySite[2] + n.offsetBySite[3]));
            mfStages.initNote = mergeResult->resourcesReused
                                    ? "pipelines warm"
                                    : (mergeResult->pipelineCacheReused ? "pipeline cache reused" : "cold");
        }
        // 1A: the reference RAW stays pinned until prepareRgb consumes it for
        // the sensor-clip OR below. Non-reference frames were released by the
        // run callback; the reference release happens after prepareRgb (or
        // here on early exit paths that skip prepareRgb).
        bool referenceReleased = false;
        auto releaseReference = [&]() {
            if (!dumpRzslRequested && !referenceReleased && job->snapshot) {
                job->snapshot->release(job->frames[job->referenceIndex].raw.ref.frameId);
                referenceReleased = true;
            }
        };
        auto& workerTrace = rawrcam::diagnostics::RuntimeTraceRecorder::instance();
        workerTrace.record(rawrcam::diagnostics::RuntimeTraceStage::MultiframeComplete,
                           job->referenceMetadata.timestampNs, job->refs[job->referenceIndex].frameId, -1, 0, 0,
                           mergeResult->frameCount,
                           static_cast<int64_t>((mergeResult->physicalImageBytes * 1000ull) / (1024ull * 1024ull)));

        try {
            // Projection reads merge-arena output: keep it on the merge queue
            // so no cross-queue handoff is needed (host fence covers readback).
            outputAdapter.initialize(ctx.vulkan.physicalDevice(), ctx.vulkan.device(), ctx.vulkan.queueFamily(),
                                     mergeSubmit, mergeResult->outputExtent.width, mergeResult->outputExtent.height);
            outputAdapterReady = true;
        } catch (const std::exception& e) {
            LOGE("MULTIFRAME_OUTPUT_INIT_FAIL %s", e.what());
        }

        if (mergedRequested && outputAdapterReady) {
            try {
                rawr::raw_multiframe_output::CfaProjectionParameters projection{};
                // project_cfa_u16 indexes black by physical position (colour order in metadata).
                projection.blackByPhase =
                    rawrcam::geometry::reorderRggbByCode(job->referenceMetadata.blackLevelPhysicalRggb, ctx.cfa);
                projection.whiteLevel = job->referenceMetadata.effectiveWhiteLevel;
                projection.cfa = ctx.cfa;
                const auto projectBegin = std::chrono::steady_clock::now();
                auto mergedPacked =
                    outputAdapter.projectMerged(mergeResult->output, mergeResult->outputView, projection);
                projectMs = wallMs(projectBegin);
                mergedFrame = makeCaptured(std::move(mergedPacked));
                mergedDng.reconstructedGeometry = tuning.outputScale > 1.0001f;
                {
                    // Core merge/alignment stages were populated right after
                    // coordinator.run() (shared by both horizons); only the
                    // DNG-horizon projection time attaches here.
                    mfStages.baseReadMs = baseReadMs;
                    mfStages.projectMs = projectMs;
                    const std::string filmBlock = (work->filmEnabled && !mergedJpeg.output.filmDescription.empty())
                                                      ? mergedJpeg.output.filmDescription
                                                      : "";
                    mergedDng.imageDescription =
                        buildExifHeader() +
                        buildParametersBlock(tuningView, outputInfo, toneUi, filmBlock, work->filmEnabled,
                                             mergeResult->frameCount, mergeResult->outputExtent.width,
                                             mergeResult->outputExtent.height) +
                        buildSharpnessSection(baseFrameMode, sharpnessScores, job->referenceIndex,
                                              static_cast<std::uint32_t>(job->frames.size())) +
                        "\nTIMINGS (through CFA projection): " +
                        rawrcam::encoding::jpeg::formatMs1(
                            dngSubtotalMs(baseReadMs, mfStages.multiframeTotalMs(), projectMs)) +
                        " ms\n" + formatMultiframeTimings(mfStages, baseFrameMode) +
                        formatOutputProjection(baseReadMs, projectMs) +
                        "(Render stages, encode + overall total: see JPEG EXIF)\n";
                }
                auto context = std::move(mergedDng);
                mergedDng.outputFd = -1;
                mergedStarted = mergedWriter.start(mergedFrame, std::move(context));
                if (!mergedStarted) {
                    failDng(mergedDisplayName, "merged_dng_start_rejected");
                    mergedPublished = true;
                }
            } catch (const std::exception& e) {
                if (mergedDng.outputFd >= 0) {
                    close(mergedDng.outputFd);
                    mergedDng.outputFd = -1;
                }
                failDng(mergedDisplayName, std::string("merged_projection_failed: ") + e.what());
                mergedPublished = true;
            }
        } else if (mergedRequested) {
            if (mergedDng.outputFd >= 0) {
                close(mergedDng.outputFd);
                mergedDng.outputFd = -1;
            }
            failDng(mergedDisplayName, "merged_projection_unavailable");
            mergedPublished = true;
        }

        // Keep only the packed merged CFA + clip mask for multiframe JPEG, so merge
        // scratch can be released before the demosaic/render allocations. The
        // JPEG path demosaics the merged mosaic with the still Dual demosaicer:
        // the merge's own per-channel reconstruction is softer on fine texture.
        // DNG stays CFA (host copy in mergedFrame already done by
        // projectMerged/copyMapped). A prepare failure must only fail JPEG: the
        // DNG writer already holds a valid host copy, so never let it reach the
        // outer catch (which would mark the DNG as merge_failed).
        bool rgbPrepareFailed = false;
        bool sensorClipOrApplied = false;
        if (jpegRequested && outputAdapterReady) {
            try {
                const auto lsc = rawrcam::develop::highlight::LensShadingMapSnapshot::fromMetadata(
                    job->referenceMetadata, mergedJpeg.develop.lensShadingCorrectionEnabled);
                outputAdapter.releaseDngResources();
                // 1A sensor OR: reference RAW (R16U, pre-LSC/WB) supplies
                // per-phase truth that merge dilution can hide. Applied only
                // when reference geometry matches the merged grid (scale 1x);
                // otherwise the adapter keeps derived-only behavior.
                rawr::raw_multiframe_output::SensorClipParams sensorClip{};
                if (job->referenceIndex < job->frames.size() &&
                    job->frames[job->referenceIndex].raw.view != VK_NULL_HANDLE) {
                    sensorClip.refRawView = job->frames[job->referenceIndex].raw.view;
                    sensorClip.refWidth = ctx.rawWidth;
                    sensorClip.refHeight = ctx.rawHeight;
                    sensorClip.cfa = ctx.cfa;
                    sensorClip.blackByPhase = job->referenceMetadata.blackLevelPhysicalRggb;
                    sensorClip.whiteLevel = job->referenceMetadata.effectiveWhiteLevel;
                    sensorClip.clipThreshold = 0.995f;
                }
                const auto rgbPrepBegin = std::chrono::steady_clock::now();
                outputAdapter.preparePackedCfa(mergeResult->outputView, lsc.width, lsc.height, lsc.gains, sensorClip);
                rgbPrepMs = wallMs(rgbPrepBegin);
                sensorClipOrApplied = outputAdapter.sensorClipOrApplied();
                LOGI("MULTIFRAME_SENSOR_CLIP applied=%d ref=%ux%u out=%ux%u", sensorClipOrApplied ? 1 : 0,
                     sensorClip.refWidth, sensorClip.refHeight, mergeResult->outputExtent.width,
                     mergeResult->outputExtent.height);
            } catch (const std::exception& e) {
                LOGE("MULTIFRAME_RGB_PREPARE_FAIL %s (DNG unaffected)", e.what());
                rgbPrepareFailed = true;
            }
            // Reference pin held for the sensor OR above; release now that
            // prepareRgb has fence-waited the merged+raw reads.
            releaseReference();
        } else {
            outputAdapter.reset();
            outputAdapterReady = false;
            releaseReference();
        }
        {
            // ORDERING: prepareRgb above synchronously copies linear_output
            // (fence-waited inside prepareRgb) before releaseScratch below
            // destroys the merge arena. Do not swap: the merge output view
            // must be consumed before the arena is released (UAF otherwise).
            const long pages = sysconf(_SC_PHYS_PAGES);
            const long pageSize = sysconf(_SC_PAGESIZE);
            const unsigned long long totalRam =
                (pages > 0 && pageSize > 0)
                    ? static_cast<unsigned long long>(pages) * static_cast<unsigned long long>(pageSize)
                    : 0ull;
            struct sysinfo memInfo{};
            const unsigned long long availRam =
                sysinfo(&memInfo) == 0 ? static_cast<unsigned long long>(memInfo.freeram) * memInfo.mem_unit : 0ull;
            const bool retainArena =
                totalRam >= 12ull * 1024ull * 1024ull * 1024ull && availRam >= 8ull * 1024ull * 1024ull * 1024ull;
            LOGI("MULTIFRAME_ARENA retain=%d totalRamGB=%.1f availRamGB=%.1f", retainArena ? 1 : 0,
                 double(totalRam) / (1024.0 * 1024.0 * 1024.0), double(availRam) / (1024.0 * 1024.0 * 1024.0));
            if (!retainArena) coordinator.releaseScratch();
        }

        if (jpegRequested) {
            // Declared outside try so the catch below can release mapped
            // pixels after a worker exception (persistent-engine poison).
            develop::rendered::StillImageRenderer localRendered(ctx.filesDir);
            // Per capture, like the single-frame still demosaicers.
            develop::demosaic::dual::DualStillProcessor mergedDemosaic;
            develop::rendered::StillImageRenderer& rendered =
                ctx.persistentEngine ? ctx.renderedStill : localRendered;
            try {
                if (!job->referenceMetadata.cameraContext) {
                    throw std::runtime_error("missing_camera_context");
                }
                const auto& camera = *job->referenceMetadata.cameraContext;
                const uint32_t turns = rawrcam::geometry::presentationQuarterTurns(
                    camera.geometry.sensorOrientationDegrees, mergedJpeg.output.deviceRotationDegrees);
                mergedJpeg.output.exifOrientation = turns == 1u ? 6u : (turns == 2u ? 3u : (turns == 3u ? 8u : 1u));
                mergedJpeg.output.exposureTimeNs = job->referenceMetadata.exposureTimeNs;
                mergedJpeg.output.sensitivity = job->referenceMetadata.sensitivity;
                mergedJpeg.output.aperture = job->referenceMetadata.aperture;
                mergedJpeg.output.focalLengthMm = job->referenceMetadata.focalLengthMm;
                if (rgbPrepareFailed) {
                    throw std::runtime_error("merged_rgb_prepare_failed");
                }
                if (outputAdapter.rgbImage() == VK_NULL_HANDLE || outputAdapter.clipView() == VK_NULL_HANDLE) {
                    throw std::runtime_error("merged_cfa_unavailable");
                }
                const std::uint32_t mergedW = mergeResult->outputExtent.width;
                const std::uint32_t mergedH = mergeResult->outputExtent.height;
                const auto demosaicBegin = std::chrono::steady_clock::now();
                mergedDemosaic.configure(
                    ctx.vulkan, ctx.queue0Mutex, camera.cameraContextGeneration, camera.cameraId, mergedW, mergedH,
                    ctx.cfa, mergedJpeg.develop.dualAutoContrast, mergedJpeg.develop.dualContrastPercent,
                    mergedJpeg.develop.pipelineDiagnosticsEnabled,
                    tuning.outputScale > 1.0001f ? develop::StillDemosaicGeometry::ReconstructedCfa
                                                 : develop::StillDemosaicGeometry::SensorNative,
                    useMfQueue ? mfQueue : VK_NULL_HANDLE, useMfQueue ? &ctx.vulkan.mfSubmitMutex() : nullptr,
                    /*externalPackedInput=*/true);
                if (!mergedDemosaic.startExternalPacked(outputAdapter.rgbImage(), outputAdapter.rgbView(), requestId,
                                                        job->referenceMetadata.timestampNs)) {
                    throw std::runtime_error("merged_demosaic_start_rejected");
                }
                std::optional<develop::demosaic::StillCompletion> demosaicDone;
                while (!(demosaicDone = mergedDemosaic.pollCompletion())) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (!demosaicDone->success) throw std::runtime_error("merged_demosaic_failed: " + demosaicDone->error);
                mergedDemosaic.releaseWorkspaceKeepOutput();
                const double demosaicMs = wallMs(demosaicBegin);
                VkImage demosaicedImage = mergedDemosaic.outputImage();
                VkImageView demosaicedView = mergedDemosaic.outputView();
                VkImage clipStateImage = outputAdapter.clipImage();
                VkImageView clipStateView = outputAdapter.clipView();
                if (demosaicedImage == VK_NULL_HANDLE || demosaicedView == VK_NULL_HANDLE ||
                    clipStateView == VK_NULL_HANDLE) {
                    throw std::runtime_error("merged_demosaic_output_unavailable");
                }
                outputInfo.demosaicLabel = "RCD+VNG4 (merged CFA)";
                outputInfo.dualLabel =
                    mergedJpeg.develop.dualAutoContrast
                        ? std::string("auto")
                        : rawrcam::encoding::jpeg::formatMs1(mergedJpeg.develop.dualContrastPercent) + "%";
                LOGI("MULTIFRAME_JPEG_INPUT mode=merged_cfa_dual requestId=%llu raw=%ux%u demosaicMs=%.1f",
                     static_cast<unsigned long long>(requestId), mergedW, mergedH, demosaicMs);

                const rawrcam::color::math::Vec3 whiteBalanceRgb =
                    rawrcam::color::collapseRggbToRgb(job->referenceColor.baselineWbRggb);
                RenderedStillContext renderedContext{};
                renderedContext.requestId = requestId;
                renderedContext.timestampNs = job->referenceMetadata.timestampNs;
                renderedContext.width = mergeResult->outputExtent.width;
                renderedContext.height = mergeResult->outputExtent.height;
                renderedContext.presentationQuarterTurns = turns;
                renderedContext.whiteBalanceRgb = whiteBalanceRgb;
                renderedContext.cameraToWorkingColumnMajor =
                    rawrcam::color::math::cameraToWorkingColumnMajor(job->referenceColor.cameraToLinearSrgbRowMajor);
                renderedContext.tonemapParams = frozenTonemap;
                // Film state rides the merge job (same shutter-time snapshot
                // semantics as single-frame): full look + sensor matrix.
                renderedContext.filmEnabled = work->filmEnabled;
                renderedContext.filmTiled = uint64_t(renderedContext.width) * renderedContext.height >= 12000000u &&
                                            (!work->filmLook.grainEnabled || work->filmLook.grainModel != 2);
                renderedContext.filmLook = work->filmLook;
                renderedContext.sensorToLinearSrgb = job->referenceColor.cameraToLinearSrgbRowMajor;
                develop::applyDevelopSettings(renderedContext, mergedJpeg.develop);
                // The single-frame denoisers (Image > Denoise: wavelet, GALOSH
                // RAW/YUV) never run here; Kotlin resolves them off and native
                // enforces it. The multiframe Chroma Denoise toggle instead runs
                // the profiled wavelet chroma-only (forceY 0: luma untouched) on
                // the merged image, with the burst-fitted green noise divided by
                // the merged frame count (the merged noise level). A larger
                // assumed noise makes the wavelet's per-band thresholds erase
                // texture, so the single-frame profile is never used.
                renderedContext.denoiseStrength = 0.0f;
                renderedContext.galoshYuvMode = 0;
                outputInfo.chromaDenoiseLabel = "off";
                if (mergedJpeg.develop.multiframeChromaDenoise) {
                    const auto& n = mergeResult->estimatedNoise;
                    const float frames = float(std::max<std::uint32_t>(mergeResult->frameCount, 1u));
                    const float a = .5f * (n.slopeBySite[1] + n.slopeBySite[2]) / frames;
                    const float b = .5f * (n.offsetBySite[1] + n.offsetBySite[2]) / frames;
                    if (mergeResult->noiseEstimated && std::isfinite(a) && a > 0.0f && std::isfinite(b) && b >= 0.0f) {
                        renderedContext.denoiseStrength = 1.0f;
                        renderedContext.denoiseDetail = 1.0f;
                        renderedContext.denoiseForceY = 0.0f;
                        renderedContext.denoiseMaxScale = 7;
                        renderedContext.denoiseNoiseA = a;
                        renderedContext.denoiseNoiseB = b;
                        outputInfo.chromaDenoiseLabel =
                            "wavelet chroma (burst noise / " + std::to_string(mergeResult->frameCount) + ")";
                    } else {
                        outputInfo.chromaDenoiseLabel = "skipped (no burst noise fit)";
                    }
                }
                renderedContext.multiframeSensorClipOrApplied = sensorClipOrApplied;
                renderedContext.clipFromMultiframe = true;
                renderedContext.diagnosticsEnabled = mergedJpeg.develop.pipelineDiagnosticsEnabled;
                renderedContext.cfaPattern = camera.rawPreviewCfa;
                renderedContext.lensShading = develop::highlight::LensShadingMapSnapshot::fromMetadata(
                    job->referenceMetadata, mergedJpeg.develop.lensShadingCorrectionEnabled);
                renderedContext.ultraHdrEnabled = mergedJpeg.output.ultraHdr.enabled;
                renderedContext.gainmapParams = mergedJpeg.output.ultraHdr.toGainmapParams();
                // HDR gainmap input is camera RGB: same calibrated matrix the
                // tonemap/film path uses (never the AP1->sRGB default).
                renderedContext.gainmapCstRowMajor = job->referenceColor.cameraToLinearSrgbRowMajor;
                renderedContext.hasGainmapCst = true;
                // Scene-exposure match (see SingleFrameCoordinator): the SDR
                // base contains the capture exposure gain, the HDR tap does
                // not. frozenTonemap carries aePostGain + exposureEV for the
                // tonemap path; the film path mirrors its folded EV.
                if (renderedContext.filmEnabled) {
                    renderedContext.gainmapParams.hdrExposure =
                        std::exp2(rawrcam::color::filmExposureEv(work->filmLook, frozenTonemap.aePostGain));
                } else {
                    renderedContext.gainmapParams.hdrExposure =
                        std::max(frozenTonemap.aePostGain, 1.0e-6f) * std::exp2(frozenTonemap.exposureEV);
                }
                // Persistent-engine toggle (Settings > Experimental > Persistent
                // Engine): member reuse across captures vs. a function-local
                // with today's per-capture lifecycle, bit-for-bit. On the
                // persistent path release pixels only so cached engines
                // survive; reset() would recompile the gainmap compute
                // pipeline every shot. reset() still self-heals leaked
                // busy/completion state on the per-capture path.
                LOGI("MULTIFRAME_RENDER_ENGINE mode=%s", ctx.persistentEngine ? "persistent" : "per-capture");
                if (ctx.persistentEngine) {
                    rendered.releasePixels();
                } else {
                    rendered.reset();
                }
                rendered.setAssetManager(ctx.assetManager);
                // Still chain on the multiframe queue when present: the film
                // render alone holds the GPU ~500ms, which would stall every
                // preview submit behind it in queue0's FIFO. Same family, so
                // no ownership transfers; CPU fence-waits already order the
                // demosaic -> render handoff across queues.
                if (!rendered.start(ctx.vulkan, ctx.queue0Mutex, renderedContext, demosaicedImage, demosaicedView,
                                    clipStateImage, clipStateView, useMfQueue ? mfQueue : VK_NULL_HANDLE,
                                    useMfQueue ? &ctx.vulkan.mfSubmitMutex() : nullptr)) {
                    throw std::runtime_error("merged_render_start_rejected");
                }
                std::optional<RenderedStillCompletion> renderedDone;
                while (!renderedDone) {
                    renderedDone = rendered.pollCompletion();
                    if (!renderedDone) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
                if (renderedDone->filmFallbackMemory) {
                    persistence::markFilmFallback(persistence::jobPath(ctx.filesDir, baseDisplayName));
                    if (mergedJpeg.output.outputFd >= 0) close(mergedJpeg.output.outputFd);
                    mergedJpeg.output.outputFd = -1;
                    JpegWriteCompletion fallback{};
                    fallback.requestId = requestId;
                    fallback.displayName = jpegDisplayName;
                    fallback.error = "film_memory_saved_as_dng";
                    fallback.filmFallbackMemory = true;
                    publishJpeg(fallback);
                    jpegPublished = true;
                    rendered.releasePixels();
                } else {
                    if (!renderedDone->success) {
                        throw std::runtime_error(renderedDone->error);
                    }
                    mergedJpeg.output.colorProcessingMs = renderedDone->colorProcessingMs;
                    mergedJpeg.output.highlightReconstructionMs = renderedDone->highlightReconstructionMs;
                    mergedJpeg.output.refinementMs = renderedDone->refinementMs;
                    mergedJpeg.output.tonemapMs = renderedDone->tonemapMs;
                    mergedJpeg.output.denoiseMs = renderedDone->denoiseMs;
                    mergedJpeg.output.galoshYuvMs = renderedDone->galoshYuvMs;
                    mergedJpeg.output.gainmapMs = renderedDone->gainmapMs;
                    mergedJpeg.output.postTailMs = renderedDone->postTailMs;
                    mergedJpeg.output.highlightStage = renderedDone->highlightStage;
                    mergedJpeg.output.highlightForcedByUltraHdr = renderedDone->highlightForcedByUltraHdr;
                    mergedJpeg.output.defringeEnabled = renderedDone->defringeEnabled;
                    mergedJpeg.output.renderSetupMs = renderedDone->renderSetupMs;
                    mergedJpeg.output.renderEngineBuildMs = renderedDone->engineBuildMs;
                    mergedJpeg.output.demosaicSetupMs = demosaicDone->setupMs;
                    mergedJpeg.output.queueGapsMs = renderedDone->queueGapsMs;
                    mergedJpeg.output.readbackMs = renderedDone->readbackMs;
                    mergedJpeg.output.filmRendered = renderedDone->filmRendered;
                    mergedJpeg.output.filmFallbackMemory = renderedDone->filmFallbackMemory;
                    // Demosaic covers the CFA packing and the Dual demosaic.
                    mergedJpeg.output.demosaicMs = rgbPrepMs + demosaicMs;
                    mergedJpeg.output.renderTotalMs = mergedJpeg.output.demosaicMs + renderedDone->totalMs;
                    // The JPEG description is what gallery apps (e.g. Google Photos)
                    // show. Assembled here — after render — so the film block
                    // reflects what actually rendered rather than what was
                    // requested: film can fall back to tonemap (RAM gate, record
                    // fault) after the intent was frozen. The writer appends the
                    // unified TIMINGS section post-encode. (The merged DNG keeps
                    // the shutter-time intent: it is written before render runs.)
                    {
                        const bool filmActual =
                            mergedJpeg.output.filmRendered && !mergedJpeg.output.filmDescription.empty();
                        const std::string filmBlock = filmActual ? mergedJpeg.output.filmDescription : "";
                        mergedJpeg.output.imageDescription =
                            buildExifHeader() +
                            buildParametersBlock(tuningView, outputInfo, toneUi, filmBlock, filmActual,
                                                 mergeResult->frameCount, mergeResult->outputExtent.width,
                                                 mergeResult->outputExtent.height) +
                            buildSharpnessSection(baseFrameMode, sharpnessScores, job->referenceIndex,
                                                  static_cast<std::uint32_t>(job->frames.size()));
                        // mergeResult is engaged here (a run failure jumps to the
                        // outer catch); guard anyway so a future early-exit path
                        // can never emit an all-zero multiframe block.
                        mergedJpeg.output.hasMultiframeTimings = mergeResult.has_value();
                        mergedJpeg.output.multiframeTimingsBlock = formatMultiframeTimings(mfStages, baseFrameMode);
                        mergedJpeg.output.multiframeTotalMs = mfStages.multiframeTotalMs();
                        mergedJpeg.output.mfBaseReadMs = baseReadMs;
                        mergedJpeg.output.mfProjectMs = projectMs;
                        mergedJpeg.output.mfRgbPrepMs = rgbPrepMs;
                        mergedJpeg.output.mfDirectRgb = false;
                    }
                    JpegCaptureWriter jpegWriter;
                    auto jpegContext = std::move(mergedJpeg);
                    jpegContext.output.appliedLensShadingCorrection = jpegContext.develop.lensShadingCorrectionEnabled;
                    jpegContext.output.appliedFccSteps = jpegContext.develop.fccSteps;
                    mergedJpeg.output.outputFd = -1;
                    const bool mfUltraHdr = jpegContext.output.ultraHdr.enabled && rendered.gainmapPixelData() &&
                                            rendered.gainmapPixelBytes() > 0;
                    bool mfWriterAccepted = false;
                    if (mfUltraHdr) {
                        mfWriterAccepted = jpegWriter.startUltraHdr(
                            requestId, rendered.pixelData(), rendered.pixelBytes(), renderedDone->width,
                            renderedDone->height, rendered.gainmapPixelData(), rendered.gainmapPixelBytes(),
                            rendered.gainmapWidth(), rendered.gainmapHeight(), std::move(jpegContext.output));
                    } else {
                        if (jpegContext.output.ultraHdr.enabled)
                            LOGI("ULTRAHDR_MAP_MISSING requestId=%llu fallback=legacy_jpeg",
                                 static_cast<unsigned long long>(requestId));
                        mfWriterAccepted =
                            jpegWriter.start(requestId, rendered.pixelData(), rendered.pixelBytes(),
                                             renderedDone->width, renderedDone->height, std::move(jpegContext.output));
                    }
                    if (!mfWriterAccepted) {
                        throw std::runtime_error("merged_jpeg_writer_start_rejected");
                    }
                    std::optional<JpegWriteCompletion> jpegDone;
                    while (!jpegDone) {
                        jpegDone = jpegWriter.pollCompletion();
                        if (!jpegDone) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }
                    }
                    publishJpeg(*jpegDone);
                    jpegPublished = true;
                    rendered.releasePixels();
                }
            } catch (const std::exception& e) {
                if (mergedJpeg.output.outputFd >= 0) {
                    close(mergedJpeg.output.outputFd);
                    mergedJpeg.output.outputFd = -1;
                }
                // Release mapped pixels so a worker exception cannot poison
                // the persistent engine: start() rejects while mapped_ is set.
                rendered.releasePixels();
                failJpeg(std::string("merged_jpeg_failed: ") + e.what());
                jpegPublished = true;
            }
        }
        dumpRzsl();
    } catch (const std::exception& e) {
        if (mergedDng.outputFd >= 0) {
            close(mergedDng.outputFd);
            mergedDng.outputFd = -1;
        }
        if (!mergedPublished) {
            failDng(mergedDisplayName, std::string("merge_failed: ") + e.what());
            mergedPublished = true;
        }
        if (mergedJpeg.output.outputFd >= 0) {
            close(mergedJpeg.output.outputFd);
            mergedJpeg.output.outputFd = -1;
        }
        if (!jpegPublished) {
            failJpeg(std::string("merge_failed: ") + e.what());
            jpegPublished = true;
        }
        rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
            rawrcam::diagnostics::RuntimeTraceStage::MultiframeFail, 0, 0, -1, 0, 0, 1u, 0);
        LOGE("MULTIFRAME_GPU_FAIL %s", e.what());
    }
    dumpRzsl();

    auto awaitDng = [&](DngCaptureWriter& writer, bool started, bool& published) {
        if (!started || published) return;
        std::optional<DngWriteCompletion> done;
        while (!done) {
            done = writer.pollCompletion();
            if (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        publishDng(*done);
        published = true;
    };
    awaitDng(baseWriter, baseStarted, basePublished);
    awaitDng(mergedWriter, mergedStarted, mergedPublished);
    if (!basePublished) failDng(baseDisplayName, "base_dng_not_started");
    if (!mergedPublished) {
        if (mergedDng.outputFd >= 0) close(mergedDng.outputFd);
        failDng(mergedDisplayName, "merged_dng_not_started");
    }
    if (!jpegPublished && jpegRequested) {
        if (mergedJpeg.output.outputFd >= 0) close(mergedJpeg.output.outputFd);
        failJpeg("merged_jpeg_not_started");
    }
    job->snapshot.reset();
}

}  // namespace rawrcam::capture::multiframe
