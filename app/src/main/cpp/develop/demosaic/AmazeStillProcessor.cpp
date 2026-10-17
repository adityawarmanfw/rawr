#include "develop/demosaic/AmazeStillProcessor.h"

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

namespace rawrcam::develop::demosaic::amaze {

AmazeStillProcessor::AmazeStillProcessor(Diagnostic diagnostic) : worker_(diagnostic), runtime_("AMAZE", diagnostic) {}

AmazeStillProcessor::~AmazeStillProcessor() { reset(); }

std::array<float, 3> AmazeStillProcessor::amazeWhiteBalance(const std::array<float, 4>& wb) noexcept {
    const float green = 0.5f * (wb[1] + wb[2]);
    return {wb[0] / green, 1.0f, wb[3] / green};
}

void AmazeStillProcessor::configure(const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex,
                                    uint64_t generation, const std::string& cameraId, uint32_t width, uint32_t height,
                                    uint32_t cfa, rawrcam::develop::StillDemosaicGeometry geometry,
                                    VkQueue queueOverride, std::mutex* queueSubmitMutexOverride,
                                    bool externalPackedInput) {
    if (!context.physicalDevice() || !context.device() || !context.queue()) {
        throw std::invalid_argument("AMaZE requires an initialized RawrCam Vulkan context");
    }
    const bool reconstructedGeometry = geometry == rawrcam::develop::StillDemosaicGeometry::ReconstructedCfa;
    const bool geometrySupported = reconstructedGeometry
                                       ? width >= 32u && height >= 32u && (width & 1u) == 0u && (height & 1u) == 0u
                                       : rawrcam::geometry::isSupportedRawGeometry(width, height);
    if (!geometrySupported) {
        throw std::invalid_argument("AMaZE geometry is not a supported RAW geometry");
    }
    if (!generation || cameraId.empty()) {
        throw std::invalid_argument("AMaZE requires active immutable camera context identity");
    }

    const auto pattern =
        rawrcam::geometry::toBackendPattern<::amaze::BayerPattern>(cfa, "unsupported RawrCam CFA for AMaZE");
    if (configured()) {
        if (cameraContextGeneration_ != generation || cameraId_ != cameraId || width_ != width || height_ != height ||
            pattern_ != pattern || geometry_ != geometry || runtime_.device() != context.device()) {
            throw std::logic_error("AMaZE reconfiguration requires reset after GPU completion");
        }
        runtime_.configure(context, queueSubmitMutex, width, height, cfa, true, queueOverride,
                           queueSubmitMutexOverride, externalPackedInput);
        return;
    }

    runtime_.configure(context, queueSubmitMutex, width, height, cfa, true, queueOverride, queueSubmitMutexOverride,
                       externalPackedInput);
    cameraContextGeneration_ = generation;
    cameraId_ = cameraId;
    width_ = width;
    height_ = height;
    pattern_ = pattern;
    geometry_ = geometry;

    std::ostringstream out;
    out << "AMAZE_STILL_CONFIG_ACCEPTED cameraId=" << cameraId_ << " generation=" << generation << " raw=" << width_
        << 'x' << height_ << " cfa=" << cfa << " inputMode=" << (externalPackedInput ? "external_packed" : "shared_highlight_packed")
        << " geometry=" << (reconstructedGeometry ? "reconstructed_cfa" : "sensor_native")
        << " heavyInit=worker singleFlight=true async=true lazy=true";
    emit(out.str());
}

void AmazeStillProcessor::ensureRuntimeResources() {
    if (pipeline_) return;

    emit("AMAZE_STILL_INIT_BEGIN raw=" + std::to_string(width_) + "x" + std::to_string(height_));
    runtime_.ensureResources();

    ::amaze::PipelineConfig cfg{};
    cfg.width = width_;
    cfg.height = height_;
    cfg.pattern = pattern_;
    cfg.inputMode = ::amaze::InputMode::PackedCfaRgba16fImage;

    const char* reason = nullptr;
    if (!::amaze::AmazePipeline::validateConfig(cfg, &reason)) {
        runtime_.discardWorkspace();
        throw std::runtime_error(std::string("AMaZE configuration rejected: ") + (reason ? reason : "unknown"));
    }

    ::amaze::VulkanContext algorithmContext{};
    algorithmContext.physicalDevice = runtime_.physicalDevice();
    algorithmContext.device = runtime_.device();
    algorithmContext.queueFamilyIndex = runtime_.queueFamily();
    try {
        pipeline_ = std::make_unique<::amaze::AmazePipeline>(algorithmContext, raw_demosaic::embeddedAmazeShaders(), cfg);
    } catch (...) {
        runtime_.discardWorkspace();
        throw;
    }
    emit("AMAZE_STILL_INIT_PIPELINE_PASS persistentBytes=" + std::to_string(currentAllocatedBytes()));
}

bool AmazeStillProcessor::busy() const noexcept { return worker_.busy(); }

bool AmazeStillProcessor::start(const rawrcam::imaging::RawSnapshot& frame,
                                std::shared_ptr<const std::vector<uint8_t>> raw, bool lensShadingCorrectionEnabled,
                                bool highlightReconstructionEnabled) {
    if (!configured()) return false;

    const uint64_t expectedBytes = static_cast<uint64_t>(width_) * height_ * sizeof(uint16_t);
    if (frame.width != width_ || frame.height != height_ || expectedBytes > std::numeric_limits<size_t>::max() ||
        !raw || raw->size() != static_cast<size_t>(expectedBytes)) {
        emit("AMAZE_STILL_REQUEST_REJECTED reason=geometry_or_payload_mismatch");
        return false;
    }
    if (!frame.metadata.cameraContext || !(frame.metadata.effectiveWhiteLevel > 0.0f)) {
        emit("AMAZE_STILL_REQUEST_REJECTED reason=missing_processing_metadata");
        return false;
    }
    if (frame.metadata.cameraContext->cameraContextGeneration != cameraContextGeneration_ ||
        frame.metadata.cameraContext->cameraId != cameraId_) {
        emit("AMAZE_STILL_REQUEST_REJECTED reason=camera_context_mismatch");
        return false;
    }
    const auto whiteBalanceRggb = frame.colorState.baselineWbRggb;
    for (float g : whiteBalanceRggb) {
        if (!(g > 0.0f)) {
            emit("AMAZE_STILL_REQUEST_REJECTED reason=invalid_white_balance");
            return false;
        }
    }

    if (!worker_.tryClaim()) {
        emit("AMAZE_STILL_REQUEST_REJECTED reason=single_flight_busy");
        return false;
    }
    joinWorker();

    const uint64_t requestId = frame.requestId;
    const uint64_t timestampNs = frame.timestampNs;
    const auto blackPhysical = frame.metadata.blackLevelPhysicalRggb;
    const float whiteLevel = frame.metadata.effectiveWhiteLevel;
    const auto lensShading =
        rawrcam::develop::highlight::LensShadingMapSnapshot::fromMetadata(frame.metadata, lensShadingCorrectionEnabled);

    worker_.spawn([this, requestId, timestampNs, blackPhysical, whiteLevel, whiteBalanceRggb, lensShading,
                   highlightReconstructionEnabled, raw = std::move(raw)]() mutable {
        StillCompletion done{};
        done.requestId = requestId;
        done.timestampNs = timestampNs;
        const auto started = std::chrono::steady_clock::now();
        try {
            ensureRuntimeResources();
            done.setupMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            VkCommandBuffer command = runtime_.beginFrame(*raw, blackPhysical, whiteLevel, whiteBalanceRggb,
                                                          lensShading, highlightReconstructionEnabled);
            raw.reset();
            done.persistentBytes = currentAllocatedBytes();
            // Bounded submissions (upload/packing, then each tile batch) so the
            // preview queue is not starved for the whole still; the tile batches
            // are double-buffered so the GPU never idles on a CPU round trip.
            runtime_.flush();
            pipeline_->setCommandBoundary([this] { return runtime_.flushPipelined(); });
            emit("AMAZE_STILL_UPLOAD_READY requestId=" + std::to_string(requestId));

            ::amaze::LinearRgbImage output{};
            output.image = runtime_.outputImage();
            output.view = runtime_.outputView();
            output.format = runtime_.outputFormat();
            output.layout = VK_IMAGE_LAYOUT_GENERAL;
            output.width = width_;
            output.height = height_;

            ::amaze::PackedCfaImageView input{};
            input.image = runtime_.packedImage();
            input.view = runtime_.packedView();
            input.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            input.layout = VK_IMAGE_LAYOUT_GENERAL;
            input.width = width_ / 2u;
            input.height = height_ / 2u;
            input.rawWidth = width_;
            input.rawHeight = height_;
            input.pattern = pattern_;
            pipeline_->record(command, input, output, amazeWhiteBalance(whiteBalanceRggb));

            runtime_.submit(runtime_.currentCommand());
            emit("AMAZE_STILL_GPU_SUBMITTED requestId=" + std::to_string(requestId));
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

bool AmazeStillProcessor::startExternalPacked(VkImage packedImage, VkImageView packedView,
                                              const std::array<float, 4>& whiteBalanceRggb, uint64_t requestId,
                                              uint64_t timestampNs) {
    if (!configured() || packedImage == VK_NULL_HANDLE || packedView == VK_NULL_HANDLE) return false;
    for (float g : whiteBalanceRggb) {
        if (!(g > 0.0f)) {
            emit("AMAZE_STILL_REQUEST_REJECTED reason=invalid_white_balance");
            return false;
        }
    }
    if (!worker_.tryClaim()) return false;
    joinWorker();
    worker_.spawn([this, packedImage, packedView, whiteBalanceRggb, requestId, timestampNs]() {
        StillCompletion done{};
        done.requestId = requestId;
        done.timestampNs = timestampNs;
        const auto started = std::chrono::steady_clock::now();
        try {
            ensureRuntimeResources();
            done.setupMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            VkCommandBuffer command = runtime_.beginExternal();
            done.persistentBytes = currentAllocatedBytes();
            // Bounded tile-batch submissions, double-buffered so the GPU never
            // idles on a CPU round trip between them.
            pipeline_->setCommandBoundary([this] { return runtime_.flushPipelined(); });

            ::amaze::LinearRgbImage output{};
            output.image = runtime_.outputImage();
            output.view = runtime_.outputView();
            output.format = runtime_.outputFormat();
            output.layout = VK_IMAGE_LAYOUT_GENERAL;
            output.width = width_;
            output.height = height_;

            ::amaze::PackedCfaImageView input{};
            input.image = packedImage;
            input.view = packedView;
            input.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            input.layout = VK_IMAGE_LAYOUT_GENERAL;
            input.width = width_ / 2u;
            input.height = height_ / 2u;
            input.rawWidth = width_;
            input.rawHeight = height_;
            input.pattern = pattern_;
            pipeline_->record(command, input, output, amazeWhiteBalance(whiteBalanceRggb));

            runtime_.submit(runtime_.currentCommand());
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

std::optional<StillCompletion> AmazeStillProcessor::pollCompletion() { return worker_.pollCompletion(); }

void AmazeStillProcessor::joinWorker() noexcept { worker_.joinWorker(); }

bool AmazeStillProcessor::dumpOutputRgba16f(const std::string& path) {
    joinWorker();
    return runtime_.dumpOutputRgba16f(path);
}

void AmazeStillProcessor::releaseWorkspaceKeepOutput() noexcept {
    joinWorker();
    pipeline_.reset();
    runtime_.releaseWorkspaceKeepOutput();
}

void AmazeStillProcessor::releaseOutput() noexcept {
    joinWorker();
    runtime_.releaseOutput();
}

void AmazeStillProcessor::reset() noexcept {
    worker_.resetState();
    pipeline_.reset();
    runtime_.reset();
    cameraContextGeneration_ = 0;
    cameraId_.clear();
    width_ = 0;
    height_ = 0;
    pattern_ = ::amaze::BayerPattern::RGGB;
    geometry_ = rawrcam::develop::StillDemosaicGeometry::SensorNative;
}

uint64_t AmazeStillProcessor::currentAllocatedBytes() const noexcept {
    return pipeline_ ? pipeline_->currentAllocatedBytes() : 0;
}

uint64_t AmazeStillProcessor::peakAllocatedBytes() const noexcept {
    return pipeline_ ? pipeline_->peakAllocatedBytes() : 0;
}

void AmazeStillProcessor::emit(const std::string& line) const { worker_.emit(line); }

bool AmazeStillProcessor::dumpPackedCfaRgba16f(const std::string& path) {
    joinWorker();
    return runtime_.dumpPackedCfaRgba16f(path);
}

}  // namespace rawrcam::develop::demosaic::amaze
