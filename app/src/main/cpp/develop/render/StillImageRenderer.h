#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "develop/render/RenderCommandSession.h"
#include "develop/render/RenderResources.h"

struct AAssetManager;
struct AAsset;

namespace rawrcam::develop::rendered {

// Owns the shared post-demosaic still path: explicit camera RGB white balance,
// configurable FCC, TonemapEngine, and RGBA8 readback. Demosaic mathematics stay in
// their algorithm modules; color/tonemap mathematics stay in their owners.
// The stage stops at a full-resolution RGBA8 in-memory result.
class StillImageRenderer final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    explicit StillImageRenderer(std::string filesDir, Diagnostic diagnostic = {});
    ~StillImageRenderer();
    // APK asset access for the film lookup tables (.f32). Forwards to the
    // still film engine on next creation; safe to call any time.
    void setAssetManager(AAssetManager* assetManager) noexcept;
    // Destroys the cached still film engine (device teardown ordering).
    void shutdownFilm() noexcept;
    // Persistent-engine toggle ("Persistent Engine" in Settings > Experimental).
    // Off (default) preserves today's behavior exactly: engines are rebuilt
    // every capture. On, TonemapEngine/PostDemosaicProcessor/film survive
    // across captures subject to the resource owner's cache keys. The atomic
    // setter runs on the settings thread, readers on worker threads; a flip
    // takes effect on the next capture at the latest.
    void setEnginePersistenceEnabled(bool enabled) noexcept { resources_.setEnginePersistenceEnabled(enabled); }
    // Idle evict timeout for the cached film engine (default 30s).
    // Multiframe captures take ~1min end-to-end, so the 30s default would
    // evict between every two shots and reuse could never trigger. Owners
    // with slower cadence raise this; <= 0 disables eviction.
    void setFilmIdleEvictSeconds(int seconds) noexcept { resources_.setFilmIdleEvictSeconds(seconds); }

    StillImageRenderer(const StillImageRenderer&) = delete;
    StillImageRenderer& operator=(const StillImageRenderer&) = delete;

    bool start(const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex,
               const RenderedStillContext& rendered, VkImage sourceImage, VkImageView sourceView,
               VkImage clipStateImage = VK_NULL_HANDLE, VkImageView clipStateView = VK_NULL_HANDLE,
               // Optional submit-queue override (same device + queue family, no
               // ownership transfer). Lets the multiframe merge run the still
               // chain on the multiframe queue so preview submits on queue0 no
               // longer wait behind the whole still render in one FIFO.
               VkQueue queueOverride = VK_NULL_HANDLE, std::mutex* queueSubmitMutexOverride = nullptr);
    std::optional<RenderedStillCompletion> pollCompletion();
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] const void* pixelData() const noexcept { return resources_.outputView().pixels; }
    [[nodiscard]] VkImage outputImage() const noexcept { return resources_.output().image; }
    [[nodiscard]] VkImageView outputImageView() const noexcept { return resources_.outputView().imageView; }
    [[nodiscard]] size_t pixelBytes() const noexcept { return static_cast<size_t>(resources_.outputView().bytes); }
    // Half-res recovery map (RGBA8, A ignored; RGB when multi-channel, gray
    // R=G=B in single-channel mode). Null unless the completed shot ran with
    // ultraHdrEnabled. Borrowed until releasePixels(), like pixelData().
    [[nodiscard]] const void* gainmapPixelData() const noexcept { return resources_.outputView().gainmapPixels; }
    [[nodiscard]] size_t gainmapPixelBytes() const noexcept {
        return static_cast<size_t>(resources_.outputView().gainmapBytes);
    }
    [[nodiscard]] uint32_t gainmapWidth() const noexcept { return resources_.outputView().gainmapWidth; }
    [[nodiscard]] uint32_t gainmapHeight() const noexcept { return resources_.outputView().gainmapHeight; }
    void releasePixels() noexcept;
    void reset() noexcept;

   private:
    void joinWorker() noexcept;
    void emit(const std::string& line) const;

    std::string filesDir_;
    Diagnostic diagnostic_;
    RenderResources resources_;
    RenderCommandSession commands_;
    mutable std::mutex stateMutex_;
    std::thread worker_;
    bool busy_ = false;
    std::optional<RenderedStillCompletion> completion_;
};

}  // namespace rawrcam::develop::rendered
