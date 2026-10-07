#pragma once
#include <android/asset_manager.h>
#include <spektrafilm/SpektraFilm.h>
#include <tonemap/TonemapEngine.h>
#include <tonemap/lut/LutChain.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "tonemap/ColorRenderProfile.h"
#include "vulkan/VulkanContext.h"
namespace rawrcam::pipeline {
class PreviewLookController final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    using Publish = std::function<void(bool, const spektrafilm_native::FilmLook&)>;
    PreviewLookController(vulkan::VulkanContext& context, std::mutex& transitionMutex, std::mutex& queueMutex,
                          std::string filesDir, Diagnostic diagnostic, Publish publish,
                          std::function<void(const tonemap::TonemapParams&)> publishTone,
                          std::function<void(uint32_t)> publishDivisor);
    ~PreviewLookController();
    void start();
    void stop();
    void configureGeometry(uint32_t width, uint32_t height);
    void clearProcessing(bool teardownFilm);
    void setFilmSimEnabled(bool enabled);
    void setViewfinderDivisor(uint32_t divisor);
    void setFilmSimLook(const spektrafilm_native::FilmLook& look);
    void setQuickToneNativeValue(int target, float value);
    void setTonemapParameters(float exposureEV, float blackPointEV, float shadowLiftEV, float midtoneLiftEV,
                              float contrast, float whitePointEV, float highlightBiasEV, float saturation,
                              float vibrance);
    void setAssetManager(AAssetManager* assets) noexcept { assetManager_ = assets; }
    bool requestedFilmEnabled() const noexcept { return requestedFilmEnabled_; }
    const spektrafilm_native::FilmLook& filmLook() const noexcept { return filmLook_; }
    const tonemap::TonemapParams& toneParams() const noexcept { return tonemapParams_; }
    tonemap::TonemapEngine* tone() const noexcept { return tonemap_.get(); }
    std::shared_ptr<spektrafilm_native::SpektraFilm> acquireFilm() const;
    // Configuration/profile/suppression methods require the session transition gate.
    bool selectProfile(tonemap_integration::ColorRenderProfile profile, std::string importedProfileId);
    void suppressForVideo(bool suppressed);
    std::optional<tonemap::lut::LutChain> videoRenderLut() const;
    void createTone();

   private:
    struct PreparedFilm {
        std::shared_ptr<spektrafilm_native::SpektraFilm> engine;
        spektrafilm_native::FilmLook look{};
        uint32_t arenaWidth = 0, arenaHeight = 0;
    };
    struct FilmBuildRequest {
        spektrafilm_native::FilmLook look;
        uint32_t width, height;
        uint64_t generation;
    };
    bool canPrepareFilm() const noexcept;
    bool needsFilmRebuild(const spektrafilm_native::FilmLook& look) const noexcept;
    PreparedFilm prepareFilm(const spektrafilm_native::FilmLook& look, uint32_t width, uint32_t height) const;
    bool preparedFilmFits(const PreparedFilm& prepared) const noexcept;
    void installPreparedFilm(PreparedFilm prepared);
    bool applyFilmSimState(bool enabled, const spektrafilm_native::FilmLook& bakedLook, bool isLookUpdate = false);
    void createFilm();
    void scheduleFilmBuildLocked();
    void filmBuildLoop();
    void emit(const std::string& line) const {
        if (diagnostic_) diagnostic_(line);
    }
    vulkan::VulkanContext& vulkanContext_;
    std::mutex& transitionMutex_;
    std::mutex& queueSubmitMutex_;
    std::string filesDir_;
    Diagnostic diagnostic_;
    Publish publish_;
    std::function<void(const tonemap::TonemapParams&)> publishTone_;
    std::function<void(uint32_t)> publishDivisor_;
    std::unique_ptr<tonemap::TonemapEngine> tonemap_;
    tonemap::TonemapParams tonemapParams_ = tonemap::TonemapPresets::NeutralBaseline().params;
    std::shared_ptr<spektrafilm_native::SpektraFilm> film_;
    mutable std::mutex filmMutex_;
    AAssetManager* assetManager_ = nullptr;
    bool filmEnabled_ = false, filmHaveLook_ = false;
    bool requestedFilmEnabled_ = false, videoSuppressed_ = false;
    spektrafilm_native::FilmLook filmLook_{}, filmLookBaked_{};
    uint32_t filmArenaW_ = 0, filmArenaH_ = 0, previewWidth_ = 0, previewHeight_ = 0;
    tonemap_integration::ColorRenderProfile colorRenderProfile_ = tonemap_integration::ColorRenderProfile::RawrBase;
    std::string importedLutProfileId_;
    std::mutex filmBuildMutex_;
    std::condition_variable filmBuildCv_;
    std::optional<FilmBuildRequest> pendingFilmBuild_;
    std::atomic<bool> filmBuildStop_{false};
    uint64_t filmBuildGeneration_ = 0;
    std::thread filmBuildThread_;
};
}  // namespace rawrcam::pipeline
