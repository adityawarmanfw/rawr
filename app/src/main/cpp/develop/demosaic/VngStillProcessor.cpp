#include "develop/demosaic/VngStillProcessor.h"

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

namespace rawrcam::develop::demosaic::vng4 {

VngStillProcessor::VngStillProcessor(Diagnostic diagnostic) : worker_(diagnostic), runtime_("RCDVNG", diagnostic) {}
VngStillProcessor::~VngStillProcessor() { reset(); }

const char* VngStillProcessor::algorithmName() noexcept { return "VNG4"; }
::vng4::BayerPattern VngStillProcessor::vngPattern(uint32_t cfa) {
    return rawrcam::geometry::toBackendPattern<::vng4::BayerPattern>(cfa, "unsupported RawrCam CFA for VNG4");
}
void VngStillProcessor::configure(const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex,
                                  uint64_t generation, const std::string& cameraId, uint32_t width, uint32_t height,
                                  uint32_t cfa, bool diagnosticsEnabled,
                                  rawrcam::develop::StillDemosaicGeometry geometry, VkQueue queueOverride,
                                  std::mutex* queueSubmitMutexOverride) {
    if (!context.physicalDevice() || !context.device() || !context.queue())
        throw std::invalid_argument("RCD/VNG requires initialized Vulkan context");
    const bool reconstructedGeometry = geometry == rawrcam::develop::StillDemosaicGeometry::ReconstructedCfa;
    const bool geometrySupported = reconstructedGeometry
                                       ? width >= 20u && height >= 20u && (width & 1u) == 0u && (height & 1u) == 0u
                                       : rawrcam::geometry::isSupportedRawGeometry(width, height);
    if (!geometrySupported) throw std::invalid_argument("RCD/VNG geometry is not a supported RAW geometry");
    if (!generation || cameraId.empty()) throw std::invalid_argument("RCD/VNG requires immutable camera identity");
    (void)vngPattern(cfa);
    if (configured()) {
        if (cameraContextGeneration_ != generation || cameraId_ != cameraId || width_ != width || height_ != height ||
            cfa_ != cfa || diagnosticsEnabled_ != diagnosticsEnabled || geometry_ != geometry ||
            runtime_.device() != context.device())
            throw std::logic_error("RCD/VNG reconfiguration requires reset after GPU completion");
        runtime_.configure(context, queueSubmitMutex, width, height, cfa, true, queueOverride,
                           queueSubmitMutexOverride);
        return;
    }
    runtime_.configure(context, queueSubmitMutex, width, height, cfa, true, queueOverride, queueSubmitMutexOverride);
    cameraContextGeneration_ = generation;
    cameraId_ = cameraId;
    width_ = width;
    height_ = height;
    cfa_ = cfa;
    diagnosticsEnabled_ = diagnosticsEnabled;
    geometry_ = geometry;
    std::ostringstream out;
    out << "RCDVNG_STILL_CONFIG_ACCEPTED algorithm=" << algorithmName() << " cameraId=" << cameraId_
        << " generation=" << generation << " raw=" << width_ << 'x' << height_
        << " inputMode=shared_highlight_packed singleFlight=true async=true lazy=true"
        << " geometry=" << (reconstructedGeometry ? "reconstructed_cfa" : "sensor_native")
        << " diagnostics=" << (diagnosticsEnabled_ ? "on" : "off");
    emit(out.str());
}

void VngStillProcessor::ensureRuntimeResources() {
    if (vng4_) return;
    runtime_.ensureResources();
    {
        ::vng4::PipelineConfig cfg{};
        cfg.width = width_;
        cfg.height = height_;
        cfg.pattern = vngPattern(cfa_);
        cfg.inputMode = ::vng4::InputMode::PackedCfaRgba16fImage;
        cfg.outputScale = 1.0f / 255.0f;
        cfg.outputAlpha = 1.0f;
        cfg.telemetry = false;
        ::vng4::VulkanContext vc{runtime_.physicalDevice(), runtime_.device(), runtime_.queueFamily(), nullptr};
        vng4_ = std::make_unique<::vng4::Vng4Pipeline>(vc, raw_demosaic::embeddedVng4Shaders(), cfg);
    }
    emit(std::string("RCDVNG_STILL_INIT_PIPELINE_PASS algorithm=") + algorithmName() +
         " persistentBytes=" + std::to_string(currentAllocatedBytes()));
}

bool VngStillProcessor::busy() const noexcept { return worker_.busy(); }

bool VngStillProcessor::start(const rawrcam::imaging::RawSnapshot& frame,
                              std::shared_ptr<const std::vector<uint8_t>> raw, bool lensShadingCorrectionEnabled,
                              bool highlightReconstructionEnabled) {
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
    worker_.spawn([this, requestId, timestampNs, black, white, wb, lensShading, highlightReconstructionEnabled,
                   raw = std::move(raw)]() mutable {
        StillCompletion done{};
        done.requestId = requestId;
        done.timestampNs = timestampNs;
        const auto started = std::chrono::steady_clock::now();
        try {
            ensureRuntimeResources();
            VkCommandBuffer cmd =
                runtime_.beginFrame(*raw, black, white, wb, lensShading, highlightReconstructionEnabled);
            raw.reset();
            done.persistentBytes = currentAllocatedBytes();
            // Bounded submissions (upload/highlight, then each demosaic pass) so the
            // preview queue is not starved for the whole still.
            runtime_.flush();
            vng4_->setPassBoundary([this] { runtime_.flush(); });
            {
                ::vng4::PackedCfaImageView in{runtime_.packedImage(),
                                              runtime_.packedView(),
                                              VK_FORMAT_R16G16B16A16_SFLOAT,
                                              VK_IMAGE_LAYOUT_GENERAL,
                                              width_ / 2u,
                                              height_ / 2u,
                                              width_,
                                              height_,
                                              vngPattern(cfa_)};
                ::vng4::LinearRgbImage out{runtime_.outputImage(),
                                           runtime_.outputView(),
                                           VK_FORMAT_R16G16B16A16_SFLOAT,
                                           VK_IMAGE_LAYOUT_GENERAL,
                                           width_,
                                           height_};
                vng4_->record(cmd, in, out);
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

std::optional<StillCompletion> VngStillProcessor::pollCompletion() { return worker_.pollCompletion(); }
void VngStillProcessor::joinWorker() noexcept { worker_.joinWorker(); }
void VngStillProcessor::releaseWorkspaceKeepOutput() noexcept {
    joinWorker();
    vng4_.reset();
    runtime_.releaseWorkspaceKeepOutput();
}
void VngStillProcessor::releaseOutput() noexcept {
    joinWorker();
    runtime_.releaseOutput();
}
void VngStillProcessor::reset() noexcept {
    worker_.resetState();
    vng4_.reset();
    runtime_.reset();
    cameraContextGeneration_ = 0;
    cameraId_.clear();
    width_ = height_ = cfa_ = 0;
    diagnosticsEnabled_ = false;
    geometry_ = rawrcam::develop::StillDemosaicGeometry::SensorNative;
}
uint64_t VngStillProcessor::currentAllocatedBytes() const noexcept {
    return vng4_ ? vng4_->currentAllocatedBytes() : 0;
}
uint64_t VngStillProcessor::peakAllocatedBytes() const noexcept { return vng4_ ? vng4_->peakAllocatedBytes() : 0; }
void VngStillProcessor::emit(const std::string& line) const { worker_.emit(line); }

bool VngStillProcessor::dumpOutputRgba16f(const std::string& path) {
    joinWorker();
    return runtime_.dumpOutputRgba16f(path);
}
bool VngStillProcessor::dumpPackedCfaRgba16f(const std::string& path) {
    joinWorker();
    return runtime_.dumpPackedCfaRgba16f(path);
}

}  // namespace rawrcam::develop::demosaic::vng4
