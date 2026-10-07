#include "encoding/rzsl/RzslBundleSink.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "rawr/zsl_container/ZslContainer.h"

namespace rawrcam::encoding::rzsl {
namespace {

template <typename T>
void writeOptional(std::ostream& out, const char* key, const std::optional<T>& value) {
    out << key << '\t';
    if (value) out << *value;
    out << '\n';
}

void writeRect(std::ostream& out, const char* key, const rawrcam::metadata::RectI& rect) {
    out << key << '\t' << (rect.valid ? 1 : 0) << ',' << rect.left << ',' << rect.top << ',' << rect.right << ','
        << rect.bottom << '\n';
}

void writeMatrix(std::ostream& out, const char* key, const rawrcam::metadata::Matrix3x3& matrix) {
    out << key << '\t' << (matrix.valid ? 1 : 0);
    for (float value : matrix.rowMajor) out << ',' << value;
    out << '\n';
}

void writeFloatArray4(std::ostream& out, const char* key, const std::array<float, 4>& values) {
    out << key << '\t' << values[0] << ',' << values[1] << ',' << values[2] << ',' << values[3] << '\n';
}

}  // namespace

std::string RzslBundleSink::serializeMetadata(std::uint64_t zslFrameId,
                                              const rawrcam::metadata::FrameMetadataSnapshot& m) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "zslFrameId\t" << zslFrameId << '\n'
        << "frameOrdinal\t" << m.frameOrdinal << '\n'
        << "timestampNs\t" << m.timestampNs << '\n'
        << "exposureTimeNs\t" << m.exposureTimeNs << '\n'
        << "sensitivity\t" << m.sensitivity << '\n'
        << "staticWhiteLevel\t" << m.staticWhiteLevel << '\n'
        << "reportedDynamicWhiteLevel\t";
    if (m.reportedDynamicWhiteLevel) out << *m.reportedDynamicWhiteLevel;
    out << '\n'
        << "effectiveWhiteLevel\t" << m.effectiveWhiteLevel << '\n'
        << "effectiveWhiteLevelSource\t"
        << rawrcam::metadata::effectiveWhiteLevelSourceName(m.effectiveWhiteLevelSource) << '\n';
    writeOptional(out, "forcedSensorModeId", m.forcedSensorModeId);
    writeFloatArray4(out, "blackLevelPhysicalRggb", m.blackLevelPhysicalRggb);
    writeFloatArray4(out, "colorCorrectionGainsRggb", m.colorCorrectionGainsRggb);
    out << "neutralColorPoint\t" << (m.hasNeutralColorPoint ? 1 : 0) << ',' << m.neutralColorPoint[0] << ','
        << m.neutralColorPoint[1] << ',' << m.neutralColorPoint[2] << '\n';
    writeMatrix(out, "colorCorrectionTransform", m.colorCorrectionTransform);
    out << "colorCorrectionMode\t" << m.colorCorrectionMode << '\n'
        << "awbMode\t" << m.awbMode << '\n'
        << "aeMode\t" << m.aeMode << '\n'
        << "awbState\t" << m.awbState << '\n';
    writeRect(out, "scalerCropRegion", m.scalerCropRegion);
    writeRect(out, "rawCropRegion", m.rawCropRegion);
    writeOptional(out, "requestedExposureTimeNs", m.requestedExposureTimeNs);
    writeOptional(out, "requestedSensitivity", m.requestedSensitivity);
    out << "postRawSensitivityBoost\t" << m.postRawSensitivityBoost << '\n';
    // Non-zero on HDR+ bracket dark frames (post-shutter one-shot requests).
    writeOptional(out, "optimizedStillRequestId", m.optimizedStillRequestId);
    writeOptional(out, "aperture", m.aperture);
    writeOptional(out, "focalLengthMm", m.focalLengthMm);
    writeOptional(out, "focusDistanceDiopters", m.focusDistanceDiopters);
    out << "sensorNoiseProfile";
    for (double value : m.sensorNoiseProfile) out << '\t' << value;
    out << '\n'
        << "lensShadingMapSize\t" << m.lensShadingMapWidth << ',' << m.lensShadingMapHeight << '\n'
        << "lensShadingMap";
    for (float value : m.lensShadingMap) out << '\t' << value;
    out << '\n' << "hotPixelMap";
    for (std::int32_t value : m.hotPixelMap) out << '\t' << value;
    out << '\n';

