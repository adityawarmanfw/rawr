#include "camera/CameraDiscovery.h"

#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <media/NdkImage.h>

#include <algorithm>
#include <optional>
#include <set>
#include <sstream>

namespace rawrcam::camera {
namespace {

std::optional<ACameraMetadata_const_entry> entry(const ACameraMetadata* metadata, uint32_t tag) {
    if (!metadata) return std::nullopt;
    ACameraMetadata_const_entry value{};
    if (ACameraMetadata_getConstEntry(metadata, tag, &value) != ACAMERA_OK) return std::nullopt;
    return value;
}

std::string formatRawSizeList(const ACameraMetadata* characteristics) {
    const auto e = entry(characteristics, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
    if (!e || !e->data.i32 || e->count < 4) return {};

    std::ostringstream out;
    bool first = true;
    for (uint32_t i = 0; i + 3 < e->count; i += 4) {
        const int32_t format = e->data.i32[i];
        if ((format != AIMAGE_FORMAT_RAW16 && format != AIMAGE_FORMAT_RAW10) ||
            e->data.i32[i + 3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) {
            continue;
        }
        if (!first) out << ',';
        first = false;
        out << e->data.i32[i + 1] << 'x' << e->data.i32[i + 2] << (format == AIMAGE_FORMAT_RAW10 ? ":RAW10" : "");
    }
    return out.str();
}

}  // namespace

std::set<std::string> enumeratedCameraIds(ACameraManager* manager) {
    if (!manager) return {};
    ACameraIdList* list = nullptr;
    std::set<std::string> enumerated;
    if (ACameraManager_getCameraIdList(manager, &list) == ACAMERA_OK && list) {
        for (int i = 0; i < list->numCameras; ++i) {
            if (list->cameraIds[i]) enumerated.insert(list->cameraIds[i]);
        }
    }
    if (list) ACameraManager_deleteCameraIdList(list);
    return enumerated;
}
namespace {
std::vector<std::string> discoveryCandidates(const std::set<std::string>& enumerated) {
    std::vector<std::string> ids;
    for (int id = 0; id <= 11; ++id) ids.push_back(std::to_string(id));
    for (const auto& id : enumerated) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    return ids;
}
}  // namespace

std::vector<std::string> cameraDiscoveryCandidates(ACameraManager* manager) {
    return discoveryCandidates(enumeratedCameraIds(manager));
}

void emitCameraDiscovery(ACameraManager* manager, const CameraDiagnostic& diagnostic) {
    if (!manager || !diagnostic) return;
    const auto enumerated = enumeratedCameraIds(manager);
    for (const auto& cameraId : discoveryCandidates(enumerated)) {
        ACameraMetadata* characteristics = nullptr;
        if (ACameraManager_getCameraCharacteristics(manager, cameraId.c_str(), &characteristics) != ACAMERA_OK ||
            !characteristics) {
            continue;
        }

        const auto facing = entry(characteristics, ACAMERA_LENS_FACING);
        const auto focal = entry(characteristics, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
        const auto pixel = entry(characteristics, ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE);

        std::ostringstream line;
        line << "CAMERA_NDK_DISCOVERY cameraId=" << cameraId
             << " enumerated=" << (enumerated.count(cameraId) ? "true" : "false")
             << " facing=" << (facing && facing->count ? static_cast<int>(facing->data.u8[0]) : -1) << " focalLengths=";
        if (focal && focal->data.f) {
            for (uint32_t i = 0; i < focal->count; ++i) {
                if (i) line << ',';
                line << focal->data.f[i];
            }
        }
        line << " rawSizes=" << formatRawSizeList(characteristics);
        if (pixel && pixel->data.i32 && pixel->count >= 2) {
            line << " pixelArray=" << pixel->data.i32[0] << 'x' << pixel->data.i32[1];
        }
        diagnostic(line.str());
        ACameraMetadata_free(characteristics);
    }
}

std::vector<geometry::RawStreamOption> rawOutputStreams(const ACameraMetadata* characteristics) {
    std::vector<geometry::RawStreamOption> out;
    const auto e = entry(characteristics, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
    if (!e || !e->data.i32) return out;

    for (uint32_t i = 0; i + 3 < e->count; i += 4) {
        if (e->data.i32[i + 3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) continue;
        const int32_t format = e->data.i32[i];
        if (format != AIMAGE_FORMAT_RAW16 && format != AIMAGE_FORMAT_RAW10) continue;
        if (e->data.i32[i + 1] <= 0 || e->data.i32[i + 2] <= 0) continue;
        out.push_back(
            {format == AIMAGE_FORMAT_RAW10 ? geometry::RawPixelFormat::Raw10 : geometry::RawPixelFormat::Raw16,
             static_cast<uint32_t>(e->data.i32[i + 1]), static_cast<uint32_t>(e->data.i32[i + 2])});
    }
    return out;
}

std::pair<int32_t, int32_t> smallestYuvOutput(const ACameraMetadata* characteristics, int32_t minWidth,
                                              int32_t minHeight) {
    std::pair<int32_t, int32_t> best{0, 0};
    const auto e = entry(characteristics, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
    if (!e || !e->data.i32) return best;
    for (uint32_t i = 0; i + 3 < e->count; i += 4) {
        if (e->data.i32[i] != AIMAGE_FORMAT_YUV_420_888 ||
            e->data.i32[i + 3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)
            continue;
        const int32_t w = e->data.i32[i + 1], h = e->data.i32[i + 2];
        if (w < minWidth || h < minHeight) continue;
        if (best.first == 0 || int64_t(w) * h < int64_t(best.first) * best.second) best = {w, h};
    }
    return best;
}

}  // namespace rawrcam::camera
