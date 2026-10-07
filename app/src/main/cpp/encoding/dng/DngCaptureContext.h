#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace tonemap {
struct TonemapParams;
}
namespace spektrafilm_native {
struct FilmLook;
}

namespace rawrcam::encoding::dng {
enum class DngCompression : std::uint8_t {
    LosslessJpeg = 0,  // DNG Compression=7, default: ~40-60% smaller files.
    Uncompressed = 1,  // DNG Compression=1, legacy layout.
};

struct DngCaptureContext {
    int outputFd = -1;                // ownership transfers to DngCaptureWriter on accepted request
    int deviceRotationDegrees = 0;    // physical device rotation: 0/90/180/270
    int64_t wallClockUnixMillis = 0;  // shutter-request wall clock
    int16_t utcOffsetMinutes = 0;
    std::string deviceMake;
    std::string deviceModel;  // Stable hardware model for UniqueCameraModel.
    std::string deviceDisplayModel;  // Optional friendly EXIF Model, frozen at capture.
    std::string displayName;
    std::shared_ptr<const tonemap::TonemapParams> captureTone;
    std::shared_ptr<const spektrafilm_native::FilmLook> captureFilm;
    bool captureFilmEnabled = false;
    std::string processingRecipe;  // Versioned named-field JSON; independent of whether JPEG is requested.
    std::string sourceRole = "single";
    std::string resolvedRecipe;               // Frame-time native settings, preserved for replay.
    // Per-frame color state for replay, rendered by
    // capture::persistence::attachFrameRecipes before the writer starts. The
    // encoder embeds every recipe string verbatim and never parses it.
    std::string frameRecipe;
    std::string imageDescription;             // frozen human-readable capture/render settings
    std::optional<float> baselineExposureEV;  // DNG render hint; RAW samples are unchanged.
    bool reconstructedGeometry = false;       // merged CFA is a synthesized output grid, not sensor-pixel geometry
    // DNG payload encoding. LosslessJpeg is the default; Uncompressed keeps
    // the legacy byte layout. Wired to the capture settings UI (follow-up);
    // JNI still constructs the default until then.
    DngCompression compression = DngCompression::LosslessJpeg;
};
}  // namespace rawrcam::encoding::dng
