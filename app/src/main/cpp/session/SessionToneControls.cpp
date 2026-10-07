// Tone/LUT/color-mode controls.
#include "diagnostics/logging/NativeLog.h"
#include "session/SessionEngine.h"

namespace rawrcam::session {

void SessionEngine::setAssetManager(AAssetManager* assetManager) {
    std::lock_guard<std::mutex> lock(mu_);
    replayAssetManager_ = assetManager;
    look_.setAssetManager(assetManager);
    capture_.single().setFilmAssetManager(assetManager);
    capture_.multiframe().setFilmAssetManager(assetManager);
}

void SessionEngine::setColorRenderProfile(uint32_t profileId, std::string importedProfileId) {
    rawrcam::tonemap_integration::ColorRenderProfile profile{};
    if (!rawrcam::tonemap_integration::colorRenderProfileFromId(profileId, &profile)) {
        LOGW("Rejected invalid color render profile id=%u", profileId);
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (videoSession_.ready()) return;
    if (realtime_.setColorRenderProfile(profile, std::move(importedProfileId))) {
        videoSession_.renderLutChanged();
        // Also refreshes processing cached between recordings.
        {
            try {
                const auto renderLut = look_.videoRenderLut();
                videoSession_.updateRenderLut(renderLut ? &*renderLut : nullptr);
            } catch (const std::exception& error) {
                appendDiagnostic(std::string("VIDEO_RENDER_LUT_UPDATE_FAIL error=") + error.what());
                videoSession_.stop();
            }
        }
        appendDiagnostic(std::string("COLOR_RENDER_PROFILE selected=") +
                         rawrcam::tonemap_integration::colorRenderProfileName(profile));
    }
}

void SessionEngine::setMonitoringOverlay(uint32_t mode, float focusSensitivity) {
    std::lock_guard<std::mutex> lock(mu_);
    monitoringCoordinator_.setOverlay(mode, focusSensitivity);
}
void SessionEngine::setScopePresentationState(const rawrcam::monitoring::ScopePresentationState& state) {
    std::lock_guard<std::mutex> lock(mu_);
    monitoringCoordinator_.setScopePresentationState(state);
    swapchainRenderer_.setScopePresentationState(state);
}

void SessionEngine::setColorMode(const std::string& mode) {
    std::lock_guard<std::mutex> lock(mu_);
    colorMode_ = mode.empty() ? "auto" : mode;
    realtime_.coordinator().setColorMode(colorMode_);
    appendDiagnostic("COLOR_MODE mode=" + colorMode_ + " owner=native_color_module");
}

void SessionEngine::setFilmSimEnabled(bool enabled) { look_.setFilmSimEnabled(enabled); }

void SessionEngine::setViewfinderDivisor(uint32_t divisor) { look_.setViewfinderDivisor(divisor); }

void SessionEngine::setFilmSimLook(const spektrafilm_native::FilmLook& look) { look_.setFilmSimLook(look); }

void SessionEngine::setQuickToneNativeValue(int target, float value) { look_.setQuickToneNativeValue(target, value); }

void SessionEngine::setTonemapParameters(float exposureEV, float blackPointEV, float shadowLiftEV, float midtoneLiftEV,
                                         float contrast, float whitePointEV, float highlightBiasEV, float saturation,
                                         float vibrance) {
    look_.setTonemapParameters(exposureEV, blackPointEV, shadowLiftEV, midtoneLiftEV, contrast, whitePointEV,
                               highlightBiasEV, saturation, vibrance);
}

}  // namespace rawrcam::session
