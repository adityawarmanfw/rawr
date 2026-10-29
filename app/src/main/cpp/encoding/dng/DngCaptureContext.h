#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "encoding/OutputFd.h"
#include "metadata/GpsMetadata.h"

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
    // Owned destination; moves to DngCaptureWriter, which closes it on every outcome.
    OutputFd outputFd;
    int deviceRotationDegrees = 0;    // physical device rotation: 0/90/180/270
    int64_t wallClockUnixMillis = 0;  // shutter-request wall clock
    int16_t utcOffsetMinutes = 0;
    std::string deviceMake;
    std::string deviceModel;  // Stable hardware model for UniqueCameraModel.
    std::string deviceDisplayModel;  // Optional friendly EXIF Model, frozen at capture.
    std::string displayName;
    std::optional<tinydng_gps> gps;
    std::shared_ptr<const tonemap::TonemapParams> captureTone;
    std::shared_ptr<const spektrafilm_native::FilmLook> captureFilm;
    bool captureFilmEnabled = false;
    std::string processingRecipe;  // Versioned named-field JSON; independent of whether JPEG is requested.
    std::string sourceRole = "single";
    std::string resolvedRecipe;               // Frame-time native settings, preserved for replay.
    float autoKeyEv = 0.0f;                   // Shutter-time key lift; rendering uses captureTone.adaptiveTone.
    // Per-frame color state for replay, rendered by
    // capture::persistence::attachFrameRecipes before the writer starts. The
    // encoder embeds every recipe string verbatim and never parses it.
    std::string frameRecipe;
    std::string imageDescription;             // frozen human-readable capture/render settings
    std::optional<float> baselineExposureEV;  // DNG render hint; RAW samples are unchanged.
    // Merged DNGs: {shot, read} variance scale applied to the reference
    // frame's NoiseProfile (S, O), so raw developers see the merge's noise
    // rather than a single frame's.
    std::optional<std::array<double, 2>> noiseVarianceScale;
    bool reconstructedGeometry = false;       // merged CFA is a synthesized output grid, not sensor-pixel geometry
    // The app's own lens-shading decision for this capture (develop setting
    // lensShadingCorrectionEnabled). It no longer gates the DNG: the stored CFA
    // never has shading baked in, so the Camera2 map is always written as
    // GainMap opcodes. Kept for provenance (whether the capture's JPEG applied
    // it). Derived from the JPEG develop settings, not persisted.
    bool appLensShadingCorrection = false;
    // The app's distortion decision (develop setting distortionCorrectionEnabled):
    // a lens profile calibration goes in as a WarpRectilinear opcode exactly
    // when the app's render undistorts. Not persisted, like the shading flag.
    bool appDistortionCorrection = false;
    // DNG payload encoding. LosslessJpeg is the default; Uncompressed keeps
    // the legacy byte layout. Wired to the capture settings UI (follow-up);
    // JNI still constructs the default until then.
    DngCompression compression = DngCompression::LosslessJpeg;
};
}  // namespace rawrcam::encoding::dng
