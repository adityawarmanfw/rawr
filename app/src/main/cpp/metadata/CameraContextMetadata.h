#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "geometry/LensDistortion.h"
#include "geometry/RawGeometry.h"
#include "metadata/MetadataTypes.h"

namespace rawrcam::color {
struct DcpProfile;
}

namespace rawrcam::metadata {

struct StaticColorCalibration {
    int32_t referenceIlluminant1 = -1;
    int32_t referenceIlluminant2 = -1;
    Matrix3x3 colorTransform1{};
    Matrix3x3 colorTransform2{};
    Matrix3x3 calibrationTransform1{};
    Matrix3x3 calibrationTransform2{};
    Matrix3x3 forwardMatrix1{};
    Matrix3x3 forwardMatrix2{};
};

// Immutable metadata for one opened camera context. Created once per context
// generation, then pinned by every frame captured from that context.
struct CameraContextMetadata {
    uint64_t cameraContextGeneration = 0;
    // Profile ID travels with captured frames so rendering can apply narrowly scoped device policies.
    std::string cameraProfileId;
    std::string cameraId;
    std::string lensId;
    SensorGeometry geometry{};
    // Layout of the configured RAW stream; downstream consumers always see
    // unpacked RAW16 after ingress, but CPU readers must decode RAW10 groups.
    geometry::RawPixelFormat rawFormat = geometry::RawPixelFormat::Raw16;
    // The lens profile's RAW reader usage (LensRoute::rawBufferUsage), if any.
    std::optional<uint64_t> rawReaderUsage;
    // The lens profile's viewfinder reader tuning (LensRoute::rawBufferCount, rawBufferTestFirst, rawBufferFallBack).
    uint32_t rawBufferCount = 0;
    bool rawBufferTestFirst = false;
    bool rawBufferFallBack = false;
    // The lens profile's capture-stream reader usage (LensRoute::photoBufferUsage), if any.
    std::optional<uint64_t> photoBufferUsage;
    uint32_t photoBufferCount = 0;  // 0 keeps the reader default.
    // Recipe capture streams require a measured viewfinder floor before borrowing exposure.
    bool guardStillExposure = false;
    uint32_t rawPreviewCfa = 0;  // RGGB=0, GRBG=1, GBRG=2, BGGR=3.
    int32_t camera2Cfa = -1;
    // Side of the same-colour blocks in the camera's RAW (LensRoute::cfaBlock):
    // >1 means ingress remosaics each frame to Bayer in the rawPreviewCfa
    // order before anything else reads it. Never persisted: stored frames
    // are already Bayer.
    uint32_t cfaBlock = 1;
    uint32_t photoCfaBlock = 1;  // CFA of the separate photo output before remosaic.
    // Raw columns/rows the HAL pads with no image (LensRoute::crop*). All four
    // are multiples of the CFA period, so the kept area keeps the camera's own
    // colour order. Zeroed on a frame whose edges have already been dropped.
    uint32_t cropLeft = 0;
    uint32_t cropTop = 0;
    uint32_t cropRight = 0;
    uint32_t cropBottom = 0;
    // LensRoute::highFpsCrop*: padded edges of the >30 fps sensor mode.
    uint32_t highFpsCropLeft = 0;
    uint32_t highFpsCropTop = 0;
    uint32_t highFpsCropRight = 0;
    uint32_t highFpsCropBottom = 0;
    // Photo frames are reduced to a quarter of the pixels at ingress (LensRoute::photoReduced); only with
    // photoCfaBlock 4. Reduced frames are Bayer in quarter-DN units: levels are scaled by 4 (reduction x4 sum).
    bool photoReduced = false;
    bool photoReduced25 = false;  // smaller reduced size: Bayer 3x bin (22 MP), 4x4 resampled to 25 MP (LensRoute::photoReduced25)
    bool photoPhaseFit = false;
    // Photo contexts pin their original base-stream layout for a HAL binned-corner fallback.
    // Transient only: journaled pixels have already been cropped and remosaiced to Bayer.
    std::shared_ptr<const CameraContextMetadata> binnedFallbackContext;
    // imaging::CfaRemosaicQuality for live frames (ingress) and single stills.
    uint32_t cfaLiveQuality = 1;
    uint32_t cfaPhotoQuality = 2;
    int32_t lensFacing = -1;
    std::array<float, 4> baselineBlackLevelPhysicalRggb{0, 0, 0, 0};
    float baselineWhiteLevel = 0.0f;
    StaticColorCalibration color{};
    std::array<float, 2> sensorPhysicalSizeMm{0.0f, 0.0f};
    std::vector<float> availableFocalLengthsMm;
    std::vector<float> availableApertures;
    uint32_t lensShadingMapWidth = 0;
    uint32_t lensShadingMapHeight = 0;
    // Maximum Camera2 processed-output boost. RAW samples never contain this gain;
    // preview rendering reproduces the per-frame CaptureResult value explicitly.
    int32_t maxPostRawSensitivityBoost = 100;
    // Pure-metadata probe fields. All optional; absent = unknown / omit from DNG.
    bool hasLensShadingApplied = false;
    bool lensShadingApplied = false;
    int32_t maxAnalogSensitivity = -1;
    bool hasGreenSplit = false;
    float greenSplit = 0.0f;
    std::vector<RectI> opticalBlackRegions;
    std::vector<float> lensDistortion;            // float[5] [k1,k2,k3,p1,p2], empty = unknown
    std::vector<float> lensIntrinsicCalibration;  // float[5] [fx,fy,cx,cy,s], empty = unknown
    // Lens profile calibration (LensRoute::distortion) for a camera that
    // reports none; wins over lensDistortion for photo undistort and the DNG.
    std::optional<geometry::LensDistortionModel> profileDistortion;
    // Lens profile mirror (LensRoute::mirror, geometry::kLensMirror* bits).
    uint32_t profileMirror = 0;
    // Lens profile camera profile (LensRoute::dcp). When set, `color` holds
    // its calibration (color::dcpColorCalibration) instead of the camera's,
    // and the DNG embeds the profile's own tags.
    std::shared_ptr<const color::DcpProfile> dcpProfile;
    bool hasFlashInfoAvailable = false;
    bool flashInfoAvailable = false;
    // SENSOR_INFO_TIMESTAMP_SOURCE: REALTIME stamps frames in CLOCK_BOOTTIME,
    // UNKNOWN in CLOCK_MONOTONIC.
    bool sensorTimestampRealtime = false;
};

using CameraContextMetadataPtr = std::shared_ptr<const CameraContextMetadata>;

}  // namespace rawrcam::metadata
