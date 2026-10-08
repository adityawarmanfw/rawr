#include "develop/demosaic/RcdStillProcessor.h"

#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "develop/highlight/LensShadingMapSnapshot.h"
#include "geometry/CfaPattern.h"
#include "geometry/RawGeometry.h"
#include "raw_demosaic/EmbeddedShaders.hpp"

namespace rawrcam::develop::demosaic::rcd {

RcdStillProcessor::RcdStillProcessor(Diagnostic diagnostic) : worker_(diagnostic), runtime_("RCD", diagnostic) {}

RcdStillProcessor::~RcdStillProcessor() { reset(); }

::rcd::BayerPattern RcdStillProcessor::mapCfa(uint32_t cfa) {
    return rawrcam::geometry::toBackendPattern<::rcd::BayerPattern>(cfa, "unsupported RawrCam CFA for RCD");
}

std::array<float, 4> RcdStillProcessor::parityBlackLevels(::rcd::BayerPattern pattern,
                                                          const std::array<float, 4>& p) noexcept {
    return rawrcam::geometry::reorderRggbByCode(p, static_cast<std::uint32_t>(pattern));
}

void RcdStillProcessor::configure(const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex,
                                  uint64_t generation, const std::string& cameraId, uint32_t width, uint32_t height,
                                  uint32_t cfa, RcdStillInputMode inputMode,
                                  rawrcam::develop::StillDemosaicGeometry geometry, VkQueue queueOverride,
                                  std::mutex* queueSubmitMutexOverride) {
    if (!context.physicalDevice() || !context.device() || !context.queue()) {
        throw std::invalid_argument("RCD requires an initialized RawrCam Vulkan context");
    }
    const bool reconstructedGeometry = geometry == rawrcam::develop::StillDemosaicGeometry::ReconstructedCfa;
    const bool geometrySupported = reconstructedGeometry
                                       ? width >= 20u && height >= 20u && (width & 1u) == 0u && (height & 1u) == 0u
                                       : rawrcam::geometry::isSupportedRawGeometry(width, height);
    if (!geometrySupported) {
        throw std::invalid_argument("RCD geometry is not a supported RAW geometry");
    }
    if (!generation || cameraId.empty()) {
        throw std::invalid_argument("RCD requires active immutable camera context identity");
    }

    const auto pattern = mapCfa(cfa);
    const bool sharedHighlight = inputMode == RcdStillInputMode::SharedHighlightPacked;
    if (configured()) {
        if (cameraContextGeneration_ != generation || cameraId_ != cameraId || width_ != width || height_ != height ||
            pattern_ != pattern || inputMode_ != inputMode || geometry_ != geometry ||
            runtime_.device() != context.device()) {
            throw std::logic_error("RCD reconfiguration requires reset after GPU completion");
        }
        runtime_.configure(context, queueSubmitMutex, width, height, cfa, sharedHighlight, queueOverride,
                           queueSubmitMutexOverride);
        return;
    }

    runtime_.configure(context, queueSubmitMutex, width, height, cfa, sharedHighlight, queueOverride,
                       queueSubmitMutexOverride);
    cameraContextGeneration_ = generation;
    cameraId_ = cameraId;
    width_ = width;
    height_ = height;
    pattern_ = pattern;
    inputMode_ = inputMode;
    geometry_ = geometry;

    std::ostringstream out;
    out << "RCD_STILL_CONFIG_ACCEPTED version=" << "0.8.9"
        << " cameraId=" << cameraId_ << " generation=" << generation << " raw=" << width_ << 'x' << height_
        << " cfa=" << cfa << " inputMode=" << (sharedHighlight ? "shared_highlight_packed" : "raw_legacy")
        << " geometry=" << (reconstructedGeometry ? "reconstructed_cfa" : "sensor_native")
        << " heavyInit=worker singleFlight=true async=true lazy=true";
    emit(out.str());
}

void RcdStillProcessor::ensureRuntimeResources() {
    if (pipeline_) return;

    emit("RCD_STILL_INIT_BEGIN raw=" + std::to_string(width_) + "x" + std::to_string(height_));
    runtime_.ensureResources();

    ::rcd::PipelineAssets assets{};
    ::rcd::PipelineConfig cfg{};
    cfg.autoBalance = true;
    cfg.width = width_;
    cfg.height = height_;
    cfg.pattern = pattern_;
    cfg.inputMode = inputMode_ == RcdStillInputMode::SharedHighlightPacked ? ::rcd::InputMode::PackedCfaRgba16fImage
                                                                           : ::rcd::InputMode::RawR16UintImage;
    cfg.outputScale = 1.0f / 255.0f;
    cfg.outputAlpha = 1.0f;
    cfg.telemetry = false;

    const char* reason = nullptr;
    if (!::rcd::RcdPipeline::validateConfig(cfg, assets, &reason)) {
        runtime_.discardWorkspace();
        throw std::runtime_error(std::string("RCD configuration rejected: ") + (reason ? reason : "unknown"));
    }

    ::rcd::VulkanContext algorithmContext{};
    algorithmContext.physicalDevice = runtime_.physicalDevice();
    algorithmContext.device = runtime_.device();
    algorithmContext.queueFamilyIndex = runtime_.queueFamily();
    try {
        pipeline_ =
            std::make_unique<::rcd::RcdPipeline>(algorithmContext, raw_demosaic::embeddedRcdShaders(), cfg, assets);
    } catch (...) {
        runtime_.discardWorkspace();
        throw;
    }
    emit("RCD_STILL_INIT_PIPELINE_PASS persistentBytes=" + std::to_string(currentAllocatedBytes()));
}

bool RcdStillProcessor::busy() const noexcept { return worker_.busy(); }

bool RcdStillProcessor::start(const rawrcam::imaging::RawSnapshot& frame,
                              std::shared_ptr<const std::vector<uint8_t>> raw, bool lensShadingCorrectionEnabled,
                              bool highlightReconstructionEnabled, const ::galosh::GaloshRawParams& galosh) {
    if (!configured()) return false;

    const uint64_t expectedBytes = static_cast<uint64_t>(width_) * height_ * sizeof(uint16_t);
    if (frame.width != width_ || frame.height != height_ || expectedBytes > std::numeric_limits<size_t>::max() ||
        !raw || raw->size() != static_cast<size_t>(expectedBytes)) {
        emit("RCD_STILL_REQUEST_REJECTED reason=geometry_or_payload_mismatch");
        return false;
    }
    if (!frame.metadata.cameraContext || !(frame.metadata.effectiveWhiteLevel > 0.0f)) {
        emit("RCD_STILL_REQUEST_REJECTED reason=missing_processing_metadata");
        return false;
    }
    if (frame.metadata.cameraContext->cameraContextGeneration != cameraContextGeneration_ ||
        frame.metadata.cameraContext->cameraId != cameraId_) {
        emit("RCD_STILL_REQUEST_REJECTED reason=camera_context_mismatch");
        return false;
    }

    if (!worker_.tryClaim()) {
        emit("RCD_STILL_REQUEST_REJECTED reason=single_flight_busy");
        return false;
    }
    joinWorker();

    const uint64_t requestId = frame.requestId;
    const uint64_t timestampNs = frame.timestampNs;
    const auto blackPhysical = frame.metadata.blackLevelPhysicalRggb;
    const float whiteLevel = frame.metadata.effectiveWhiteLevel;
    const auto whiteBalanceRggb = frame.colorState.baselineWbRggb;
    const auto parityBlack = parityBlackLevels(pattern_, blackPhysical);
    const auto lensShading =
        rawrcam::develop::highlight::LensShadingMapSnapshot::fromMetadata(frame.metadata, lensShadingCorrectionEnabled);

    worker_.spawn([this, requestId, timestampNs, blackPhysical, whiteLevel, whiteBalanceRggb, parityBlack, lensShading,
                   highlightReconstructionEnabled, galosh, raw = std::move(raw)]() mutable {
        StillCompletion done{};
        done.requestId = requestId;
        done.timestampNs = timestampNs;
        const auto started = std::chrono::steady_clock::now();
        try {
            ensureRuntimeResources();
            VkCommandBuffer command = runtime_.beginFrame(*raw, blackPhysical, whiteLevel, whiteBalanceRggb,
                                                          lensShading, highlightReconstructionEnabled, galosh);
            raw.reset();
            done.persistentBytes = currentAllocatedBytes();
            // Bounded submissions (upload/highlight, then each demosaic pass) so the
            // preview queue is not starved for the whole still.
            runtime_.flush();
            pipeline_->setPassBoundary([this] { runtime_.flush(); });
            emit("RCD_STILL_UPLOAD_READY requestId=" + std::to_string(requestId));

            ::rcd::LinearRgbImage output{};
            output.image = runtime_.outputImage();
            output.view = runtime_.outputView();
            output.format = runtime_.outputFormat();
            output.layout = VK_IMAGE_LAYOUT_GENERAL;
            output.width = width_;
            output.height = height_;

            if (inputMode_ == RcdStillInputMode::SharedHighlightPacked) {
                ::rcd::PackedCfaImageView input{};
                input.image = runtime_.packedImage();
                input.view = runtime_.packedView();
                input.format = VK_FORMAT_R16G16B16A16_SFLOAT;
                input.layout = VK_IMAGE_LAYOUT_GENERAL;
                input.width = width_ / 2u;
                input.height = height_ / 2u;
                input.rawWidth = width_;
                input.rawHeight = height_;
                input.pattern = pattern_;
                pipeline_->record(command, input, output);
            } else {
                ::rcd::RawCfaImageView input{};
                input.image = runtime_.inputImage();
                input.view = runtime_.inputView();
                input.format = VK_FORMAT_R16_UINT;
                input.layout = VK_IMAGE_LAYOUT_GENERAL;
                input.width = width_;
                input.height = height_;
                input.pattern = pattern_;
                for (size_t i = 0; i < parityBlack.size(); ++i) {
                    input.blackLevel[i] = parityBlack[i];
                }
                input.whiteLevel = whiteLevel;
                pipeline_->record(command, input, output);
            }

            runtime_.submit(command);
            emit("RCD_STILL_GPU_SUBMITTED requestId=" + std::to_string(requestId));
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

std::optional<StillCompletion> RcdStillProcessor::pollCompletion() { return worker_.pollCompletion(); }

void RcdStillProcessor::joinWorker() noexcept { worker_.joinWorker(); }

bool RcdStillProcessor::dumpOutputRgba16f(const std::string& path) {
    joinWorker();
    return runtime_.dumpOutputRgba16f(path);
}

void RcdStillProcessor::releaseWorkspaceKeepOutput() noexcept {
    joinWorker();
    pipeline_.reset();
    runtime_.releaseWorkspaceKeepOutput();
}

void RcdStillProcessor::releaseOutput() noexcept {
    joinWorker();
    runtime_.releaseOutput();
}

void RcdStillProcessor::reset() noexcept {
    worker_.resetState();
    pipeline_.reset();
    runtime_.reset();
    cameraContextGeneration_ = 0;
    cameraId_.clear();
    width_ = 0;
    height_ = 0;
    pattern_ = ::rcd::BayerPattern::RGGB;
    inputMode_ = RcdStillInputMode::RawLegacy;
    geometry_ = rawrcam::develop::StillDemosaicGeometry::SensorNative;
}

uint64_t RcdStillProcessor::currentAllocatedBytes() const noexcept {
    return pipeline_ ? pipeline_->currentAllocatedBytes() : 0;
}

uint64_t RcdStillProcessor::peakAllocatedBytes() const noexcept {
    return pipeline_ ? pipeline_->peakAllocatedBytes() : 0;
}

void RcdStillProcessor::emit(const std::string& line) const { worker_.emit(line); }

bool RcdStillProcessor::dumpPackedCfaRgba16f(const std::string& path) {
    joinWorker();
    return runtime_.dumpPackedCfaRgba16f(path);
}

}  // namespace rawrcam::develop::demosaic::rcd
