#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "metadata/CameraContextMetadata.h"

namespace rawrcam::metadata {

inline constexpr uint64_t kSensitivityProbeRequestId = ~uint64_t{0};

enum class EffectiveWhiteLevelSource : std::uint8_t {
    StaticCharacteristics = 0,
    ReportedDynamic = 1,
    ProfileStatic = 2,  // Lens profile static levels (vendor sensor modes, DCG).
};

inline const char* effectiveWhiteLevelSourceName(EffectiveWhiteLevelSource source) noexcept {
    switch (source) {
        case EffectiveWhiteLevelSource::StaticCharacteristics:
            return "static_characteristics";
        case EffectiveWhiteLevelSource::ReportedDynamic:
            return "reported_dynamic";
        case EffectiveWhiteLevelSource::ProfileStatic:
            return "profile_static";
    }
    return "unknown";
}

// Authoritative metadata snapshot for exactly one delivered RAW frame.
// The shared cameraContext pins the immutable camera state that produced this
// frame, so later asynchronous consumers never consult mutable live state.
struct FrameMetadataSnapshot {
    // Transient startup routing only; deliberately omitted from capture journals.
    bool suppressPreview = false;
    CameraContextMetadataPtr cameraContext;
    uint64_t frameOrdinal = 0;
    uint64_t timestampNs = 0;  // SENSOR_TIMESTAMP; AImage/CaptureResult pairing key.

    std::array<float, 4> blackLevelPhysicalRggb{0, 0, 0, 0};

    // Preserve white-level provenance per frame. reportedDynamicWhiteLevel is
    // evidence from Camera2, not an assertion that it is sensor-truth under all
    // operating modes (notably future DCG modes). effectiveWhiteLevel is the
    // processing value actually supplied to raw_preview for this frame.
    float staticWhiteLevel = 0.0f;
    std::optional<float> reportedDynamicWhiteLevel;
    float effectiveWhiteLevel = 0.0f;
    EffectiveWhiteLevelSource effectiveWhiteLevelSource = EffectiveWhiteLevelSource::StaticCharacteristics;
    // No longer populated (static levels now come from lens profiles; see
    // effectiveWhiteLevelSource). Kept so the capture-journal layout is stable.
    std::optional<int32_t> forcedSensorModeId;

    std::array<float, 4> colorCorrectionGainsRggb{1, 1, 1, 1};
    std::array<float, 3> neutralColorPoint{1, 1, 1};
    bool hasNeutralColorPoint = false;
    Matrix3x3 colorCorrectionTransform{};
    int32_t colorCorrectionMode = -1;
    int32_t awbMode = -1;
    int32_t aeMode = -1;
    int32_t aeState = -1;
    int32_t awbState = -1;
    int32_t postRawSensitivityBoost = 100;  // Camera2 value; 100 == 1x.

    RectI scalerCropRegion{};
    RectI rawCropRegion{};
    int64_t exposureTimeNs = 0;
    int32_t sensitivity = 0;  // CaptureResult reported sensitivity.

    // Exact settings from the framework-owned copy of ACaptureRequest that
    // produced this frame. These are deliberately separate from CaptureResult
    // values because forced high-bit/DCG modes report a different sensitivity
    // coordinate.
    std::optional<int64_t> requestedExposureTimeNs;
    std::optional<int32_t> requestedSensitivity;
    // Non-zero only for the tagged one-shot request issued for this app capture.
    std::optional<uint64_t> optimizedStillRequestId;
    std::optional<float> aperture;
    std::optional<float> focalLengthMm;
    std::optional<float> focusDistanceDiopters;
    // Capture-result flash state (ACAMERA_FLASH_STATE). -1 = unknown.
    int32_t flashState = -1;
    // Hot-pixel map from STATISTICS_HOT_PIXEL_MAP as flat [x0,y0,x1,y1,...]
    // in active-array pixels. Empty = unknown / not requested. Capped at read.
    std::vector<int32_t> hotPixelMap;
    // Camera2 SENSOR_NOISE_PROFILE order: [S,O] for each CFA channel in row-major 2x2 order.
    std::vector<double> sensorNoiseProfile;
    // Camera2 interleaved [R,G_even,G_odd,B], row-major grid. Present only when requested.
    uint32_t lensShadingMapWidth = 0;
    uint32_t lensShadingMapHeight = 0;
    std::vector<float> lensShadingMap;
};

}  // namespace rawrcam::metadata
