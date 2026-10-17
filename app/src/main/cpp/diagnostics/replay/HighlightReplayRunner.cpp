// Frozen production GPU replay. Legacy highlight A/B and explicit film diagnostics.
#include "diagnostics/replay/HighlightReplayRunner.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "develop/demosaic/DualStillProcessor.h"
#include "develop/render/StillImageRenderer.h"
#include "diagnostics/replay/FilmReplayReadback.h"
#include "encoding/jpeg/JpegCaptureWriter.h"

namespace rawrcam::diagnostics::replay {

std::string HighlightReplayRunner::run(const std::string& inputPath) {
    try {
        uint32_t width = 4080u, height = 3072u, cfa = 0u, fccSteps = 2u;
        bool dualAutoContrast = true;
        float dualContrastPercent = 20.0f;
        std::string sourceName = "legacy";
        std::array<float, 4> wbRggb{2.04004f, 1.0f, 1.0f, 1.66504f};
        std::array<float, 3> wbRgb{2.04004f, 1.0f, 1.66504f};
        std::array<float, 9> cameraToWorking{0.657541f,  -0.105752f, -0.0182974f, 0.268878f, 1.1262f,
                                             -0.436998f, 0.0671213f, -0.0157576f, 1.45875f};
        float exposureEV = 0.0f;
        bool filmReplay = false, recovery = true, diagnostics = false;
        int jpegQuality = 98;
        std::string replayName = "film";
        std::array<float, 4> filmRoi{};
        rawrcam::develop::rendered::RenderedStillContext filmContext{};
        // Production edge-aware FCC defaults; config keys below can override
        // (0 selects legacy uniform/unbounded for archived comparisons).
        filmContext.fccEdgeSigma = 0.08f;
        filmContext.fccChromaBound = 1.0f;
        filmContext.defringeStrength = 1.0f;
        // A replay configuration is self-contained; never inherit mutable UI state.

        const auto parseFloatList = [](const std::string& text, float* output, size_t count) {
            std::istringstream stream(text);
            std::string token;
            for (size_t i = 0; i < count; ++i) {
                if (!std::getline(stream, token, ',')) return false;
                output[i] = std::stof(token);
            }
            return !std::getline(stream, token, ',');
        };
        std::ifstream config(inputPath + ".txt");
        const bool hasConfig = config.good();
        if (hasConfig) {
            std::string line;
            while (std::getline(config, line)) {
                const size_t separator = line.find('=');
                if (separator == std::string::npos) continue;
                const std::string key = line.substr(0, separator);
                const std::string value = line.substr(separator + 1u);
                if (key == "filmEnabled")
                    filmReplay = std::stoi(value) != 0;
                else if (key == "replayName")
                    replayName = value;
                else if (key == "recovery")
                    recovery = std::stoi(value) != 0;
                else if (key == "replayBypassFcc")
                    filmContext.replayBypassFcc = std::stoi(value) != 0;
                else if (key == "normalizedFcc" || key == "replayNormalizedFcc")
                    filmContext.normalizedFcc = std::stoi(value) != 0;
                else if (key == "fccEdgeSigma")
                    filmContext.fccEdgeSigma = std::stof(value);
                else if (key == "fccChromaBound")
                    filmContext.fccChromaBound = std::stof(value);
                else if (key == "defringeStrength")
                    filmContext.defringeStrength = std::stof(value);
                else if (key == "diagnostics")
                    diagnostics = std::stoi(value) != 0;
                else if (key == "jpegQuality")
                    jpegQuality = std::stoi(value);
                else if (key == "filmDiagnosticRoi") {
                    if (!parseFloatList(value, filmRoi.data(), 4)) return "HIGHLIGHT_REPLAY_FAIL roi";
                } else if (key == "aePostGain")
                    filmContext.tonemapParams.aePostGain = std::stof(value);
                else if (key == "sensorToLinearSrgb") {
                    if (!parseFloatList(value, filmContext.sensorToLinearSrgb.data(), 9))
                        return "HIGHLIGHT_REPLAY_FAIL invalid_film_matrix";
                } else if (key == "distortionEnabled")
                    filmContext.distortionCorrectionEnabled = std::stoi(value) != 0;
                else if (key == "lensIntrinsic") {
                    if (!parseFloatList(value, filmContext.lensIntrinsic.data(), 5))
                        return "HIGHLIGHT_REPLAY_FAIL intrinsic";
                    filmContext.hasLensCalibration = true;
                } else if (key == "lensDistortion") {
                    if (!parseFloatList(value, filmContext.lensDistortion.data(), 5))
                        return "HIGHLIGHT_REPLAY_FAIL distortion";
                }
#define RAWR_FILM_FIELD(type, name) \
    else if (key == "film." #name) filmContext.filmLook.name = static_cast<type>(std::stod(value));
#include "tonemap/FilmLookFields.inc"
#undef RAWR_FILM_FIELD
                else if (key == "source")
                    sourceName = value;
                else if (key == "width")
                    width = static_cast<uint32_t>(std::stoul(value));
                else if (key == "height")
                    height = static_cast<uint32_t>(std::stoul(value));
                else if (key == "cfa")
                    cfa = static_cast<uint32_t>(std::stoul(value));
                else if (key == "fccSteps")
                    fccSteps = static_cast<uint32_t>(std::stoul(value));
                else if (key == "dualAutoContrast")
                    dualAutoContrast = std::stoul(value) != 0u;
                else if (key == "dualContrastPercent")
                    dualContrastPercent = std::stof(value);
                else if (key == "exposureEV")
                    exposureEV = std::stof(value);
                else if (key == "wbRggb" && !parseFloatList(value, wbRggb.data(), wbRggb.size()))
                    return "HIGHLIGHT_REPLAY_FAIL invalid_wbRggb";
                else if (key == "wbRgb" && !parseFloatList(value, wbRgb.data(), wbRgb.size()))
                    return "HIGHLIGHT_REPLAY_FAIL invalid_wbRgb";
                else if (key == "cameraToWorkingColumnMajor" &&
                         !parseFloatList(value, cameraToWorking.data(), cameraToWorking.size()))
                    return "HIGHLIGHT_REPLAY_FAIL invalid_camera_matrix";
            }
        }

        if (width == 0 || height == 0 || width % 2 || height % 2 || cfa > 3 || fccSteps > 8 || fccSteps < 1 ||
            jpegQuality < 1 || jpegQuality > 100 || replayName.empty() ||
            replayName.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                std::string::npos)
            return "HIGHLIGHT_REPLAY_FAIL invalid_config";
        std::ifstream in(inputPath, std::ios::binary | std::ios::ate);
        if (!in) return "HIGHLIGHT_REPLAY_FAIL open=" + inputPath;
        const auto size = in.tellg();
        if (!hasConfig && size == static_cast<std::streamoff>(static_cast<size_t>(4080u / 2u) * (3064u / 2u) * 8u))
            height = 3064u;
        const size_t expected = static_cast<size_t>(width / 2u) * (height / 2u) * 8u;
        if (size < 0 || static_cast<size_t>(size) != expected)
            return "HIGHLIGHT_REPLAY_FAIL size=" + std::to_string(static_cast<long long>(size)) +
                   " expected=" + std::to_string(expected);
        in.seekg(0);
        std::vector<uint8_t> tagged(expected);
        in.read(reinterpret_cast<char*>(tagged.data()), static_cast<std::streamsize>(tagged.size()));
        if (!in) return "HIGHLIGHT_REPLAY_FAIL read";

        const auto error = prepare_();
        if (!error.empty()) return error;
        appendDiagnostic("HIGHLIGHT_REPLAY_STAGE configured_input source=" + sourceName + " dimensions=" +
                         std::to_string(width) + "x" + std::to_string(height) + " cfa=" + std::to_string(cfa));
        appendDiagnostic("HIGHLIGHT_REPLAY_STAGE preview_resources_released vulkan_preserved=1 gpu=" +
                         vulkanContext_.gpuName());

        rawrcam::develop::demosaic::dual::DualStillProcessor demosaic(
            [this](const std::string& line) { appendDiagnostic("HIGHLIGHT_REPLAY " + line); });
        demosaic.configure(vulkanContext_, queueSubmitMutex_, 1u, "replay", width, height, cfa, dualAutoContrast,
                           dualContrastPercent, false);

        rawrcam::develop::rendered::RenderedStillContext ctx = filmContext;
        ctx.width = width;
        ctx.height = height;
        ctx.presentationQuarterTurns = 0;
        ctx.whiteBalanceRgb = wbRgb;
        ctx.cameraToWorkingColumnMajor = cameraToWorking;
        ctx.tonemapParams = tonemap::TonemapPresets::NeutralBaseline().params;
        ctx.tonemapParams.exposureEV = exposureEV;
        ctx.tonemapParams.aePostGain = filmReplay ? filmContext.tonemapParams.aePostGain : 1.0f;
        ctx.filmEnabled = filmReplay;
        ctx.tonemapParams.blackPointEV = 0.0f;
        ctx.tonemapParams.shadowLiftEV = 0.0f;
        ctx.tonemapParams.midtoneLiftEV = 0.0f;
        ctx.tonemapParams.contrast = 0.0f;
        ctx.tonemapParams.whitePointEV = 0.0f;
        ctx.tonemapParams.highlightBiasEV = 0.0f;
        ctx.tonemapParams.saturation = 0.0f;
        ctx.tonemapParams.vibrance = 0.0f;
        ctx.colorRenderProfile = rawrcam::tonemap_integration::ColorRenderProfile::RawrBase;
        ctx.fccSteps = fccSteps;
        ctx.diagnosticsEnabled = diagnostics;
        ctx.diagnosticRoi = {uint32_t(filmRoi[0]), uint32_t(filmRoi[1]), uint32_t(filmRoi[2]), uint32_t(filmRoi[3])};
        FilmReplayReadback taps(
            vulkanContext_.physicalDevice(), vulkanContext_.device(),
            {uint32_t(filmRoi[0]), uint32_t(filmRoi[1]), uint32_t(filmRoi[2]), uint32_t(filmRoi[3])});
        if (filmRoi[2] > 0 && filmRoi[3] > 0) {
            ctx.filmDiagnosticTap = &FilmReplayReadback::record;
            ctx.filmDiagnosticUserData = &taps;
        }

        std::ofstream manifest(filesDir_ + "/highlight_lab_manifest.txt", std::ios::trunc);
        if (!manifest) return "HIGHLIGHT_REPLAY_FAIL manifest_open";
        manifest << "source=" << sourceName << "\nwidth=" << width << "\nheight=" << height << "\ncfa=" << cfa
                 << "\nwb=" << wbRggb[0] << ',' << wbRggb[1] << ',' << wbRggb[2] << ',' << wbRggb[3]
                 << "\nfcc=" << fccSteps << "\n"
                 << "demosaic=production_dual_rcd_vng4\n"
                 << "reconstruction=production_post_wb\n"
                 << "tonemap_highlights=-100,0\n";

        manifest << std::setprecision(9) << "gpu=" << vulkanContext_.gpuName() << "\nfilmEnabled=" << filmReplay
                 << "\naePostGain=" << ctx.tonemapParams.aePostGain << "\njpegQuality=" << jpegQuality
                 << "\njpegSubsampling=422\n";
        manifest << "replayBypassFcc=" << ctx.replayBypassFcc << "\nnormalizedFcc=" << ctx.normalizedFcc
                 << "\nfccEdgeSigma=" << ctx.fccEdgeSigma << "\nfccChromaBound=" << ctx.fccChromaBound
                 << "\ndefringeStrength=" << ctx.defringeStrength << "\nrecovery=" << recovery
                 << "\nsensorToLinearSrgb=";
        for (size_t i = 0; i < ctx.sensorToLinearSrgb.size(); ++i)
            manifest << (i ? "," : "") << ctx.sensorToLinearSrgb[i];
        manifest << "\ndiagnosticRoi=" << filmRoi[0] << ',' << filmRoi[1] << ',' << filmRoi[2] << ',' << filmRoi[3]
                 << '\n';
#define RAWR_FILM_FIELD(type, name) manifest << "film." #name "=" << ctx.filmLook.name << "\n";
#include "tonemap/FilmLookFields.inc"
#undef RAWR_FILM_FIELD
        uint64_t requestId = 9200u;
        bool packedDumped = false;
        for (const bool reconstructionEnabled : {false, true}) {
            if (filmReplay && reconstructionEnabled != recovery) continue;
            const char* recoveryName = reconstructionEnabled ? "recon_on" : "recon_off";
            const float highlightValues[] = {-100.0f, 0.0f};
            for (const float highlightBias : highlightValues) {
                if (filmReplay && highlightBias != 0.0f) continue;
                // StillImageRenderer writes intermediate results into the demosaic
                // image. Re-run the production demosaic for every output so the A/B
                // variants all start from the identical frozen CFA.
                std::vector<uint8_t> payload = tagged;
                if (!demosaic.startTaggedPackedReplay(std::move(payload), wbRggb, 0u, 0.16f, 0.22f, 0.22f, 3u,
                                                      requestId++))
                    return std::string("HIGHLIGHT_REPLAY_FAIL demosaic_start=") + recoveryName;
                for (;;) {
                    if (auto done = demosaic.pollCompletion()) {
                        if (!done->success)
                            return std::string("HIGHLIGHT_REPLAY_FAIL demosaic=") + recoveryName +
                                   " error=" + done->error;
                        manifest << "demosaic_for=" << recoveryName << " highlights=" << highlightBias
                                 << " gpu_submit_to_fence_ms=" << done->submitToFenceMs << "\n";
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (!packedDumped) {
                    if (!demosaic.dumpPackedCfaRgba16f(filesDir_ + "/highlight_lab_packed.rgba16f"))
                        return "HIGHLIGHT_REPLAY_FAIL packed_dump";
                    packedDumped = true;
                }

                const char* suffix = highlightBias < -50.0f ? "hm100" : (highlightBias > 50.0f ? "hp100" : "h0");
                ctx.requestId = requestId++;
                ctx.tonemapParams.highlightBiasEV = highlightBias;
                ctx.highlightReconstructionEnabled = reconstructionEnabled;
                appendDiagnostic(std::string("HIGHLIGHT_REPLAY_STAGE render_begin recovery=") + recoveryName +
                                 " highlights=" + std::to_string(highlightBias));
                rawrcam::develop::rendered::StillImageRenderer render(
                    filesDir_, [this](const std::string& line) { appendDiagnostic("HIGHLIGHT_REPLAY " + line); });
                render.setAssetManager(replayAssetManager_);
                if (!render.start(vulkanContext_, queueSubmitMutex_, ctx, demosaic.outputImage(), demosaic.outputView(),
                                  demosaic.clipStateImage(), demosaic.clipStateView()))
                    return std::string("HIGHLIGHT_REPLAY_FAIL render_start=") + recoveryName + " highlights=" + suffix;
                for (;;) {
                    if (auto done = render.pollCompletion()) {
                        if (!done->success || (filmReplay && !done->filmRendered))
                            return std::string("HIGHLIGHT_REPLAY_FAIL render=") + recoveryName +
                                   " highlights=" + suffix + " error=" + done->error;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                const auto* px = static_cast<const uint8_t*>(render.pixelData());
                const std::string stem =
                    filmReplay ? replayName : std::string("highlight_lab_") + recoveryName + "_" + suffix;
                const std::string ppmPath = filesDir_ + "/" + stem + ".ppm";
                std::ofstream out(ppmPath, std::ios::binary | std::ios::trunc);
                if (!out) return std::string("HIGHLIGHT_REPLAY_FAIL output=") + ppmPath;
                out << "P6\n" << width << " " << height << "\n255\n";
                for (size_t k = 0, n = static_cast<size_t>(width) * height; k < n; ++k) {
                    out.put(static_cast<char>(px[k * 4 + 0]));
                    out.put(static_cast<char>(px[k * 4 + 1]));
                    out.put(static_cast<char>(px[k * 4 + 2]));
                }
                out.close();
                if (filmReplay && ctx.filmDiagnosticTap) taps.write(filesDir_ + "/" + stem);
                if (filmReplay) {
                    rawrcam::encoding::jpeg::JpegCaptureContext jpeg{};
                    jpeg.outputFd = rawrcam::encoding::OutputFd(
                        ::open((filesDir_ + "/" + stem + ".jpg").c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600));
                    jpeg.quality = jpegQuality;
                    jpeg.subsampling = rawrcam::encoding::jpeg::ChromaSubsampling::Yuv422;
                    jpeg.exifOrientation = 6;
                    jpeg.imageDescription = "Frozen production GPU replay: " + sourceName;
                    rawrcam::encoding::jpeg::JpegCaptureWriter writer;
                    if (!writer.start(requestId++, px, render.pixelBytes(), width, height, std::move(jpeg)))
                        return "HIGHLIGHT_REPLAY_FAIL jpeg_start";
                    for (;;) {
                        if (auto done = writer.pollCompletion()) {
                            if (!done->success) return "HIGHLIGHT_REPLAY_FAIL jpeg=" + done->error;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
                render.releasePixels();
                if (!out) return std::string("HIGHLIGHT_REPLAY_FAIL write=") + ppmPath;
                manifest << "render=" << recoveryName << " highlights=" << highlightBias
                         << " file=" << ppmPath.substr(ppmPath.find_last_of('/') + 1) << "\n";
            }
        }
        manifest.close();
        appendDiagnostic("HIGHLIGHT_REPLAY_PASS mode=production_full_pipeline outputs=" +
                         std::string(filmReplay ? "1" : "4") + " input=" + inputPath + " cfa=" + std::to_string(cfa));
        return "HIGHLIGHT_REPLAY_PASS";
    } catch (const std::exception& e) {
        appendDiagnostic(std::string("HIGHLIGHT_REPLAY_FAIL exception=") + e.what());
        return std::string("HIGHLIGHT_REPLAY_FAIL ") + e.what();
    }
}

}  // namespace rawrcam::diagnostics::replay
