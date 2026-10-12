#include "capture/single/SingleFrameCaptureDevelop.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#include "develop/HotPixelConceal.h"
#include "develop/render/DenoiseProfile.h"
#include "geometry/OrientationTransform.h"

namespace rawrcam::capture {
SingleFrameCaptureDevelop::SingleFrameCaptureDevelop(const vulkan::VulkanContext& vk, std::mutex& mutex,
                                                     std::string filesDir, CaptureDiagnostic diagnostic)
    : vulkanContext_(vk),
      queueSubmitMutex_(mutex),
      filesDir_(std::move(filesDir)),
      diagnostic_(std::move(diagnostic)),
      amaze_(diagnostic_),
      rcd_(diagnostic_),
      vng_(diagnostic_),
      dual_(diagnostic_),
      renderer_(filesDir_, diagnostic_) {}
bool SingleFrameCaptureDevelop::start(const std::shared_ptr<const rawrcam::imaging::RawSnapshot>& capturedHandle,
                                      const tonemap::TonemapParams& tonemapParams, float aePostGain, bool filmEnabled,
                                      const spektrafilm_native::FilmLook& filmLook, const JpegCaptureRequest& request) {
    if (busy()) return false;
    requestId_ = capturedHandle->requestId;
    diagnostics_ = request.develop.pipelineDiagnosticsEnabled;
    demosaicMs_ = demosaicSetupMs_ = 0.0;
    const rawrcam::imaging::RawSnapshot& captured = *capturedHandle;
    try {
        if (!captured.metadata.cameraContext) {
            throw std::runtime_error("JPEG captured frame lost immutable camera context");
        }
        const auto& camera = *captured.metadata.cameraContext;
        const auto& jpegContext = request;
        const auto turns = geometry::presentationQuarterTurns(camera.geometry.sensorOrientationDegrees,
                                                              jpegContext.output.deviceRotationDegrees);
        context_ = makeSingleFrameRenderContext(captured, tonemapParams, aePostGain, turns, filmEnabled, filmLook,
                                                &request, diagnostic_);

        // Own hot-pixel conceal for the JPEG/rendered copy only. DNG bytes
        // stay untouched; the map is also recorded in DNGPrivateData.
        // Copy-on-write so the shared completedCaptureHandle used by the
        // DNG writer is never mutated; without a map the demosaic reads
        // the shared bytes directly (no copy).
        std::shared_ptr<const std::vector<uint8_t>> demosaicRaw(capturedHandle, &captured.raw16);
        if (!captured.metadata.hotPixelMap.empty() && !captured.raw16.empty()) {
            auto concealed = std::make_shared<std::vector<uint8_t>>(captured.raw16);
            const std::size_t fixed = develop::concealHotPixelsInPackedRaw16(
                *concealed, captured.width, captured.height, captured.metadata.hotPixelMap);
            emit("HOT_PIXEL_CONCEAL requestId=" + std::to_string(captured.requestId) + " fixed=" +
                 std::to_string(fixed) + " reported=" + std::to_string(captured.metadata.hotPixelMap.size() / 2));
            demosaicRaw = std::move(concealed);
        }

        // GALOSH-RAW Bayer request (P1a): resolved per shot from the
        // frozen JPEG context + frame metadata. Anything invalid forces
        // Off (legacy bit-identical); non-RGGB CFA logs + forces Off
        // until the phase-aware CFA extension lands (future work).
        ::galosh::GaloshRawParams galoshRequest{};
        {
            const int mode = jpegContext.develop.galoshRawMode;
            float black = 0.0f, white = 0.0f;
            const bool levelsOk = develop::rendered::resolveGaloshRawLevels(captured.metadata, black, white);
            if (mode >= 1 && mode <= 2 && levelsOk) {
                if (camera.rawPreviewCfa != 0u) {
                    emit("GALOSH_RAW_SKIP requestId=" + std::to_string(captured.requestId) + " reason=non_rggb_cfa");
                } else {
                    galoshRequest.mode = static_cast<::galosh::GaloshRawMode>(mode);
                    galoshRequest.strength = std::clamp(jpegContext.develop.galoshStrength, 0.0f, 8.0f);
                    galoshRequest.lumaStrength = std::clamp(jpegContext.develop.galoshLuma, 0.0f, 8.0f);
                    galoshRequest.chromaStrength = std::clamp(jpegContext.develop.galoshChroma, 0.0f, 8.0f);
                    galoshRequest.black = black;
                    galoshRequest.white = white;
                    galoshRequest.cfa = ::galosh::GaloshCfa::Rggb;
                }
            }
        }

        switch (jpegContext.develop.demosaicAlgorithm) {
            // Demosaic on the multiframe queue when present (same
            // rationale as the rendered still chain): keep queue0 for
            // preview. CPU fence-waits order the handoff.
            case develop::DemosaicAlgorithm::Amaze:
                amaze_.configure(vulkanContext_, queueSubmitMutex_, camera.cameraContextGeneration, camera.cameraId,
                                 captured.width, captured.height, camera.rawPreviewCfa,
                                 rawrcam::develop::StillDemosaicGeometry::SensorNative,
                                 vulkanContext_.hasMultiframeQueue() ? vulkanContext_.multiframeQueue() : VK_NULL_HANDLE,
                                 vulkanContext_.hasMultiframeQueue() ? &vulkanContext_.mfSubmitMutex() : nullptr);
                if (!amaze_.start(captured, demosaicRaw, jpegContext.develop.lensShadingCorrectionEnabled,
                                  jpegContext.develop.highlightReconstructionEnabled, galoshRequest))
                    throw std::runtime_error("AMaZE start rejected");
                break;
            case develop::DemosaicAlgorithm::Rcd:
                rcd_.configure(vulkanContext_, queueSubmitMutex_, camera.cameraContextGeneration, camera.cameraId,
                               captured.width, captured.height, camera.rawPreviewCfa,
                               develop::demosaic::rcd::RcdStillInputMode::SharedHighlightPacked,
                               rawrcam::develop::StillDemosaicGeometry::SensorNative,
                               vulkanContext_.hasMultiframeQueue() ? vulkanContext_.multiframeQueue() : VK_NULL_HANDLE,
                               vulkanContext_.hasMultiframeQueue() ? &vulkanContext_.mfSubmitMutex() : nullptr);
                if (!rcd_.start(captured, demosaicRaw, jpegContext.develop.lensShadingCorrectionEnabled,
                                jpegContext.develop.highlightReconstructionEnabled, galoshRequest))
                    throw std::runtime_error("RCD start rejected");
                break;
            case develop::DemosaicAlgorithm::Vng4:
                vng_.configure(vulkanContext_, queueSubmitMutex_, camera.cameraContextGeneration, camera.cameraId,
                               captured.width, captured.height, camera.rawPreviewCfa,
                               jpegContext.develop.pipelineDiagnosticsEnabled,
                               rawrcam::develop::StillDemosaicGeometry::SensorNative,
                               vulkanContext_.hasMultiframeQueue() ? vulkanContext_.multiframeQueue() : VK_NULL_HANDLE,
                               vulkanContext_.hasMultiframeQueue() ? &vulkanContext_.mfSubmitMutex() : nullptr);
                if (!vng_.start(captured, demosaicRaw, jpegContext.develop.lensShadingCorrectionEnabled,
                                jpegContext.develop.highlightReconstructionEnabled, galoshRequest))
                    throw std::runtime_error(
                        std::string(singleFrameDemosaicName(jpegContext.develop.demosaicAlgorithm)) +
                        " start rejected");
                break;
            case develop::DemosaicAlgorithm::DualRcdVng4:
            case develop::DemosaicAlgorithm::DualAmazeVng4: {
                dual_.configure(vulkanContext_, queueSubmitMutex_, camera.cameraContextGeneration, camera.cameraId,
                                captured.width, captured.height, camera.rawPreviewCfa,
                                jpegContext.develop.dualAutoContrast, jpegContext.develop.dualContrastPercent,
                                jpegContext.develop.pipelineDiagnosticsEnabled,
                                rawrcam::develop::StillDemosaicGeometry::SensorNative,
                                vulkanContext_.hasMultiframeQueue() ? vulkanContext_.multiframeQueue() : VK_NULL_HANDLE,
                                vulkanContext_.hasMultiframeQueue() ? &vulkanContext_.mfSubmitMutex() : nullptr,
                                /*externalPackedInput=*/false,
                                jpegContext.develop.demosaicAlgorithm == develop::DemosaicAlgorithm::DualAmazeVng4
                                    ? ::dual::Primary::Amaze
                                    : ::dual::Primary::Rcd);
                if (!dual_.start(captured, demosaicRaw, jpegContext.develop.lensShadingCorrectionEnabled,
                                 jpegContext.develop.highlightReconstructionEnabled, galoshRequest))
                    throw std::runtime_error(
                        std::string(singleFrameDemosaicName(jpegContext.develop.demosaicAlgorithm)) +
                        " start rejected");
                break;
            }
        }
        emit(std::string("JPEG_DEMOSAIC_PROCESS_STARTED requestId=") + std::to_string(captured.requestId) +
             " demosaic=" + singleFrameDemosaicName(jpegContext.develop.demosaicAlgorithm) + " raw=" +
             std::to_string(captured.width) + "x" + std::to_string(captured.height) + " sameRawSnapshotAsDng=true");
        return true;
    } catch (const std::exception& e) {
        emit("JPEG_DEMOSAIC_PROCESS_FAIL requestId=" + std::to_string(captured.requestId) + " error=" + e.what());
        failPendingJpeg(captured.requestId, e.what());
        amaze_.reset();
        rcd_.reset();
        vng_.reset();
        dual_.reset();
        return false;
    }
}
void SingleFrameCaptureDevelop::failPendingJpeg(uint64_t id, const std::string& error) {
    develop::rendered::RenderedStillCompletion done{};
    done.requestId = id;
    done.error = error;
    failure_ = std::move(done);
}
template <class Processor>
void SingleFrameCaptureDevelop::advanceDemosaic(Processor& processor, const char* name) {
    auto completed = processor.pollCompletion();
    if (!completed) return;
    std::ostringstream status;
    constexpr bool rcd = std::is_same_v<Processor, develop::demosaic::rcd::RcdStillProcessor>;
    constexpr bool amaze = std::is_same_v<Processor, develop::demosaic::amaze::AmazeStillProcessor>;
    status << (amaze ? (completed->success ? "AMAZE_STILL_PROCESS_PASS" : "AMAZE_STILL_PROCESS_FAIL")
               : rcd ? (completed->success ? "RCD_STILL_PROCESS_PASS" : "RCD_STILL_PROCESS_FAIL")
                     : (completed->success ? "RCDVNG_STILL_PROCESS_PASS" : "RCDVNG_STILL_PROCESS_FAIL"))
           << " requestId=" << completed->requestId << " demosaic=" << name << " timestampNs=" << completed->timestampNs
           << " totalMs=" << completed->submitToFenceMs << " persistentBytes=" << completed->persistentBytes;
    if (!completed->success) status << " error=" << completed->error;
    emit(status.str());
    if (!requestId_ || *requestId_ != completed->requestId) return;
    if (!completed->success || !context_) {
        failPendingJpeg(completed->requestId, completed->success ? "missing_rendered_context" : completed->error);
        processor.reset();
        return;
    }
    demosaicMs_ = completed->submitToFenceMs;
    demosaicSetupMs_ = completed->setupMs;
    if (diagnostics_) {
        const bool packedOk = processor.dumpPackedCfaRgba16f(filesDir_ + "/rawrcam_diag_packed_cfa_latest.rgba16f");
        const bool rgbOk = processor.dumpOutputRgba16f(filesDir_ + "/rawrcam_diag_demosaic_latest.rgba16f");
        bool branchesOk = true;
        if constexpr (std::is_same_v<Processor, develop::demosaic::dual::DualStillProcessor>) {
            const bool rcdOk = processor.dumpDualRcdRgba16f(filesDir_ + "/rawrcam_diag_dual_rcd_latest.rgba16f");
            const bool vngOk = processor.dumpDualVngRgba16f(filesDir_ + "/rawrcam_diag_dual_vng_latest.rgba16f");
            const bool preMaskOk =
                processor.dumpDualPreBlurMaskR32f(filesDir_ + "/rawrcam_diag_dual_mask_preblur_latest.r32f");
            const bool maskOk = processor.dumpDualBlendMaskR32f(filesDir_ + "/rawrcam_diag_dual_mask_latest.r32f");
            branchesOk = rcdOk && vngOk && preMaskOk && maskOk;
        }
        emit(std::string(amaze ? "DEMOSAIC_DIAG_AMAZE_DUMP " : rcd ? "DEMOSAIC_DIAG_RCD_DUMP " : "DEMOSAIC_DIAG_RCDVNG_DUMP ") +
             (packedOk && rgbOk && branchesOk ? "PASS" : "FAIL") +
             " requestId=" + std::to_string(completed->requestId) + " demosaic=" + name);
    }
    processor.releaseWorkspaceKeepOutput();
    if (!renderer_.start(vulkanContext_, queueSubmitMutex_, *context_, processor.outputImage(), processor.outputView(),
                         processor.clipStateImage(), processor.clipStateView(),
                         vulkanContext_.hasMultiframeQueue() ? vulkanContext_.multiframeQueue() : VK_NULL_HANDLE,
                         vulkanContext_.hasMultiframeQueue() ? &vulkanContext_.mfSubmitMutex() : nullptr)) {
        failPendingJpeg(completed->requestId, "rendered_tonemap_start_rejected");
        processor.reset();
    } else {
        emit("RENDERED_STILL_TONEMAP_STARTED requestId=" + std::to_string(completed->requestId) + " demosaic=" + name);
    }
}
std::optional<SingleFrameDevelopResult> SingleFrameCaptureDevelop::pollCompletion() {
    advanceDemosaic(amaze_, "AMaZE");
    advanceDemosaic(rcd_, "RCD");
    advanceDemosaic(vng_, "VNG4");
    advanceDemosaic(dual_, dual_.algorithmName());
    auto done = renderer_.pollCompletion();
    if (!done && failure_) {
        done = std::move(failure_);
        failure_.reset();
    }
    if (!done) return std::nullopt;
    std::ostringstream status;
    status << (done->success ? "RENDERED_STILL_MEMORY_PASS" : "RENDERED_STILL_MEMORY_FAIL")
           << " requestId=" << done->requestId << " raw=" << done->width << 'x' << done->height
           << " rgba8Bytes=" << done->pixelBytes << " checksum=0x" << std::hex << done->checksum << std::dec
           << " totalMs=" << done->totalMs;
    if (!done->success) status << " error=" << done->error;
    emit(status.str());
    amaze_.releaseOutput();
    amaze_.reset();
    rcd_.releaseOutput();
    rcd_.reset();
    vng_.releaseOutput();
    vng_.reset();
    dual_.releaseOutput();
    dual_.reset();
    const develop::rendered::RenderOutputView pixels{renderer_.pixelData(),         renderer_.pixelBytes(),
                                                     renderer_.outputImageView(),   renderer_.gainmapPixelData(),
                                                     renderer_.gainmapPixelBytes(), renderer_.gainmapWidth(),
                                                     renderer_.gainmapHeight()};
    return SingleFrameDevelopResult{std::move(*done), context_, pixels, demosaicMs_, demosaicSetupMs_};
}
bool SingleFrameCaptureDevelop::busy() const noexcept {
    return requestId_.has_value() || amaze_.busy() || rcd_.busy() || vng_.busy() || dual_.busy() || renderer_.busy() ||
           failure_.has_value();
}
void SingleFrameCaptureDevelop::reset() noexcept {
    renderer_.reset();
    amaze_.reset();
    rcd_.reset();
    vng_.reset();
    dual_.reset();
    requestId_.reset();
    context_.reset();
    failure_.reset();
    diagnostics_ = false;
}
void SingleFrameCaptureDevelop::shutdown() noexcept {
    renderer_.setEnginePersistenceEnabled(false);
    reset();
    renderer_.shutdownFilm();
}

}  // namespace rawrcam::capture
