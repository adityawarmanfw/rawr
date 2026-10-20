#include <sys/system_properties.h>
#include "pipeline/RealtimePipeline.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "diagnostics/logging/FrameAuditWriter.h"
#include "diagnostics/probes/PipelineDiagnostics.h"
#include "diagnostics/probes/RawCpuCopyProbe.h"
#include "diagnostics/probes/RawIntegrityProbe.h"
#include "diagnostics/timing/GpuTimingTracker.h"
#include "geometry/RawGeometry.h"
#include "imaging/FrameLimits.h"
#include "metadata/MetadataDiagnostics.h"
#include "monitoring/MonitoringCoordinator.h"
#include "pipeline/FrameSlotPool.h"
#include "pipeline/FrameSubmitCoordinator.h"
#include "pipeline/PreviewLookController.h"
#include "pipeline/RawDevelopRecorder.h"
#include "presentation/SwapchainRenderer.h"
#include "raw_preview.h"
#include "raw_preview/RawPreview.hpp"
#include "raw_preview_cfa_state.h"
#include "raw_preview_coloropp.h"
#include "raw_preview_coloropp_tone.h"
#include "raw_preview_highlight_apply.h"
#include "raw_preview_highlight_guide_propagate.h"
#include "raw_preview_highlight_guide_seed.h"
#include "raw_preview_highlight_guide_smooth.h"
#include "tonemap/ColorRenderProfile.h"
#include "tonemap/ImportedLutProfileStore.h"
#include "vulkan/RawAhbImporter.h"
#include "vulkan/VulkanContext.h"
#include "vulkan/VulkanDispatch.h"

namespace rawrcam::pipeline {
namespace {
// Wider RAW frames are vendor full-size modes (8160 px); 12 MP sensors are ~4096.
constexpr uint32_t kFullSizeRawWidth = 6000;
}  // namespace

namespace {
#define SESSION_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", __VA_ARGS__)
#define SESSION_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", __VA_ARGS__)
void vkCheck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
}
}  // namespace
RealtimePipeline::RealtimePipeline(std::string rawShaderPath, std::string rawCfaStateShaderPath, std::string filesDir,
                                   vulkan::VulkanContext& context, PreviewLookController& look,
                                   monitoring::MonitoringCoordinator& monitoring,
                                   diagnostics::RawIntegrityProbe& integrity,
                                   diagnostics::PipelineDiagnostics& diagnostics,
                                   presentation::SwapchainRenderer& presentation,
                                   diagnostics::RawCpuCopyProbe& cpuProbe, FrameLifecyclePort& lifecycle,
                                   FrameCapturePort& capture, FrameDiagnosticsPort& feedback, std::mutex& queueMutex,
                                   Diagnostic diagnostic, Audit audit)
    : rawShaderPath_(std::move(rawShaderPath)),
      rawCfaStateShaderPath_(std::move(rawCfaStateShaderPath)),
      filesDir_(std::move(filesDir)),
      vulkanContext_(context),
      look_(look),
      monitoring_(monitoring),
      rawIntegrityProbe_(integrity),
      pipelineDiagnostics_(diagnostics),
      swapchainRenderer_(presentation),
      capturePort_(capture),
      rawCpuCopyProbe_(cpuProbe),
      queueSubmitMutex_(queueMutex),
      diagnostic_(std::move(diagnostic)),
      audit_(std::move(audit)),
      performanceTracker_([this](const std::string& line) { emit(line); }),
      realtime_(context, frameSlots_, performanceTracker_, *this, presentation, lifecycle, capture, feedback,
                queueMutex, filesDir_) {
    std::ofstream out(rawShaderPath_, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot create raw_preview shader file");
    out.write(reinterpret_cast<const char*>(raw_preview_spv), static_cast<std::streamsize>(raw_preview_spv_size));
    if (!out) throw std::runtime_error("Cannot write raw_preview shader file");
    std::ofstream stateOut(rawCfaStateShaderPath_, std::ios::binary | std::ios::trunc);
    if (!stateOut) throw std::runtime_error("Cannot create raw_preview CFA-state shader file");
    stateOut.write(reinterpret_cast<const char*>(raw_preview_cfa_state_spv),
                   static_cast<std::streamsize>(raw_preview_cfa_state_spv_size));
    if (!stateOut) throw std::runtime_error("Cannot write raw_preview CFA-state shader file");
    auto writeShader = [&](const char* name, const unsigned char* bytes, size_t count) {
        std::ofstream shaderOut(filesDir_ + "/" + name, std::ios::binary | std::ios::trunc);
        if (!shaderOut) throw std::runtime_error(std::string("Cannot create ") + name);
        shaderOut.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(count));
        if (!shaderOut) throw std::runtime_error(std::string("Cannot write ") + name);
    };
    writeShader("raw_preview_highlight_guide_seed.comp.spv", raw_preview_highlight_guide_seed_spv,
                raw_preview_highlight_guide_seed_spv_size);
    writeShader("raw_preview_highlight_guide_propagate.comp.spv", raw_preview_highlight_guide_propagate_spv,
                raw_preview_highlight_guide_propagate_spv_size);
    writeShader("raw_preview_highlight_guide_smooth.comp.spv", raw_preview_highlight_guide_smooth_spv,
                raw_preview_highlight_guide_smooth_spv_size);
    writeShader("raw_preview_coloropp.comp.spv", raw_preview_coloropp_spv, raw_preview_coloropp_spv_size);
    writeShader("raw_preview_coloropp_tone.comp.spv", raw_preview_coloropp_tone_spv,
                raw_preview_coloropp_tone_spv_size);
    writeShader("raw_preview_highlight_apply.comp.spv", raw_preview_highlight_apply_spv,
                raw_preview_highlight_apply_spv_size);
}