    if (m.cameraContext) {
        const auto& c = *m.cameraContext;
        out << "cameraContextGeneration\t" << c.cameraContextGeneration << '\n'
            << "cameraId\t" << c.cameraId << '\n'
            << "lensId\t" << c.lensId << '\n'
            << "rawPreviewCfa\t" << c.rawPreviewCfa << '\n'
            << "camera2Cfa\t" << c.camera2Cfa << '\n'
            << "lensFacing\t" << c.lensFacing << '\n'
            << "baselineWhiteLevel\t" << c.baselineWhiteLevel << '\n'
            << "sensorPhysicalSizeMm\t" << c.sensorPhysicalSizeMm[0] << ',' << c.sensorPhysicalSizeMm[1] << '\n'
            << "geometryRawBuffer\t" << c.geometry.rawBufferWidth << ',' << c.geometry.rawBufferHeight << '\n'
            << "geometryPixelArray\t" << c.geometry.pixelArrayWidth << ',' << c.geometry.pixelArrayHeight << '\n'
            << "sensorOrientationDegrees\t" << c.geometry.sensorOrientationDegrees << '\n';
        writeRect(out, "preCorrectionActiveArray", c.geometry.preCorrectionActiveArray);
        writeRect(out, "activeArray", c.geometry.activeArray);
        writeFloatArray4(out, "baselineBlackLevelPhysicalRggb", c.baselineBlackLevelPhysicalRggb);
        out << "referenceIlluminant1\t" << c.color.referenceIlluminant1 << '\n'
            << "referenceIlluminant2\t" << c.color.referenceIlluminant2 << '\n';
        writeMatrix(out, "colorTransform1", c.color.colorTransform1);
        writeMatrix(out, "colorTransform2", c.color.colorTransform2);
        writeMatrix(out, "calibrationTransform1", c.color.calibrationTransform1);
        writeMatrix(out, "calibrationTransform2", c.color.calibrationTransform2);
        writeMatrix(out, "forwardMatrix1", c.color.forwardMatrix1);
        writeMatrix(out, "forwardMatrix2", c.color.forwardMatrix2);
        out << "cameraContextLensShadingMapSize\t" << c.lensShadingMapWidth << ',' << c.lensShadingMapHeight << '\n';
        out << "availableFocalLengthsMm";
        for (float value : c.availableFocalLengthsMm) out << '\t' << value;
        out << '\n' << "availableApertures";
        for (float value : c.availableApertures) out << '\t' << value;
        out << '\n';
    }
    return out.str();
}

RzslBundleSink::BundleInfo RzslBundleSink::writeBundle(
    const std::string& filesDir, std::uint32_t cfa, const std::vector<Frame>& frames,
    const std::vector<std::string>& serializedMetadata,
    std::function<bool(std::uint64_t, std::vector<std::uint8_t>&)> readPacket) {
    if (frames.empty() || frames.size() != serializedMetadata.size()) {
        throw std::invalid_argument("RZSL bundle frame/metadata mismatch");
    }
    std::vector<rawr::zsl_container::PackedFrame> containerFrames;
    containerFrames.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        containerFrames.push_back(rawr::zsl_container::PackedFrame{f.frameId,
                                                                   f.timestampNs,
                                                                   f.width,
                                                                   f.height,
                                                                   f.tilesX,
                                                                   f.tilesY,
                                                                   f.streams,
                                                                   f.tableBytes,
                                                                   f.payloadBytes,
                                                                   {},
                                                                   serializedMetadata[i]});
    }
    const std::filesystem::path finalPath = std::filesystem::path(filesDir) / "rawrcam_zsl_bundle_latest.rzsl";
    const std::filesystem::path temporaryPath = std::filesystem::path(filesDir) / "rawrcam_zsl_bundle_latest.tmp";
    const std::filesystem::path readyPath = std::filesystem::path(filesDir) / "rawrcam_zsl_bundle_latest.ready";
    std::error_code ec;
    std::filesystem::remove(temporaryPath, ec);
    std::filesystem::remove(finalPath, ec);
    std::filesystem::remove(readyPath, ec);
    const auto info = rawr::zsl_container::writeBundleStreaming(
        temporaryPath.string(), cfa, containerFrames,
        [&readPacket](std::uint64_t frameId, std::vector<std::uint8_t>& out) { return readPacket(frameId, out); });
    std::filesystem::rename(temporaryPath, finalPath);
    std::ofstream ready(readyPath, std::ios::trunc);
    ready << info.frameCount << '\t' << info.bytesWritten << '\n';
    if (!ready) throw std::runtime_error("RZSL ready marker failed");
    return BundleInfo{info.frameCount, info.bytesWritten};
}

}  // namespace rawrcam::encoding::rzsl
