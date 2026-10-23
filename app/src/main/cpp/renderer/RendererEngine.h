#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "develop/DevelopSettings.h"
#include "DngSource.h"
#include "develop/render/StillImageRenderer.h"
namespace image_scopes { class ImageScopes; }
namespace rawr::capsharp { class CaptureSharpening; }
namespace rawrcam::renderer {
struct RenderOptions {
    develop::rendered::RenderedStillContext context;
    // Recipe develop settings, kept for the export's ImageDescription.
    develop::DevelopSettings develop;
    // Editor shadow-lift mode override ("Off".."Strong"); empty replays the
    // captured adaptive tone. adaptiveToneMeasured: the stored key/highlight
    // came from the camera meter (a lift mode was on at capture).
    std::string adaptiveToneMode;
    bool adaptiveToneMeasured = false;
    uint32_t width = 0, height = 0;
    std::array<uint32_t, 4> region{}; // Sensor-space preview extent; zero width selects the active crop.
    float temperature = 0, tint = 0;
    bool overview = false;
    bool quickProxy = false;
    bool bayerBin2x = false;
    bool waveform = false;
    bool shading = true;
    bool distortion = true;
    std::string demosaic = "Rcd";
    bool dualAutoContrast = true;
    float dualContrastPercent = 20;
    // adapt004 quad-cell lattice pre-filter (after normalize+shading, before
    // demosaic). Default off: zero behavior change until device validation.
    // Auto-applies to the affected sensor even when false? No — explicit only.
    bool quadfix = false;
    bool quadfixFastMedian = false;
};
class RendererEngine {
   public:
    RendererEngine(const std::string& filesDir, const std::string& libraryDir, AAssetManager* assets);
    ~RendererEngine();
    void prepareProxy(DngSource& source, const std::string& cachePath, bool shading);
    void prepare(DngSource& source, const std::string& cachePath, const RenderOptions& options);
    std::vector<uint8_t> render(DngSource& source, const std::string& cachePath, const RenderOptions& options);
    // Capture sharpening for the Renderer: sharpens the full-resolution
    // demosaic cache (width x height RGBA16F, pre-WB) once per level and
    // returns the sharpened cache path; previews and exports resample from it,
    // so the still pipeline's own stage stays off here.
    std::string captureSharpened(const DngSource& source, const std::string& cachePath, uint32_t width,
                                 uint32_t height, uint32_t iterations);
    const std::vector<uint32_t>& waveform() const { return waveformPixels_; }
    // Last post-demosaic completion (GPU stage times, setup/gaps/readback).
    // Populated by render(); valid only when render() succeeded. Lets the
    // JPEG export path report the same TIMINGS stages as still captures.
    const develop::rendered::RenderedStillCompletion& lastCompletion() const { return lastCompletion_; }
    // UltraHDR gain map for the last render() (half-res RGBA8, A ignored).
    // Empty when the render ran without ultraHdrEnabled (preview, SDR
    // export, or map missing). Copied before releasePixels() so the JPEG
    // exporter holds stable memory until its writer completes, mirroring
    // SingleFrame/MFSR ownership (processor pixels stay mapped there).
    const std::vector<uint8_t>& lastGainmap() const { return lastGainmap_; }
    uint32_t lastGainmapWidth() const { return lastGainmapWidth_; }
    uint32_t lastGainmapHeight() const { return lastGainmapHeight_; }
    // Wall-clock demosaic cost for the current output: prepare() tiled
    // demosaic plus render() resample/distortion pre-work. Each stage
    // contributes only the work it actually performed (0 on cache hit).
    double lastDemosaicMs() const { return lastDemosaicMs_; }
    double lastCaptureSharpenMs() const { return lastCaptureSharpenMs_; }
    bool lastCaptureSharpenCached() const { return lastCaptureSharpenCached_; }
    std::atomic<bool> cancelled{false};
    std::atomic<int> progress{0};
    // 0 idle, 1 proxy decode, 2 Bayer bin, 3 demosaic, 4 resize, 5 post/tonemap.
    std::atomic<int> stage{0};

   private:
    void check();
    void renderWaveform(uint32_t width, uint32_t height, uint32_t orientation);
    std::unique_ptr<image_scopes::ImageScopes> scopeEngine_;
    std::unique_ptr<rawr::capsharp::CaptureSharpening> captureSharpening_;
    vulkan::OwnedImage scopeImage_{};
    std::vector<uint32_t> waveformPixels_;
    std::string filesDir_;
    vulkan::VulkanContext vulkan_;
    std::mutex& queueMutex_ = vulkan_.primaryQueue().mutex();
    develop::rendered::StillImageRenderer processor_;
    // Pristine linear overview. PostDemosaicProcessor writes its input, so
    // render() copies this image before handing it to the still pipeline.
    vulkan::OwnedImage overviewInput_{};
    vulkan::OwnedImage overviewWorking_{};
    std::string overviewWorkingKey_;
    std::string overviewInputKey_;
    std::string linearResultKey_;
    develop::rendered::RenderedStillCompletion lastCompletion_{};
    std::vector<uint8_t> lastGainmap_;
    uint32_t lastGainmapWidth_ = 0;
    uint32_t lastGainmapHeight_ = 0;
    double lastDemosaicMs_ = 0.0;
    double lastCaptureSharpenMs_ = 0.0;
    bool lastCaptureSharpenCached_ = false;
};
}  // namespace rawrcam::renderer
