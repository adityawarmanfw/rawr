#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "encoding/jpeg/JpegCaptureContext.h"

namespace rawrcam::encoding::jpeg {

struct JpegWriteCompletion {
    std::uint64_t requestId = 0;
    bool success = false;
    std::string displayName;
    std::string error;
    // Render-only outcome forwarded without encoding: film memory rejection
    // requires a DNG substitution. success remains false; no JPEG was written.
    bool filmFallbackMemory = false;
    double encodeMs = 0.0;
    double fsyncMs = 0.0;
    std::uint64_t fileBytes = 0;
};

// Asynchronous libjpeg-turbo writer for already-tonemapped RGBA8 pixels.
// Pixel memory remains caller-owned and must remain stable until completion.
class JpegCaptureWriter final {
   public:
    JpegCaptureWriter() = default;
    ~JpegCaptureWriter();

    JpegCaptureWriter(const JpegCaptureWriter&) = delete;
    JpegCaptureWriter& operator=(const JpegCaptureWriter&) = delete;

    bool start(std::uint64_t requestId, const void* rgba8, std::size_t rgbaBytes, std::uint32_t width,
               std::uint32_t height, JpegCaptureContext context);
    // UltraHDR path: mapRgba8 contains the RGBA8 recovery map, optionally
    // followed by an equally sized packed capture-chromaticity plane (see
    // GainmapParams::writeHdrChromaticity). mapBytes includes both planes.
    // mapWidth/mapHeight are the logical recovery-map dimensions. Requires
    // context.ultraHdr.enabled; otherwise behaves like start(). If the gain
    // map encode or mux fails, degrades to a legacy SDR file from the base
    // pixels (logged as JPEG_UHDR_FALLBACK) instead of failing the capture.
    bool startUltraHdr(std::uint64_t requestId, const void* rgba8, std::size_t rgbaBytes, std::uint32_t width,
                       std::uint32_t height, const void* mapRgba8, std::size_t mapBytes, std::uint32_t mapWidth,
                       std::uint32_t mapHeight, JpegCaptureContext context);
    std::optional<JpegWriteCompletion> pollCompletion();
    bool busy() const noexcept;
    void reset() noexcept;

   private:
    void joinWorker();
    mutable std::mutex mutex_;
    std::thread worker_;
    bool busy_ = false;
    std::optional<JpegWriteCompletion> completion_;
};

}  // namespace rawrcam::encoding::jpeg
