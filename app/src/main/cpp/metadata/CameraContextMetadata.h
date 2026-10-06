#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "geometry/RawGeometry.h"
#include "metadata/MetadataTypes.h"

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
    std::string cameraId;
    std::string lensId;
    SensorGeometry geometry{};
    // Layout of the configured RAW stream; downstream consumers always see
    // unpacked RAW16 after ingress, but CPU readers must decode RAW10 groups.
    geometry::RawPixelFormat rawFormat = geometry::RawPixelFormat::Raw16;
    uint32_t rawPreviewCfa = 0;  // RGGB=0, GRBG=1, GBRG=2, BGGR=3.
    int32_t camera2Cfa = -1;
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
    std::vector<float> lensDistortion;            // float[5] [k1,k2,p1,p2,k3], empty = unknown
    std::vector<float> lensIntrinsicCalibration;  // float[5] [fx,fy,cx,cy,s], empty = unknown
    bool hasFlashInfoAvailable = false;
    bool flashInfoAvailable = false;
    // SENSOR_INFO_TIMESTAMP_SOURCE: REALTIME stamps frames in CLOCK_BOOTTIME,
    // UNKNOWN in CLOCK_MONOTONIC.
    bool sensorTimestampRealtime = false;
};

using CameraContextMetadataPtr = std::shared_ptr<const CameraContextMetadata>;

}  // namespace rawrcam::metadata
