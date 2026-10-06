#include "capture/single/SingleFrameCaptureOutputs.h"

#include <android/log.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "capture/persistence/FrameRecipe.h"
#include "develop/render/DenoiseProfile.h"
#include "encoding/jpeg/JpegPublicDescription.h"

namespace rawrcam::capture {
SingleFrameCaptureOutputs::~SingleFrameCaptureOutputs() {
    cancelDng();
    resetJpeg();
}
bool SingleFrameCaptureOutputs::validate(const encoding::dng::DngCaptureContext& dng, const JpegCaptureRequest& jpeg) {
    return (dng.outputFd >= 0 || jpeg.output.outputFd >= 0) &&
           (jpeg.output.outputFd < 0 || (jpeg.output.quality >= 95 && jpeg.output.quality <= 100));
}
void SingleFrameCaptureOutputs::accept(encoding::dng::DngCaptureContext dng, JpegCaptureRequest jpeg,
                                       bool jpegRequested) {
    dngFd_.reset(dng.outputFd);
    dng.outputFd = -1;
    jpegFd_.reset(jpeg.output.outputFd);
    jpeg.output.outputFd = -1;
    dng_ = std::move(dng);
    if (jpegRequested) jpeg_ = std::move(jpeg);
}
void SingleFrameCaptureOutputs::prepare(const imaging::RawSnapshot& captured, const tonemap::TonemapParams& tone,
                                        float gain, bool filmEnabled) {
    std::string description = "Captured with Rawr";
    if (jpeg_) {
        if (captured.metadata.cameraContext) applySingleFrameCaptureMetadata(jpeg_->output, captured);
        auto frozen = tone;
        frozen.aePostGain = gain;
        float a = 0, b = 0;
        const auto& settings = jpeg_->develop;
        const float strength =
            settings.denoiseStrength > 0 && develop::rendered::resolveDenoiseNoise(captured.metadata, a, b)
                ? settings.denoiseStrength
                : 0.0f;
        description = encoding::jpeg::buildPublicDescription(
            frozen, jpeg_->output.rendererDisplayName, filmEnabled, jpeg_->output.filmDescription, strength,
            settings.denoiseDetail, a, b, settings.galoshYuvMode, settings.galoshYuvStrengthY,
            settings.galoshYuvStrengthC, settings.galoshRawMode, settings.galoshStrength, settings.galoshLuma,
            settings.galoshChroma);
        jpeg_->output.imageDescription = description;
    }
    if (dng_) {
        dng_->imageDescription = description;
        dng_->baselineExposureEV = static_cast<float>(std::log2(std::max(1.0e-6f, gain)));
    }
}
bool SingleFrameCaptureOutputs::startDng(std::shared_ptr<const imaging::RawSnapshot> frame, const std::string& recipe) {
    if (!dng_) return false;
    if (dngFd_.get() < 0) {
        dng_.reset();
        return false;
    }
    auto context = std::move(*dng_);
    dng_.reset();
    context.resolvedRecipe = recipe;
    persistence::attachFrameRecipes(context, *frame);
    const std::string name = context.displayName;
    context.outputFd = dngFd_.release();
    if (dngWriter_.start(frame, std::move(context))) {
        emit("DNG_CAPTURE_WRITE_STARTED requestId=" + std::to_string(frame->requestId) +
             " timestampNs=" + std::to_string(frame->timestampNs));
        return true;
    }
    encoding::dng::DngWriteCompletion failed{};
    failed.requestId = frame->requestId;
    failed.displayName = name;
    failed.error = "dng_start_rejected";
    dngCompletion_ = std::move(failed);
    return false;
}
bool SingleFrameCaptureOutputs::startJpeg(const SingleFrameDevelopResult& result) {
    const auto& completed = result.completion;
    if (!completed.success || !jpeg_) {
        failJpeg(completed.requestId, completed.success ? "missing_jpeg_context" : completed.error);
        return false;
    }
    // Film may fall back to tonemap after the description was frozen
    // with film intent (RAM gate, record fault): refresh the block to
    // match the actual pixels. No-op otherwise (same inputs → byte
    // identical), so normal captures are untouched.
    if (result.context && result.context->filmEnabled && !jpeg_->output.filmDescription.empty() &&
        !completed.filmRendered) {
        jpeg_->output.imageDescription = encoding::jpeg::buildPublicDescription(
            result.context->tonemapParams, jpeg_->output.rendererDisplayName,
            /*filmRendered=*/false, jpeg_->output.filmDescription, result.context->denoiseStrength,
            result.context->denoiseDetail, result.context->denoiseNoiseA, result.context->denoiseNoiseB,
            jpeg_->develop.galoshYuvMode, jpeg_->develop.galoshYuvStrengthY, jpeg_->develop.galoshYuvStrengthC,
            jpeg_->develop.galoshRawMode, jpeg_->develop.galoshStrength, jpeg_->develop.galoshLuma,
            jpeg_->develop.galoshChroma);
        emit("JPEG_FILM_FALLBACK_DESCRIPTION requestId=" + std::to_string(completed.requestId));
    }

    if (jpeg_->develop.pipelineDiagnosticsEnabled) {
        dumpPreJpegPpm(completed.requestId, completed.width, completed.height, result.pixels.pixels,
                       result.pixels.bytes);
    }

    auto jpegContext = std::move(*jpeg_);
    jpegContext.output.appliedLensShadingCorrection = jpegContext.develop.lensShadingCorrectionEnabled;
    jpegContext.output.appliedFccSteps = jpegContext.develop.fccSteps;
    jpeg_.reset();
    jpegContext.output.demosaicMs = result.demosaicMs;
    jpegContext.output.demosaicSetupMs = result.demosaicSetupMs;
    jpegContext.output.colorProcessingMs = completed.colorProcessingMs;
    jpegContext.output.highlightReconstructionMs = completed.highlightReconstructionMs;
    jpegContext.output.refinementMs = completed.refinementMs;
    jpegContext.output.tonemapMs = completed.tonemapMs;
    jpegContext.output.denoiseMs = completed.denoiseMs;
    jpegContext.output.galoshYuvMs = completed.galoshYuvMs;
    jpegContext.output.gainmapMs = completed.gainmapMs;
    jpegContext.output.postTailMs = completed.postTailMs;
    jpegContext.output.highlightStage = completed.highlightStage;
    jpegContext.output.highlightForcedByUltraHdr = completed.highlightForcedByUltraHdr;
    jpegContext.output.defringeEnabled = completed.defringeEnabled;
    jpegContext.output.filmRendered = completed.filmRendered;
    jpegContext.output.filmFallbackMemory = completed.filmFallbackMemory;
    jpegContext.output.renderSetupMs = completed.renderSetupMs;
    jpegContext.output.renderEngineBuildMs = completed.engineBuildMs;
    jpegContext.output.queueGapsMs = completed.queueGapsMs;
    jpegContext.output.readbackMs = completed.readbackMs;
    jpegContext.output.renderTotalMs = result.demosaicMs + completed.totalMs;
    const std::string displayName = jpegContext.output.displayName;
    const bool ultraHdr =
        jpegContext.output.ultraHdr.enabled && result.pixels.gainmapPixels && result.pixels.gainmapBytes > 0;
    bool writerAccepted = false;
    if (ultraHdr) {
        jpegContext.output.outputFd = jpegFd_.release();
        writerAccepted = jpegWriter_.startUltraHdr(completed.requestId, result.pixels.pixels, result.pixels.bytes,
                                                   completed.width, completed.height, result.pixels.gainmapPixels,
                                                   result.pixels.gainmapBytes, result.pixels.gainmapWidth,
                                                   result.pixels.gainmapHeight, std::move(jpegContext.output));
    } else {
        if (jpegContext.output.ultraHdr.enabled)
            emit("ULTRAHDR_MAP_MISSING requestId=" + std::to_string(completed.requestId) + " fallback=legacy_jpeg");
        jpegContext.output.outputFd = jpegFd_.release();
        writerAccepted = jpegWriter_.start(completed.requestId, result.pixels.pixels, result.pixels.bytes,
                                           completed.width, completed.height, std::move(jpegContext.output));
    }
    if (!writerAccepted) {
        encoding::jpeg::JpegWriteCompletion failed{};
        failed.requestId = completed.requestId;
        failed.success = false;
        failed.displayName = displayName;
        failed.error = "jpeg_writer_start_rejected";
        jpegCompletion_ = std::move(failed);

        emit("JPEG_CAPTURE_WRITE_FAIL requestId=" + std::to_string(completed.requestId) +
             " error=jpeg_writer_start_rejected");
        return false;
    }
    emit("JPEG_CAPTURE_WRITE_STARTED requestId=" + std::to_string(completed.requestId) +
         " source=full_resolution_rgba8 colorspace=sRGB dct=islow" +
         (ultraHdr ? " ultrahdr=1 gainmap=" + std::to_string(result.pixels.gainmapWidth) + "x" +
                         std::to_string(result.pixels.gainmapHeight)
                   : " ultrahdr=0"));
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "JPEG_CAPTURE_WRITE_STARTED requestId=%llu ultrahdr=%d",
                        (unsigned long long)completed.requestId, ultraHdr ? 1 : 0);
    return true;
}
bool SingleFrameCaptureOutputs::advanceDng() {
    if (auto completed = dngWriter_.pollCompletion()) {
        std::ostringstream dng;
        dng << (completed->success ? "DNG_CAPTURE_WRITE_PASS" : "DNG_CAPTURE_WRITE_FAIL")
            << " requestId=" << completed->requestId << " writeMs=" << completed->writeMs
            << " fsyncMs=" << completed->fsyncMs << " fileBytes=" << completed->fileBytes
            << " displayName=" << completed->displayName;
        if (!completed->success) dng << " error=" << completed->error;
        emit(dng.str());
        dngCompletion_ = std::move(*completed);
        return true;
    }

    return false;
}
bool SingleFrameCaptureOutputs::advanceJpeg() {
    if (auto completed = jpegWriter_.pollCompletion()) {
        std::ostringstream jpeg;
        jpeg << (completed->success ? "JPEG_CAPTURE_WRITE_PASS" : "JPEG_CAPTURE_WRITE_FAIL")
             << " requestId=" << completed->requestId << " encodeMs=" << completed->encodeMs
             << " fsyncMs=" << completed->fsyncMs << " fileBytes=" << completed->fileBytes
             << " displayName=" << completed->displayName;
        if (!completed->success) jpeg << " error=" << completed->error;
        emit(jpeg.str());

        jpegCompletion_ = std::move(*completed);
        return true;
    }

    return false;
}
std::string SingleFrameCaptureOutputs::pollDngCompletion() {
    if (!dngCompletion_) return {};
    auto done = std::move(*dngCompletion_);
    dngCompletion_.reset();
    std::ostringstream out;
    out << done.requestId << '\t' << (done.success ? 1 : 0) << '\t' << done.displayName << '\t' << done.error;
    return out.str();
}
std::string SingleFrameCaptureOutputs::pollJpegCompletion() {
    if (!jpegCompletion_) return {};
    auto done = std::move(*jpegCompletion_);
    jpegCompletion_.reset();
    std::ostringstream out;
    out << done.requestId << '\t' << (done.success ? 1 : 0) << '\t' << done.displayName << '\t' << done.error << '\t'
        << (done.filmFallbackMemory ? 1 : 0);
    return out.str();
}
bool SingleFrameCaptureOutputs::dumpPreJpegPpm(uint64_t requestId, uint32_t width, uint32_t height,
                                               const void* rgbaData, size_t rgbaBytes) {
    if (!rgbaData || width == 0 || height == 0) {
        emit("PREJPEG_LOSSLESS_DUMP_FAIL requestId=" + std::to_string(requestId) + " error=invalid_source");
        return false;
    }
    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixelCount > std::numeric_limits<uint64_t>::max() / 4u) {
        emit("PREJPEG_LOSSLESS_DUMP_FAIL requestId=" + std::to_string(requestId) + " error=rgba_size_overflow");
        return false;
    }
    const uint64_t expectedRgbaBytes = pixelCount * 4u;
    if (expectedRgbaBytes != static_cast<uint64_t>(rgbaBytes)) {
        emit("PREJPEG_LOSSLESS_DUMP_FAIL requestId=" + std::to_string(requestId) + " error=rgba_size_mismatch");
        return false;
    }

    const std::string finalPath = filesDir_ + "/rawrcam_prejpeg_latest.ppm";
    const std::string temporaryPath = finalPath + ".tmp";
    try {
        std::ofstream out(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("open_failed");
        out << "P6\n" << width << ' ' << height << "\n255\n";
        const auto* rgba = static_cast<const uint8_t*>(rgbaData);
        std::vector<uint8_t> rgbRow(static_cast<size_t>(width) * 3u);
        for (uint32_t y = 0; y < height; ++y) {
            const auto* source = rgba + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
            for (uint32_t x = 0; x < width; ++x) {
                rgbRow[static_cast<size_t>(x) * 3u + 0u] = source[static_cast<size_t>(x) * 4u + 0u];
                rgbRow[static_cast<size_t>(x) * 3u + 1u] = source[static_cast<size_t>(x) * 4u + 1u];
                rgbRow[static_cast<size_t>(x) * 3u + 2u] = source[static_cast<size_t>(x) * 4u + 2u];
            }
            out.write(reinterpret_cast<const char*>(rgbRow.data()), static_cast<std::streamsize>(rgbRow.size()));
            if (!out) throw std::runtime_error("write_failed");
        }
        out.close();
        if (!out) throw std::runtime_error("close_failed");
        if (::rename(temporaryPath.c_str(), finalPath.c_str()) != 0) {
            throw std::runtime_error("rename_failed");
        }
        emit("PREJPEG_LOSSLESS_DUMP_PASS requestId=" + std::to_string(requestId) +
             " format=ppm_p6 source=exact_prejpeg_rgba8_rgb_channels width=" + std::to_string(width) +
             " height=" + std::to_string(height) + " path=rawrcam_prejpeg_latest.ppm");
        return true;
    } catch (const std::exception& e) {
        ::unlink(temporaryPath.c_str());
        emit("PREJPEG_LOSSLESS_DUMP_FAIL requestId=" + std::to_string(requestId) + " error=" + e.what());
        return false;
    }
}
void SingleFrameCaptureOutputs::failDng(uint64_t id, const std::string& reason) {
    encoding::dng::DngWriteCompletion done{};
    done.requestId = id;
    done.error = reason;
    if (dng_) done.displayName = dng_->displayName;
    cancelDng();
    dngCompletion_ = std::move(done);
}
void SingleFrameCaptureOutputs::failJpeg(uint64_t id, const std::string& reason, bool filmFallback) {
    encoding::jpeg::JpegWriteCompletion done{};
    done.requestId = id;
    done.error = reason;
    done.filmFallbackMemory = filmFallback;
    if (jpeg_) {
        done.displayName = jpeg_->output.displayName;
        jpeg_.reset();
    }
    jpegFd_.reset();
    jpegCompletion_ = std::move(done);
    emit("JPEG_CAPTURE_FAIL requestId=" + std::to_string(id) + " error=" + reason);
}
void SingleFrameCaptureOutputs::cancelDng() noexcept {
    dngFd_.reset();
    dng_.reset();
}
void SingleFrameCaptureOutputs::resetJpeg() noexcept {
    jpegWriter_.reset();
    jpegFd_.reset();
    jpeg_.reset();
    jpegCompletion_.reset();
}

}  // namespace rawrcam::capture
