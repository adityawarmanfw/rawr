#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "develop/render/RenderDeviceContext.h"
#include "develop/render/RenderedStillTypes.h"
#include "galosh/GaloshYuvPipeline.hpp"
#include "post_demosaic/PostDemosaicProcessor.h"
#include "vulkan/ImageResources.h"
#include "vulkan/VulkanContext.h"
namespace rawrcam::develop::rendered {
// Sole owner of images, mapped buffers, engines, assets and their cache keys.
class RenderResources final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    RenderResources(std::string filesDir, Diagnostic diagnostic)
        : filesDir_(std::move(filesDir)), diagnostic_(std::move(diagnostic)) {}
    ~RenderResources() { reset(); }
    void releasePixels() noexcept;
    void reset() noexcept;
    void bind(const RenderDeviceContext& context) noexcept;
    double prepareEngines(const RenderedStillContext& rendered);
    void setAssetManager(AAssetManager* assets) noexcept { assetManager_.store(assets); }
    void setEnginePersistenceEnabled(bool value) noexcept { enginePersistenceEnabled_.store(value); }
    void setFilmIdleEvictSeconds(int seconds) noexcept { filmIdleEvictSeconds_.store(seconds); }
    RenderOutputView outputView() const noexcept;
    RenderDeviceContext deviceContext() const noexcept {
        return {physicalDevice_, device_, queue_, queueFamily_, queueSubmitMutex_, float16Compute_};
    }
    bool holdsOutput() const noexcept { return output_.image != VK_NULL_HANDLE; }
    rawr::post::PostDemosaicProcessor& postDemosaic() { return *postDemosaic_; }
    tonemap::TonemapEngine& tone() { return *tonemap_; }
    spektrafilm_native::SpektraFilm& film() { return *film_; }
    ::galosh::GaloshYuvPipeline& galoshYuv() { return *galoshYuv_; }
    gainmap::GainmapCompute& gainmap() { return *gainmap_; }
    RenderImageView output() const noexcept { return {output_.image, output_.view}; }
    RenderImageView map() const noexcept { return {map_.image, map_.view}; }
    RenderImageView glow() const noexcept { return {glowGain_.image, glowGain_.view}; }
    VkImageView prepareGlow(uint32_t width, uint32_t height);
    VkBuffer readbackBuffer() const noexcept { return readback_; }
    VkBuffer gainmapReadbackBuffer() const noexcept { return mapReadback_; }

    bool ensureFilm(const RenderedStillContext& rendered);
    void destroyFilm() noexcept;

    void ensureGaloshYuv();
    void createPixels(const RenderedStillContext& rendered);

   private:
    void closeFilmAssets() noexcept;
    void emit(const std::string& line) const {
        if (diagnostic_) diagnostic_(line);
    }
    std::string filesDir_;
    Diagnostic diagnostic_;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    std::mutex* queueSubmitMutex_ = nullptr;

    rawrcam::vulkan::OwnedImage output_{};
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize readbackBytes_ = 0;
    // Film glow-factor tap (full-res RGBA16F quotient, transient): written
    // by SpektraFilm when film+UltraHDR run with a linear scatter path
    // (halation/camera diffusion); consumed as the gain-map glow input so
    // glow carries HDR headroom with hue. Empty (null image) on the tonemap
    // path or when film has no scatter path: gain map uses the pure scene tap.
    rawrcam::vulkan::OwnedImage glowGain_{};
    // UltraHDR map resources (still-capture only, half res).
    rawrcam::vulkan::OwnedImage map_{};
    VkBuffer mapReadback_ = VK_NULL_HANDLE;
    VkDeviceMemory mapReadbackMemory_ = VK_NULL_HANDLE;
    void* mapMapped_ = nullptr;
    VkDeviceSize mapReadbackBytes_ = 0;
    uint32_t mapWidth_ = 0;
    uint32_t mapHeight_ = 0;
    std::unique_ptr<gainmap::GainmapCompute> gainmap_;
    VkDevice gainmapDevice_ = VK_NULL_HANDLE;
    std::unique_ptr<rawr::post::PostDemosaicProcessor> postDemosaic_;
    std::unique_ptr<tonemap::TonemapEngine> tonemap_;
    // GALOSH-YUV blind denoiser (P1c): lazy, first enabled shot. Null
    // without float16 compute (tap stays off, wavelet serves). Owns its
    // scratch per geometry like the RAW tap in SharedHighlightRuntime.
    std::unique_ptr<::galosh::GaloshYuvPipeline> galoshYuv_;
    // Device the galosh pipeline was built on. start() overwrites device_
    // every shot; a device change (surface recreation) must rebuild galosh
    // instead of submitting to a destroyed VkDevice.
    VkDevice galoshYuvDevice_ = VK_NULL_HANDLE;
    bool float16Compute_ = false;
    // Rebuild match keys for engine persistence. Compared in start(); a
    // mismatch rebuilds that engine only (build-then-swap, so a failed build
    // keeps the previous engine).
    struct ToneKey {
        rawrcam::tonemap_integration::ColorRenderProfile profile{};
        std::string lutId;
        float strength = 1.0f;
        VkDevice device = VK_NULL_HANDLE;
        bool operator==(const ToneKey& other) const noexcept;
    };
    struct PostDemosaicKey {
        uint32_t width = 0, height = 0, fccSteps = 0;
        bool normalized = false;
        float edgeSigma = 0.0f, chromaBound = 0.0f, defringe = 0.0f, defringeEdge = 0.02f, defringeLuma = 0.08f;
        VkDevice device = VK_NULL_HANDLE;
        bool operator==(const PostDemosaicKey& other) const noexcept;
    };
    ToneKey toneKey_;
    PostDemosaicKey postKey_;
    // A null engine always rebuilds, so no separate validity flags needed.
    std::atomic<bool> enginePersistenceEnabled_{false};
    // Still film engine (full-res arena, cached across shots per stock).
    // Null until first film still (or asset manager missing); tonemap is the
    // fallback. Evicted after kFilmIdleEvictSeconds without a still so
    // gigabytes of scratch don't sit resident while preview allocates.
    static constexpr int kFilmIdleEvictSeconds = 30;
    std::atomic<AAssetManager*> assetManager_{nullptr};
    std::unique_ptr<spektrafilm_native::SpektraFilm> film_;
    spektrafilm_native::FilmLook filmBaked_{};
    bool filmBakedDiagnostics_ = false;
    bool filmBakedTiled_ = false;
    std::chrono::steady_clock::time_point lastFilmUse_{};
    uint32_t filmArenaW_ = 0;
    uint32_t filmArenaH_ = 0;
    VkDevice filmDevice_ = VK_NULL_HANDLE;
    std::atomic<int> filmIdleEvictSeconds_{kFilmIdleEvictSeconds};
    struct FilmAssets {
        AAsset* hanatos = nullptr;
        AAsset* gamut = nullptr;
    };
    FilmAssets filmAssets_;
};
/** "need=<MB> free=<MB>" from the last film memory gate check, for the capture's failure message. */
std::string lastFilmGateSummary();

}  // namespace rawrcam::develop::rendered
