#include "develop/render/FilmRenderStage.h"

#include <android/log.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "color/FilmExposure.h"

namespace rawrcam::develop::rendered {
FilmRenderOutcome renderFilmOrTone(RenderResources& resources, RenderCommandSession& commands,
                                   const RenderedStillContext& rendered, VkImageView sourceView,
                                   [[maybe_unused]] const std::string& filesDir,
                                   const RenderResources::Diagnostic& emit) {
    FilmRenderOutcome outcome{};
    tonemap::TonemapRecordInfo tri{};
    tri.commandBuffer = commands.command();
    tri.input = {resources.postDemosaic().sdrOutputView(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                 rendered.width, rendered.height};
    tri.output = {resources.output().view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, rendered.width,
                  rendered.height};
    tri.frameSlot = 0;
    tri.cameraToWorkingColumnMajor3x3 = rendered.cameraToWorkingColumnMajor.data();
    tri.params = rendered.tonemapParams;
    tonemap_integration::applyRenderProfile(rendered.colorRenderProfile, tri.params);
    if (tri.params.renderTransform != tonemap::RenderTransform::Existing)
        tri.input.view = resources.postDemosaic().outputView();
    bool filmOk = false;
    // A memory rejection is a distinct terminal render outcome. Capture
    // persistence replaces the JPEG with RAW; record faults below keep
    // their existing handling.
    bool filmFallbackMemory = false;
    if (rendered.filmEnabled) {
        try {
#ifndef NDEBUG
            // Deterministic device coverage of the existing memory-gate outcome.
            // Debug builds only; no setting or release behavior is added.
            if (::access((filesDir + "/.test-film-memory-fallback").c_str(), F_OK) == 0)
                filmOk = false;
            else
#endif
                filmOk = resources.ensureFilm(rendered);
            // ensureFilm returns false only on RAM-gate reject.
            if (!filmOk) filmFallbackMemory = true;
        } catch (const std::exception& e) {
            emit(std::string("STILL_FILM_FALLBACK error=") + e.what());
            __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", "STILL_FILM_FALLBACK error=%s", e.what());
            // On a production device the APK assets always exist, so
            // a build throw here is effectively a GPU-memory failure.
            filmFallbackMemory = true;
            filmOk = false;
        }
    }
    if (filmFallbackMemory) {
        outcome.memoryRejected = true;
        return outcome;
    }
    // Film glow-factor tap for film+UltraHDR (see GlowGainImageView):
    // allocated only when film will actually write it; otherwise the
    // gain map uses the pure scene (post-demosaic) tap. Declared
    // outside the film/tonemap branch so the gain-map block can see
    // the outcome.

    if (filmOk) {
        // Full-res film: the demosaiced still feeds the film chain
        // directly (same look + matrix as preview), writing the
        // output image. Grain is frozen at capture time
        // (deterministic per shot).
        spektrafilm_native::SpektraFilmRecordInfo fri{};
        fri.commandBuffer = commands.command();
        fri.input = {resources.postDemosaic().sdrOutputView(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                     rendered.width, rendered.height};
        if (rendered.replayBypassFcc) fri.input.view = sourceView;
        fri.output = {resources.output().view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, rendered.width,
                      rendered.height};
        fri.frameSlot = 0;
        if (rendered.filmTiled) fri.tilingMode = spektrafilm_native::GpuRenderTilingMode::Tiled;
        // Predict whether film will write the tap. willWriteGlowGain
        // only inspects scatter gating (enables, amounts, diffusion
        // solve), which is exposure-independent, so the pre-fold
        // look is sufficient (filmExposureEv is folded below).
        const bool wantGlow =
            rendered.ultraHdrEnabled && !rendered.replayBypassFcc && rendered.gainmapParams.glowStrength > 0.0f &&
            spektrafilm_native::SpektraFilm::willWriteGlowGain(rendered.filmLook, rendered.width, rendered.height,
                                                               fri.tilingMode, rendered.filmTiled);
        if (wantGlow) {
            resources.prepareGlow(rendered.width, rendered.height);
            VkImageMemoryBarrier glowInit{};
            glowInit.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            glowInit.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            glowInit.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            glowInit.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            glowInit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            glowInit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            glowInit.image = resources.glow().image;
            glowInit.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(commands.command(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &glowInit);
            fri.glowGainOutput = {resources.glow().view, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                                  rendered.width, rendered.height};
        }
        fri.look = rendered.filmLook;
        fri.diagnosticTap = rendered.filmDiagnosticTap;
        fri.diagnosticUserData = rendered.filmDiagnosticUserData;
        // Use the captured frame's gain just as preview does. Re-metering
        // the RAW here would normalize away camera EV compensation.
        fri.look.filmExposureEv = rawrcam::color::filmExposureEv(rendered.filmLook, rendered.tonemapParams.aePostGain);
        fri.adaptiveTone = {rendered.tonemapParams.adaptiveTone.liftEv, rendered.tonemapParams.adaptiveTone.keyEv,
                            rendered.tonemapParams.adaptiveTone.highlightEv, rendered.tonemapParams.aePostGain};
        fri.adaptiveToneLiftPerEv = tonemap::adaptiveToneLiftPerEv(rendered.tonemapParams.adaptiveTone);
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "STILL_FILM_EXPOSURE mode=fold gain=%.3f finalEv=%.2f",
                            rendered.tonemapParams.aePostGain, fri.look.filmExposureEv);
        fri.timeSec = static_cast<double>(rendered.timestampNs) / 1e9;
        static_assert(sizeof(fri.sensorToLinearSrgb) == sizeof(rendered.sensorToLinearSrgb),
                      "sensor matrix size mismatch");
        std::memcpy(fri.sensorToLinearSrgb, rendered.sensorToLinearSrgb.data(), sizeof(fri.sensorToLinearSrgb));
        try {
            // Keep the full-resolution demosaic and image initialization
            // out of the first film tile submit. The Adreno watchdog is
            // sensitive to a long command buffer even when each tile
            // is small; a completed submit also makes failures local
            // to the stage that actually lost the device.
            if (rendered.filmTiled) commands.submitAndRestart("film prepass");
            if (commands.timingPool())
                vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 6);
            if (rendered.filmTiled && fri.look.halationEnabled && fri.look.halationBoostEv > 0.0f) {
                resources.film().recordBoostMilestone(fri);
                commands.submitAndRestart("film boost milestone");
                const auto boost = resources.film().readBoostMilestone(fri.frameSlot);
                std::memcpy(fri.boostInfo, boost.values, sizeof(fri.boostInfo));
                fri.hasBoostInfo = true;
            }
            // A 512x256 film tile runs dozens of passes (~70 ms on Adreno 840) and
            // the passes vary too much in cost to batch safely, so every pass is its
            // own submission and the preview queue runs in between. This trades film
            // throughput (it now shares the GPU with a 30 fps preview) for a smooth
            // viewfinder.
            std::function<void()> filmPassFlush = [&] { commands.submitAndRestart("film pass"); };
            if (rendered.filmTiled) {
                fri.passBoundaryUserData = &filmPassFlush;
                fri.passBoundary = [](void* user) { (*static_cast<std::function<void()>*>(user))(); };
            }
            if (rendered.filmTiled) {
                const uint32_t columns = (rendered.width + fri.tileWidth - 1u) / fri.tileWidth;
                const uint32_t rows = (rendered.height + fri.tileHeight - 1u) / fri.tileHeight;
                const uint32_t tileCount = columns * rows;
                constexpr uint32_t tilesPerSubmit = 4u;
                for (uint32_t first = 0; first < tileCount; first += tilesPerSubmit) {
                    fri.tileFirst = first;
                    fri.tileCount = std::min(tilesPerSubmit, tileCount - first);
                    fri.tileFinalize = first + fri.tileCount == tileCount;
                    resources.film().record(fri);
                    if (!fri.tileFinalize) {
                        commands.submitAndRestart("film tiles " + std::to_string(first) + "-" +
                                                  std::to_string(first + fri.tileCount - 1u));
                    }
                }
            } else {
                resources.film().record(fri);
            }
            if (commands.timingPool())
                vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 7);
            outcome.rendered = true;
            outcome.usedGlow = fri.glowGainOutput.view != VK_NULL_HANDLE;
        } catch (const std::exception& e) {
            // A failed submit or wait leaves the command buffer ended or
            // still pending; only a failure while recording can fall back.
            if (rendered.rendererStrict || !commands.recording()) throw;
            // Never a broken still: a rejected look falls back to tonemap
            // for this shot (and logs loudly).
            emit(std::string("STILL_FILM_FALLBACK_RECORD error=") + e.what());
            __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", "STILL_FILM_FALLBACK_RECORD error=%s", e.what());
            if (commands.timingPool())
                vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 6);
            resources.tone().record(tri);
            if (commands.timingPool())
                vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 7);
        }
    } else {
        if (commands.timingPool())
            vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 6);
        resources.tone().record(tri);
        if (commands.timingPool())
            vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 7);
    }

    return outcome;
}
}  // namespace rawrcam::develop::rendered