RealtimePipeline::~RealtimePipeline() = default;
void RealtimePipeline::initializeDevice() {
    rawAhbImporter_ = std::make_unique<vulkan::RawAhbImporter>(
        vulkanContext_.physicalDevice(), vulkanContext_.device(), vulkanContext_.queueFamily(),
        vulkanContext_.getAhbProperties(), [this](const std::string& line) { emit(line); });
    frameSlots_.initialize(vulkanContext_.physicalDevice(), vulkanContext_.device(), vulkanContext_.queueFamily());
    performanceTracker_.initialize(vulkanContext_.device(), vulkanContext_.timestampPeriod(),
                                   imaging::kRealtimeFramesInFlight);
}
void RealtimePipeline::releaseProcessingResourcesForFrozenReplay() noexcept {
    if (vulkanContext_.device()) destroyCameraResources(false, false);
    if (rawAhbImporter_) rawAhbImporter_->clear();
}
void RealtimePipeline::shutdown() noexcept {
    if (vulkanContext_.device()) destroyCameraResources();
    realtime_.shutdownDeviceResources();
    rawAhbImporter_.reset();
    performanceTracker_.destroy();
    frameSlots_.destroy();
}
bool RealtimePipeline::configureCamera(uint64_t generation, const metadata::CameraContextMetadata& metadata) {
    return configureGeometry(generation, metadata.geometry.rawBufferWidth, metadata.geometry.rawBufferHeight,
                             metadata.rawPreviewCfa, metadata.cfaBlock, metadata.cfaLiveQuality,
                             metadata.baselineBlackLevelPhysicalRggb,
                             metadata.baselineWhiteLevel, metadata.geometry.sensorOrientationDegrees);
}
void RealtimePipeline::emit(const std::string& line) const {
    if (diagnostic_) diagnostic_(line);
}
void RealtimePipeline::ensureMultiframeBridge() {
    if (!configured_ || !vulkanContext_.device()) return;
    bool allocated = false;
    {
        // In-flight slots may still reference the old (null) bridge; idle the
        // queue before publishing new images into them.
        std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
        vkCheck(vkQueueWaitIdle(vulkanContext_.queue()), "vkQueueWaitIdle multiframe bridge");
        allocated = frameSlots_.ensureBridgeImages(rawWidth_, rawHeight_, vulkanContext_.queue());
    }
    if (!allocated) return;
    emit("FRAME_SLOTS_BRIDGE_COPY retained reason=multiframe_live generation=" + std::to_string(generation_));
    SESSION_LOGI("FRAME_SLOTS_BRIDGE_COPY retained reason=multiframe_live generation=%llu",
                 static_cast<unsigned long long>(generation_));
}
void RealtimePipeline::createFrameProcessor() {
    frameProcessor_ = std::make_unique<pipeline::RawDevelopRecorder>(
        *rawPreview_, *look_.tone(), [this] { return look_.acquireFilm(); }, pipelineDiagnostics_, rawIntegrityProbe_,
        monitoring_.overlay(), monitoring_.scopes(), swapchainRenderer_, performanceTracker_,
        [this](const std::string& line) {
            if (audit_) audit_(line);
        },
        localTone_.get());
}
bool RealtimePipeline::setColorRenderProfile(tonemap_integration::ColorRenderProfile profile,
                                             std::string importedProfileId) {
    if (!look_.selectProfile(profile, std::move(importedProfileId))) return false;
    if (look_.tone()) createFrameProcessor();
    return true;
}
void RealtimePipeline::setScopeDeviceRotationDegrees(int rotation) {
    scopeDeviceRotationDegrees_ = ((rotation % 360) + 360) % 360;
    realtime_.setScopeDeviceRotationDegrees(scopeDeviceRotationDegrees_);
}
bool RealtimePipeline::configureGeometry(uint64_t generation, uint32_t width, uint32_t height, uint32_t cfa,
                                         uint32_t cfaBlock, uint32_t cfaQuality, const std::array<float, 4>& black, float white, int sensorOrientationDegrees) {
    bool oldResourcesDestroyed = false;
    bool preserveHqStillAcrossReconfigure = false;
    try {
        if (!vulkanContext_.device()) {
            throw std::runtime_error("Vulkan surface/device not ready");
        }
        if (!rawrcam::geometry::isSupportedRawGeometry(width, height)) {
            throw std::runtime_error("Unsupported RAW geometry (needs even sides within limits)");
        }

        preserveHqStillAcrossReconfigure = capturePort_.detachedCaptureActive();
        if (preserveHqStillAcrossReconfigure) {
            emit("STILL_FOREGROUND_CAMERA_RECONFIG_PRESERVE hqStillPreserved=true generation=" +
                 std::to_string(generation));
        } else {
            capturePort_.resetStillProcessing();
        }

        // Preview resources are device-local but independent from the still
        // processors. Waiting for current GPU work is sufficient before
        // destroying/replacing preview images; do not reset an already-owned
        // still snapshot or its demosaic/render/JPEG state.
        emit("GEOMETRY_GPU_WAIT_BEGIN generation=" + std::to_string(generation));
        SESSION_LOGI("GEOMETRY_GPU_WAIT_BEGIN generation=%llu", static_cast<unsigned long long>(generation));
        vulkanContext_.waitIdle();
        emit("GEOMETRY_GPU_WAIT_DONE generation=" + std::to_string(generation));
        SESSION_LOGI("GEOMETRY_GPU_WAIT_DONE generation=%llu", static_cast<unsigned long long>(generation));
        realtime_.releaseCompletedSlots(true);
        emit("GEOMETRY_SLOTS_RELEASED generation=" + std::to_string(generation));
        destroyCameraResources(preserveHqStillAcrossReconfigure, /*teardownFilm=*/false);
        emit("GEOMETRY_RESOURCES_DESTROYED generation=" + std::to_string(generation));
        oldResourcesDestroyed = true;

        generation_ = generation;
        rawWidth_ = width;
        rawHeight_ = height;
        // The viewfinder bins 2x2 Bayer cells; full-size (50 MP) frames bin
        // 4x4 so the live pipeline stays at the ~12 MP sensors' cost.
        const uint32_t bin = width > kFullSizeRawWidth ? 4u : 2u;
        previewWidth_ = width / bin;
        previewHeight_ = height / bin;
        cfa_ = cfa;
        cfaBlock_ = cfaBlock;
        cfaQuality_ = cfaQuality;
        black_ = black;
        white_ = white;
        sensorOrientationDegrees_ = sensorOrientationDegrees;

        rawCpuCopyProbe_.configure(width, height);
        capturePort_.configureStillSnapshot(width, height, generation);

        raw_preview::RawPreviewCreateInfo rawCreate{};
        rawCreate.physicalDevice = vulkanContext_.physicalDevice();
        rawCreate.device = vulkanContext_.device();
        rawCreate.shaderPath = rawShaderPath_;
        rawCreate.cfaStateShaderPath = rawCfaStateShaderPath_;
        rawCreate.highlightGuideSeedShaderPath = filesDir_ + "/raw_preview_highlight_guide_seed.comp.spv";
        rawCreate.highlightGuidePropagateShaderPath = filesDir_ + "/raw_preview_highlight_guide_propagate.comp.spv";
        rawCreate.highlightGuideSmoothShaderPath = filesDir_ + "/raw_preview_highlight_guide_smooth.comp.spv";
        rawCreate.highlightApplyShaderPath = filesDir_ + "/raw_preview_highlight_apply.comp.spv";
        rawCreate.coloroppShaderPath = filesDir_ + "/raw_preview_coloropp.comp.spv";
        rawCreate.coloroppToneShaderPath = filesDir_ + "/raw_preview_coloropp_tone.comp.spv";
        rawCreate.workgroupX = 8;
        rawCreate.workgroupY = 8;
        rawCreate.conditionalRendering = vulkanContext_.conditionalRenderingEnabled();
        {
            char value[PROP_VALUE_MAX]{};
            // A/B switch for the clip-predicated highlight chain.
            if (__system_property_get("debug.rawr.hl_predicate", value) > 0 && value[0] == '0')
                rawCreate.conditionalRendering = false;
        }
        rawPreview_ = std::make_unique<raw_preview::RawPreview>(rawCreate);
        if (rawAhbImporter_)
            rawAhbImporter_->setEvictListener([preview = rawPreview_.get()](const vulkan::ImportedRaw& raw) {
                preview->forgetInput(raw.view, raw.importBuffer);
            });
        try {
            localTone_ = std::make_unique<rawr::local_tone::ExposureFusionProcessor>(
                rawr::vk::GpuContext{vulkanContext_.physicalDevice(), vulkanContext_.device(),
                                     vulkanContext_.queueFamily()},
                imaging::kRealtimeFramesInFlight);
        } catch (const std::exception& e) {
            localTone_.reset();  // The viewfinder runs without local tone mapping.
            SESSION_LOGI("LOCAL_TONE_PREVIEW_UNAVAILABLE %s", e.what());
        }
        SESSION_LOGI("RAW_PREVIEW_HIGHLIGHT_PREDICATE conditionalRendering=%d",
                     rawCreate.conditionalRendering ? 1 : 0);

        look_.createTone();
        // Film engine follows the baked look stored via applyFilmSimState.
        // Recreate only when missing (arena sized for current geometry);
        // geometry shrink reuses the larger arena, growth recreates below.
        // No look yet (startup race) -> skip: applyFilmSimState creates once
        // the first real look arrives.
        look_.configureGeometry(previewWidth_, previewHeight_);

        // Skip the owned R16 bridge image unless some consumer needs it: the
        // bridge fallback itself, the multiframe ring (fed from the owned
        // copy), the copy visualizations (modes 6/9), or the sweep/oracle
        // harnesses (modes 11-16, whose references are bridge outputs).
        // Everywhere else demosaic reads the import buffer (or the
        // import image directly), so the ~25MB/slot image stays unallocated.
        // Mid-session multiframe enables allocate it in place
        // (ensureMultiframeBridge); other mid-session switches into a
        // bridge-needing mode degrade to logged frame drops via the recorder
        // guards.
        const bool bufferImportCapable =
            (vulkanContext_.ahbExternalBufferFeatures() & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
        const uint32_t diagMode = realtime_.diagnosticMode();
        const bool bridgeNeeded = !bufferImportCapable || capturePort_.multiframeEnabled() || diagMode == 6u ||
                                  diagMode == 9u || (diagMode >= 11u && diagMode <= 16u);
        if (audit_) {
            audit_(std::string("FRAME_SLOTS_BRIDGE_COPY ") + (bridgeNeeded ? "retained" : "released") +
                   " bufferImportCapable=" + (bufferImportCapable ? "yes" : "no") + " multiframe=" +
                   (capturePort_.multiframeEnabled() ? "on" : "off") + " diagnosticMode=" + std::to_string(diagMode));
        }
        SESSION_LOGI("FRAME_SLOTS_BRIDGE_COPY %s multiframe=%s diagnosticMode=%u",
                     bridgeNeeded ? "retained" : "released", capturePort_.multiframeEnabled() ? "on" : "off", diagMode);
        auto logHeapUsage = [&](const char* stage) {
            // Resolve per-instance: some loaders only expose physical-device
            // entry points that way (see VulkanContext buffer probe).
            auto getMemProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
                vkGetInstanceProcAddr(vulkanContext_.instance(), "vkGetPhysicalDeviceMemoryProperties2"));
            if (getMemProps2 == nullptr) getMemProps2 = vkGetPhysicalDeviceMemoryProperties2;
            if (getMemProps2 == nullptr) return;
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
            budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
            VkPhysicalDeviceMemoryProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
            props2.pNext = &budget;
            getMemProps2(vulkanContext_.physicalDevice(), &props2);
            std::ostringstream h;
            h << "FRAME_SLOTS_HEAP stage=" << stage;
            for (uint32_t i = 0; i < props2.memoryProperties.memoryHeapCount; ++i) {
                h << " heap" << i << "Flags=0x" << std::hex << props2.memoryProperties.memoryHeaps[i].flags << std::dec
                  << " budget=" << budget.heapBudget[i] << " usage=" << budget.heapUsage[i];
            }
            SESSION_LOGI("%s", h.str().c_str());
        };
        logHeapUsage("pre_images");
        emit("GEOMETRY_IMAGES_CREATE_BEGIN generation=" + std::to_string(generation));
        SESSION_LOGI("GEOMETRY_IMAGES_CREATE_BEGIN generation=%llu", static_cast<unsigned long long>(generation));
        frameSlots_.createImages(rawWidth_, rawHeight_, previewWidth_, previewHeight_, bridgeNeeded);
        emit("GEOMETRY_IMAGES_CREATE_DONE generation=" + std::to_string(generation));
        SESSION_LOGI("GEOMETRY_IMAGES_CREATE_DONE generation=%llu", static_cast<unsigned long long>(generation));
        {
            std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
            frameSlots_.transitionImagesToGeneral(vulkanContext_.queue());
        }
        logHeapUsage("post_images");

        {
            std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
            monitoring_.initialize(vulkanContext_.physicalDevice(), vulkanContext_.device(), vulkanContext_.queue(),
                                   frameSlots_.commandPool(), vulkanContext_.queueFamily(),
                                   vulkanContext_.timestampPeriod(), rawWidth_, rawHeight_, previewWidth_,
                                   previewHeight_);
        }

        rawIntegrityProbe_.initialize(vulkanContext_.physicalDevice(), vulkanContext_.device(), rawWidth_, rawHeight_,
                                      previewWidth_, previewHeight_, rawrcam::imaging::kRealtimeFramesInFlight);

        {
            std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> rawCopyViews{};
            std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> linearViews{};
            std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> tonemappedViews{};
            for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i) {
                rawCopyViews[i] = frameSlots_[i].rawCopy.view;
                linearViews[i] = frameSlots_[i].linear.view;
                tonemappedViews[i] = frameSlots_[i].tonemapped.view;
            }
            pipelineDiagnostics_.initialize(vulkanContext_.device(), rawWidth_, rawHeight_, previewWidth_,
                                            previewHeight_, rawCopyViews, linearViews, tonemappedViews);
        }

        createFrameProcessor();

        {
            std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> tonemappedViews{};
            std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> linearViews{};
            std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> overlayViews{};
            std::array<std::array<VkImageView, 3>, rawrcam::imaging::kRealtimeFramesInFlight> scopeViews{};
            for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i) {
                tonemappedViews[i] = frameSlots_[i].tonemapped.view;
                linearViews[i] = frameSlots_[i].linear.view;
                overlayViews[i] = monitoring_.overlay().overlayView(i);
                for (uint32_t j = 0; j < 3; ++j) {
                    scopeViews[i][j] = monitoring_.scopes().renderedView(i, j);
                }
            }
            swapchainRenderer_.setFrameSources(tonemappedViews, linearViews, overlayViews, scopeViews,
                                               rawrcam::monitoring::ImageScopesProcessor::renderAspect());
            swapchainRenderer_.setScopePresentationState(monitoring_.scopes().presentationState());
        }

        configured_ = true;
        realtime_.configure(pipeline::FrameSubmitConfiguration{
            generation_, rawWidth_, rawHeight_, previewWidth_, previewHeight_, cfa_, cfaBlock_, cfaQuality_,
            sensorOrientationDegrees_,
            realtime_.diagnosticMode(), realtime_.experimentalZeroCopy(), realtime_.lensShadingCorrectionEnabled()});
        realtime_.setDisplayRotationDegrees(displayRotationDegrees_);
        realtime_.setScopeDeviceRotationDegrees(scopeDeviceRotationDegrees_);

        if (audit_) {
            audit_("PIPELINE_CONFIGURED generation=" + std::to_string(generation_) +
                   " raw=" + std::to_string(rawWidth_) + "x" + std::to_string(rawHeight_) +
                   " preview=" + std::to_string(previewWidth_) + "x" + std::to_string(previewHeight_));
        }

        std::ostringstream out;
        out << "CAMERA_CONFIG generation=" << generation_ << " raw=" << rawWidth_ << "x" << rawHeight_
            << " preview=" << previewWidth_ << "x" << previewHeight_ << " cfa=" << cfa_ << " white=" << white_
            << " black=" << black_[0] << "," << black_[1] << "," << black_[2] << "," << black_[3]
            << " sensorOrientation=" << sensorOrientationDegrees_ << " geometryClass=production";
        emit(out.str());
        SESSION_LOGI(
            "configured generation=%llu RAW=%ux%u preview=%ux%u CFA=%u white=%.1f "
            "black=[%.1f %.1f %.1f %.1f] sensorOrientation=%d",
            static_cast<unsigned long long>(generation_), rawWidth_, rawHeight_, previewWidth_, previewHeight_, cfa_,
            white_, black_[0], black_[1], black_[2], black_[3], sensorOrientationDegrees_);
        return true;
    } catch (const std::exception& e) {
        // Preserve an existing valid configuration when validation fails before
        // teardown. Once old resources have been retired, however, any partially
        // constructed replacement must be destroyed before returning failure.
        if (oldResourcesDestroyed) destroyCameraResources(preserveHqStillAcrossReconfigure, /*teardownFilm=*/false);
        SESSION_LOGE("configureCamera failed: %s", e.what());
        emit(std::string("CAMERA_CONFIG_FAILURE ") + e.what());
        return false;
    }
}
void RealtimePipeline::destroyCameraResources(bool preserveHqStill, bool teardownFilm) noexcept {
    realtime_.resetConfiguration();
    if (!preserveHqStill) {
        capturePort_.resetStillProcessing();
    }
    frameProcessor_.reset();
    localTone_.reset();
    swapchainRenderer_.clearFrameSources();
    rawIntegrityProbe_.reset();
    pipelineDiagnostics_.reset();
    monitoring_.reset();
    if (rawAhbImporter_) rawAhbImporter_->setEvictListener({});
    rawPreview_.reset();
    look_.clearProcessing(teardownFilm);
    frameSlots_.destroyImages();
    realtime_.releaseCpuUpload();
    if (rawAhbImporter_) rawAhbImporter_->clear();
    configured_ = false;
}
}  // namespace rawrcam::pipeline
