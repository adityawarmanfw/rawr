#include "develop/demosaic/DualStillProcessor.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "develop/highlight/LensShadingMapSnapshot.h"
#include "geometry/CfaPattern.h"
#include "geometry/RawGeometry.h"
#include "raw_demosaic/EmbeddedShaders.hpp"

namespace rawrcam::develop::demosaic::dual {

DualStillProcessor::DualStillProcessor(Diagnostic diagnostic) : worker_(diagnostic), runtime_("RCDVNG", diagnostic) {}
DualStillProcessor::~DualStillProcessor() { reset(); }

const char* DualStillProcessor::algorithmName() noexcept { return "RCD+VNG4"; }
::dual::BayerPattern DualStillProcessor::dualPattern(uint32_t cfa) {
    return rawrcam::geometry::toBackendPattern<::dual::BayerPattern>(cfa, "unsupported RawrCam CFA for Dual");
}

void DualStillProcessor::configure(const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex,
                                   uint64_t generation, const std::string& cameraId, uint32_t width, uint32_t height,
                                   uint32_t cfa, bool dualAutoContrast, float dualContrastPercent,
                                   bool diagnosticsEnabled, rawrcam::develop::StillDemosaicGeometry geometry,
                                   VkQueue queueOverride, std::mutex* queueSubmitMutexOverride,
                                   bool externalPackedInput) {
    if (!context.physicalDevice() || !context.device() || !context.queue())
        throw std::invalid_argument("RCD/VNG requires initialized Vulkan context");
    const bool reconstructedGeometry = geometry == rawrcam::develop::StillDemosaicGeometry::ReconstructedCfa;
    const bool geometrySupported = reconstructedGeometry
                                       ? width >= 20u && height >= 20u && (width & 1u) == 0u && (height & 1u) == 0u
                                       : rawrcam::geometry::isSupportedRawGeometry(width, height);
    if (!geometrySupported) throw std::invalid_argument("RCD/VNG geometry is not a supported RAW geometry");
    if (!generation || cameraId.empty()) throw std::invalid_argument("RCD/VNG requires immutable camera identity");
    (void)dualPattern(cfa);
    if (configured()) {
        if (cameraContextGeneration_ != generation || cameraId_ != cameraId || width_ != width || height_ != height ||
            cfa_ != cfa || dualAutoContrast_ != dualAutoContrast ||
            std::abs(dualContrastPercent_ - dualContrastPercent) > 1e-6f || diagnosticsEnabled_ != diagnosticsEnabled ||
            geometry_ != geometry || runtime_.device() != context.device())
            throw std::logic_error("RCD/VNG reconfiguration requires reset after GPU completion");
        runtime_.configure(context, queueSubmitMutex, width, height, cfa, true, queueOverride, queueSubmitMutexOverride,
                           externalPackedInput);
        pipelineCache_ = context.pipelineCache();
        return;
    }
    runtime_.configure(context, queueSubmitMutex, width, height, cfa, true, queueOverride, queueSubmitMutexOverride,
                       externalPackedInput);
    pipelineCache_ = context.pipelineCache();
    cameraContextGeneration_ = generation;
    cameraId_ = cameraId;
    width_ = width;
    height_ = height;
    cfa_ = cfa;
    dualAutoContrast_ = dualAutoContrast;
    dualContrastPercent_ = std::clamp(dualContrastPercent, 0.0f, 100.0f);
    diagnosticsEnabled_ = diagnosticsEnabled;
    geometry_ = geometry;
    std::ostringstream out;
    out << "RCDVNG_STILL_CONFIG_ACCEPTED algorithm=" << algorithmName() << " cameraId=" << cameraId_
        << " generation=" << generation << " raw=" << width_ << 'x' << height_
        << " inputMode=" << (externalPackedInput ? "external_packed" : "shared_highlight_packed")
        << " singleFlight=true async=true lazy=true"
        << " geometry=" << (reconstructedGeometry ? "reconstructed_cfa" : "sensor_native")
        << " diagnostics=" << (diagnosticsEnabled_ ? "on" : "off");
    out << " dualContrastMode=" << (dualAutoContrast_ ? "auto" : "manual")
        << " requestedContrast=" << dualContrastPercent_ << " optimization=vng_export_blend";
    emit(out.str());
}

void DualStillProcessor::ensureRuntimeResources() {
    if (dual_) return;
    runtime_.ensureResources();
    {
        ::dual::PipelineConfig cfg{};
        cfg.autoBalance = true;
        cfg.width = width_;
        cfg.height = height_;
        cfg.pattern = dualPattern(cfa_);
        cfg.inputMode = ::dual::InputMode::PackedCfaRgba16fImage;
        cfg.outputScale = 1.0f / 255.0f;
        cfg.outputAlpha = 1.0f;
        cfg.telemetry = false;
        cfg.contrastPercent = dualContrastPercent_;
        cfg.autoContrast = dualAutoContrast_;
        cfg.telemetry = true;
        cfg.diagnosticBranchOutputs = diagnosticsEnabled_;
        cfg.optimizationMode = ::dual::OptimizationMode::VngExportBlend;
        ::dual::VulkanContext dc{runtime_.physicalDevice(), runtime_.device(), runtime_.queueFamily(), nullptr,
                                 pipelineCache_};
        dual_ = std::make_unique<::dual::DualDemosaicPipeline>(dc, raw_demosaic::embeddedDualShaders(), cfg);
    }
    emit(std::string("RCDVNG_STILL_INIT_PIPELINE_PASS algorithm=") + algorithmName() +
         " persistentBytes=" + std::to_string(currentAllocatedBytes()));
}

bool DualStillProcessor::busy() const noexcept { return worker_.busy(); }

bool DualStillProcessor::start(const rawrcam::imaging::RawSnapshot& frame,
                               std::shared_ptr<const std::vector<uint8_t>> raw, bool lensShadingCorrectionEnabled,
                               bool highlightReconstructionEnabled, const ::galosh::GaloshRawParams& galosh) {
    if (!configured()) return false;
    const uint64_t expected = static_cast<uint64_t>(width_) * height_ * sizeof(uint16_t);
    if (frame.width != width_ || frame.height != height_ || expected > std::numeric_limits<size_t>::max() || !raw ||
        raw->size() != static_cast<size_t>(expected))
        return false;
    if (!frame.metadata.cameraContext || !(frame.metadata.effectiveWhiteLevel > 0.0f)) return false;
    if (frame.metadata.cameraContext->cameraContextGeneration != cameraContextGeneration_ ||
        frame.metadata.cameraContext->cameraId != cameraId_)
        return false;
    if (!worker_.tryClaim()) return false;
    joinWorker();
    const uint64_t requestId = frame.requestId, timestampNs = frame.timestampNs;
    const auto black = frame.metadata.blackLevelPhysicalRggb;
    const float white = frame.metadata.effectiveWhiteLevel;
    const auto wb = frame.colorState.baselineWbRggb;
    const auto lensShading =
        rawrcam::develop::highlight::LensShadingMapSnapshot::fromMetadata(frame.metadata, lensShadingCorrectionEnabled);
    worker_.spawn([this, requestId, timestampNs, black, white, wb, lensShading, highlightReconstructionEnabled, galosh,
                   raw = std::move(raw)]() mutable {
        StillCompletion done{};
        done.requestId = requestId;
        done.timestampNs = timestampNs;
        const auto started = std::chrono::steady_clock::now();
        try {
            ensureRuntimeResources();
            done.setupMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            VkCommandBuffer cmd =
                runtime_.beginFrame(*raw, black, white, wb, lensShading, highlightReconstructionEnabled, galosh);
            raw.reset();
            done.persistentBytes = currentAllocatedBytes();
            // Bounded submissions (upload/highlight, then each demosaic pass) so the
            // preview queue is not starved for the whole still.
            runtime_.flush();
            dual_->setPassBoundary([this] { runtime_.flush(); });
            {
                ::dual::PackedCfaImageView in{runtime_.packedImage(),
                                              runtime_.packedView(),
                                              VK_FORMAT_R16G16B16A16_SFLOAT,
                                              VK_IMAGE_LAYOUT_GENERAL,
                                              width_ / 2u,
                                              height_ / 2u,
                                              width_,
                                              height_,
                                              dualPattern(cfa_)};
                ::dual::LinearRgbImage out{runtime_.outputImage(),
                                           runtime_.outputView(),
                                           VK_FORMAT_R16G16B16A16_SFLOAT,
                                           VK_IMAGE_LAYOUT_GENERAL,
                                           width_,
                                           height_};
                dual_->record(cmd, in, out);
            }
            runtime_.submit(cmd);
            runtime_.waitForCompletion();
            if (dual_) {
                std::ostringstream diag;
                diag << "DUAL_CONTRAST_RESOLVED requestId=" << requestId
                     << " mode=" << (dualAutoContrast_ ? "auto" : "manual")
                     << " requestedContrast=" << dualContrastPercent_
                     << " resolvedContrast=" << dual_->resolvedContrastPercent()
                     << " autoTileSize=" << dual_->autoDetectionTileSize();
                emit(diag.str());
                ::dual::FrameTelemetry telemetry{};
                if (dual_->collectTelemetry(telemetry)) {
                    double autoMs = 0.0;
                    std::ostringstream timing;
                    timing << "DUAL_GPU_TIMING requestId=" << requestId;
                    for (const auto& event : telemetry.gpuEvents) {
                        if (event.name.rfind("dual.auto_", 0) == 0) autoMs += event.milliseconds;
                        timing << ' ' << event.name << "Ms=" << event.milliseconds;
                    }
                    timing << " autoTotalMs=" << autoMs;
                    emit(timing.str());
                }
            }
            done.success = true;
        } catch (const std::exception& e) {
            done.error = e.what();
        }
        done.submitToFenceMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        worker_.finish(std::move(done));
    });
    return true;
}

bool DualStillProcessor::startExternalPacked(VkImage packedImage, VkImageView packedView, uint64_t requestId,
                                             uint64_t timestampNs) {
    if (!configured() || packedImage == VK_NULL_HANDLE || packedView == VK_NULL_HANDLE) return false;
    if (!worker_.tryClaim()) return false;
    joinWorker();
    worker_.spawn([this, packedImage, packedView, requestId, timestampNs]() {
        StillCompletion done{};
        done.requestId = requestId;
        done.timestampNs = timestampNs;
        const auto started = std::chrono::steady_clock::now();
        try {
            ensureRuntimeResources();
            done.setupMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            VkCommandBuffer cmd = runtime_.beginExternal();
            done.persistentBytes = currentAllocatedBytes();
            // Bounded submissions per demosaic pass, as in start().
            dual_->setPassBoundary([this] { runtime_.flush(); });
            ::dual::PackedCfaImageView in{packedImage,
                                          packedView,
                                          VK_FORMAT_R16G16B16A16_SFLOAT,
                                          VK_IMAGE_LAYOUT_GENERAL,
                                          width_ / 2u,
                                          height_ / 2u,
                                          width_,
                                          height_,
                                          dualPattern(cfa_)};
            ::dual::LinearRgbImage out{runtime_.outputImage(),
                                       runtime_.outputView(),
                                       VK_FORMAT_R16G16B16A16_SFLOAT,
                                       VK_IMAGE_LAYOUT_GENERAL,
                                       width_,
                                       height_};
            dual_->record(cmd, in, out);
            runtime_.submit(cmd);
            runtime_.waitForCompletion();
            std::ostringstream diag;
            diag << "DUAL_CONTRAST_RESOLVED requestId=" << requestId << " input=external_packed"
                 << " mode=" << (dualAutoContrast_ ? "auto" : "manual") << " requestedContrast=" << dualContrastPercent_
                 << " resolvedContrast=" << dual_->resolvedContrastPercent();
            emit(diag.str());
            done.success = true;
        } catch (const std::exception& e) {
            done.error = e.what();
        }
        done.submitToFenceMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        worker_.finish(std::move(done));
    });
    return true;
}

bool DualStillProcessor::startTaggedPackedReplay(std::vector<uint8_t> taggedPackedRgba16f,
                                                 const std::array<float, 4>& whiteBalanceRggb, uint32_t variant,
                                                 float anchorTolerance, float madLimit, float consensusLimit,
                                                 uint32_t minSupport, uint64_t requestId) {
    if (!configured()) return false;
    const size_t expected = static_cast<size_t>(width_ / 2u) * (height_ / 2u) * 8u;
    if (taggedPackedRgba16f.size() != expected) return false;
    if (!worker_.tryClaim()) return false;
    joinWorker();
    worker_.spawn([this, requestId, whiteBalanceRggb, variant, anchorTolerance, madLimit, consensusLimit, minSupport,
                   tagged = std::move(taggedPackedRgba16f)]() mutable {
        StillCompletion done{};
        done.requestId = requestId;
        const auto started = std::chrono::steady_clock::now();
        try {
            ensureRuntimeResources();
            VkCommandBuffer cmd = runtime_.beginTaggedPackedReplay(tagged, whiteBalanceRggb, variant, anchorTolerance,
                                                                   madLimit, consensusLimit, minSupport);
            tagged.clear();
            tagged.shrink_to_fit();
            done.persistentBytes = currentAllocatedBytes();
            {
                ::dual::PackedCfaImageView in{runtime_.packedImage(),
                                              runtime_.packedView(),
                                              VK_FORMAT_R16G16B16A16_SFLOAT,
                                              VK_IMAGE_LAYOUT_GENERAL,
                                              width_ / 2u,
                                              height_ / 2u,
                                              width_,
                                              height_,
                                              dualPattern(cfa_)};
                ::dual::LinearRgbImage out{runtime_.outputImage(),
                                           runtime_.outputView(),
                                           VK_FORMAT_R16G16B16A16_SFLOAT,
                                           VK_IMAGE_LAYOUT_GENERAL,
                                           width_,
                                           height_};
                dual_->record(cmd, in, out);
            }
            runtime_.submit(cmd);
            runtime_.waitForCompletion();
            done.success = true;
        } catch (const std::exception& e) {
            done.error = e.what();
        }
        done.submitToFenceMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        worker_.finish(std::move(done));
    });
    return true;
}

std::optional<StillCompletion> DualStillProcessor::pollCompletion() { return worker_.pollCompletion(); }
void DualStillProcessor::joinWorker() noexcept { worker_.joinWorker(); }
void DualStillProcessor::releaseWorkspaceKeepOutput() noexcept {
    joinWorker();
    dual_.reset();
    runtime_.releaseWorkspaceKeepOutput();
}
void DualStillProcessor::releaseOutput() noexcept {
    joinWorker();
    runtime_.releaseOutput();
}
void DualStillProcessor::reset() noexcept {
    worker_.resetState();
    dual_.reset();
    runtime_.reset();
    cameraContextGeneration_ = 0;
    cameraId_.clear();
    width_ = height_ = cfa_ = 0;
    dualAutoContrast_ = true;
    dualContrastPercent_ = 20.0f;
    diagnosticsEnabled_ = false;
    geometry_ = rawrcam::develop::StillDemosaicGeometry::SensorNative;
    pipelineCache_ = VK_NULL_HANDLE;
}
uint64_t DualStillProcessor::currentAllocatedBytes() const noexcept {
    return dual_ ? dual_->currentAllocatedBytes() : 0;
}
uint64_t DualStillProcessor::peakAllocatedBytes() const noexcept { return dual_ ? dual_->peakAllocatedBytes() : 0; }
void DualStillProcessor::emit(const std::string& line) const { worker_.emit(line); }

bool DualStillProcessor::dumpOutputRgba16f(const std::string& path) {
    joinWorker();
    return runtime_.dumpOutputRgba16f(path);
}
bool DualStillProcessor::dumpPackedCfaRgba16f(const std::string& path) {
    joinWorker();
    return runtime_.dumpPackedCfaRgba16f(path);
}
bool DualStillProcessor::dumpDualRcdRgba16f(const std::string& path) {
    joinWorker();
    if (!dual_ || !diagnosticsEnabled_) return false;
    return runtime_.dumpExternalImage(path, dual_->diagnosticRcdImage(), width_, height_, 8u);
}
bool DualStillProcessor::dumpDualVngRgba16f(const std::string& path) {
    joinWorker();
    if (!dual_ || !diagnosticsEnabled_) return false;
    return runtime_.dumpExternalImage(path, dual_->diagnosticVngImage(), width_, height_, 8u);
}
bool DualStillProcessor::dumpDualPreBlurMaskR32f(const std::string& path) {
    joinWorker();
    if (!dual_ || !diagnosticsEnabled_) return false;
    return runtime_.dumpExternalImage(path, dual_->diagnosticPreBlurMaskImage(), width_, height_, 4u);
}
bool DualStillProcessor::dumpDualBlendMaskR32f(const std::string& path) {
    joinWorker();
    if (!dual_) return false;
    return runtime_.dumpExternalImage(path, dual_->diagnosticBlendMaskImage(), width_, height_, 4u);
}

}  // namespace rawrcam::develop::demosaic::dual
