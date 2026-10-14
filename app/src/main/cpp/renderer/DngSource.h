#pragma once
#include <tinydng.h>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rawrcam::renderer {
// Pixel coordinates always refer to the uncropped, unrotated sensor raster.
class DngSource {
   public:
    explicit DngSource(const std::string& path);
    ~DngSource();
    DngSource(const DngSource&) = delete;
    DngSource& operator=(const DngSource&) = delete;
    const tinydng_image_info& info() const { return *image_; }
    std::vector<uint16_t> read(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    std::vector<float> normalized(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool shading);
    // normalized() for one sensor sample, given its read() value: callers
    // that sample sparse sites skip the full-band arithmetic.
    float normalizedSample(uint16_t raw, uint32_t sx, uint32_t sy, bool shading) const;
    void prepareBinned(const std::string& path, bool shading, const std::function<void()>& check);
    std::vector<float> binnedNormalized(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool shading);
#ifndef NDEBUG
    // An offline replay may replace the pre-shading CFA while retaining all
    // metadata and the remaining production render stages of the source DNG.
    void setReplayCfa(const std::string& path);
#endif
    std::array<uint32_t, 4> crop{};  // x,y,width,height
    std::array<double, 4> black{};
    // DNG NoiseProfile ([S,O] per CFA channel, row-major 2x2) + white level
    // for profiled denoise. Count 0 = absent (denoise resolves to off).
    std::array<double, 8> noiseProfile{};
    uint16_t noiseProfileCount = 0;
    double whiteLevel = 0.0;
    std::array<float, 3> wb{1, 1, 1};
    std::array<float, 9> cameraToSrgb{};  // row-major, applied after wb
    uint32_t cfa = 0;
    uint32_t orientation = 1;
    std::string provenance;
    bool rawr = false;
    // Capture EXIF carried by the source DNG, when present. ExposureTime and
    // ISOSpeedRatings are standard tags; both are optional here so tagless
    // files still render (the JPEG writer omits absent tags, as on stills).
    // ApertureValue is APEX; converted to f-number on read.
    std::optional<std::int64_t> exposureTimeNs() const;
    std::optional<std::int32_t> sensitivity() const;
    std::optional<float> aperture() const;
    size_t memoryPeak() const { return tinydng_context_memory_peak(context_); }

   private:
    const float* binCache_ = nullptr;
    size_t binCacheBytes_ = 0;
    std::string binCachePath_;
    tinydng_context* context_ = nullptr;
    tinydng_document* document_ = nullptr;
    const tinydng_image_info* image_ = nullptr;
    size_t index_ = 0;
    bool rawCacheRequired_ = false;
    std::string rawCachePath_;
    const uint16_t* rawCache_ = nullptr;
    size_t rawCacheBytes_ = 0;
    // read() may run on several binning workers: guards the lazy raw-cache
    // build and tinydng region decodes.
    std::mutex decodeMutex_;
    void ensureRawCache();
#ifndef NDEBUG
    std::vector<float> replayCfa_;
#endif
};
}  // namespace rawrcam::renderer
