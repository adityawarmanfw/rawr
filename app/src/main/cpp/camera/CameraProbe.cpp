#include "camera/CameraProbe.h"

#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <media/NdkImage.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>

#include "camera/CameraDiscovery.h"
#include "camera/CameraProfileJson.h"
#include "camera/CameraMetadataTags.h"
#include "geometry/RawGeometry.h"

namespace rawrcam::camera {
namespace {

using ManagerPtr = std::unique_ptr<ACameraManager, decltype(&ACameraManager_delete)>;
using MetadataPtr = std::unique_ptr<ACameraMetadata, decltype(&ACameraMetadata_free)>;

std::optional<ACameraMetadata_const_entry> entry(const ACameraMetadata* metadata, uint32_t tag) {
    ACameraMetadata_const_entry value{};
    if (!metadata || ACameraMetadata_getConstEntry(metadata, tag, &value) != ACAMERA_OK || !value.count)
        return std::nullopt;
    return value;
}

MetadataPtr characteristics(ACameraManager* manager, const std::string& id) {
    ACameraMetadata* raw = nullptr;
    if (ACameraManager_getCameraCharacteristics(manager, id.c_str(), &raw) != ACAMERA_OK) raw = nullptr;
    return {raw, ACameraMetadata_free};
}

void appendNumber(std::string& out, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.8g", v);
    out += buf;
}

template <class T>
void appendNumbers(std::string& out, const T* values, uint32_t count) {
    out += '[';
    for (uint32_t i = 0; values && i < count; ++i) {
        if (i) out += ',';
        appendNumber(out, double(values[i]));
    }
    out += ']';
}

const char* typeName(uint8_t type) {
    switch (type) {
        case ACAMERA_TYPE_BYTE:
            return "byte";
        case ACAMERA_TYPE_INT32:
            return "int32";
        case ACAMERA_TYPE_FLOAT:
            return "float";
        case ACAMERA_TYPE_INT64:
            return "int64";
        case ACAMERA_TYPE_DOUBLE:
            return "double";
        case ACAMERA_TYPE_RATIONAL:
            return "rational";
    }
    return "unknown";
}

// First entries of a metadata value; enough to show a default in the picker.
void appendEntryValues(std::string& out, const ACameraMetadata_const_entry& e) {
    const uint32_t n = std::min<uint32_t>(e.count, 16);
    switch (e.type) {
        case ACAMERA_TYPE_BYTE:
            appendNumbers(out, e.data.u8, n);
            return;
        case ACAMERA_TYPE_INT32:
            appendNumbers(out, e.data.i32, n);
            return;
        case ACAMERA_TYPE_FLOAT:
            appendNumbers(out, e.data.f, n);
            return;
        case ACAMERA_TYPE_INT64:
            appendNumbers(out, e.data.i64, n);
            return;
        case ACAMERA_TYPE_DOUBLE:
            appendNumbers(out, e.data.d, n);
            return;
        case ACAMERA_TYPE_RATIONAL: {
            out += '[';
            for (uint32_t i = 0; i < n; ++i) {
                if (i) out += ',';
                const auto& r = e.data.r[i];
                appendNumber(out, r.denominator ? double(r.numerator) / r.denominator : 0.0);
            }
            out += ']';
            return;
        }
    }
    out += "[]";
}

const char* rawFormatName(int32_t format) {
    switch (format) {
        case AIMAGE_FORMAT_RAW16:
            return "RAW_SENSOR";
        case AIMAGE_FORMAT_RAW10:
            return "RAW10";
        case AIMAGE_FORMAT_RAW12:
            return "RAW12";
        case AIMAGE_FORMAT_RAW_PRIVATE:
            return "RAW_PRIVATE";
    }
    return nullptr;
}

std::vector<std::string> physicalIds(const ACameraMetadata* chars) {
    std::vector<std::string> ids;
    const auto e = entry(chars, ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS);
    if (!e || !e->data.u8) return ids;
    std::string current;
    for (uint32_t i = 0; i < e->count; ++i) {
        const char c = char(e->data.u8[i]);
        if (c == '\0') {
            if (!current.empty()) ids.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) ids.push_back(current);
    return ids;
}

bool hasCapability(const ACameraMetadata* chars, uint8_t capability) {
    const auto e = entry(chars, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES);
    if (!e || !e->data.u8) return false;
    return std::find(e->data.u8, e->data.u8 + e->count, capability) != e->data.u8 + e->count;
}

void appendSessionKeys(std::string& out, const ACameraMetadata* chars) {
    const auto e = entry(chars, ACAMERA_REQUEST_AVAILABLE_SESSION_KEYS);
    out += "\"sessionKeys\":";
    if (e && e->data.i32) {
        out += '[';
        for (uint32_t i = 0; i < e->count; ++i) {
            if (i) out += ',';
            out += std::to_string(uint32_t(e->data.i32[i]));
        }
        out += ']';
    } else {
        out += "[]";
    }
}

void appendCamera(std::string& out, const std::string& id, const std::string& parentId, bool enumerated,
                  const ACameraMetadata* chars) {
    out += "{\"id\":";
    appendJsonString(out, id);
    out += ",\"parentId\":";
    appendJsonString(out, parentId);
    out += std::string(",\"enumerated\":") + (enumerated ? "true" : "false");
    const auto physical = physicalIds(chars);
    out += std::string(",\"logical\":") +
           (hasCapability(chars, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA) ? "true" : "false");
    out += ",\"physicalIds\":[";
    for (size_t i = 0; i < physical.size(); ++i) {
        if (i) out += ',';
        appendJsonString(out, physical[i]);
    }
    out += "],\"facing\":";
    const auto facing = entry(chars, ACAMERA_LENS_FACING);
    out += facing && facing->data.u8 ? std::to_string(int(facing->data.u8[0])) : "-1";

    const auto focal = entry(chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
    out += ",\"focalLengths\":";
    appendNumbers(out, focal ? focal->data.f : static_cast<const float*>(nullptr), focal ? focal->count : 0);
    const auto size = entry(chars, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE);
    out += ",\"sensorSizeMm\":";
    appendNumbers(out, size ? size->data.f : static_cast<const float*>(nullptr), size ? size->count : 0);
    // 35mm-equivalent from the diagonal crop factor (full frame 43.27 mm).
    double equivalent = 0.0;
    if (focal && focal->count && size && size->count >= 2) {
        const double diagonal = std::hypot(size->data.f[0], size->data.f[1]);
        if (diagonal > 0) equivalent = focal->data.f[0] * 43.27 / diagonal;
    }
    out += ",\"equivalentFocalMm\":";
    appendNumber(out, equivalent);

    out += ",\"rawStreams\":[";
    bool first = true;
    if (const auto configs = entry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS); configs && configs->data.i32) {
        for (uint32_t i = 0; i + 3 < configs->count; i += 4) {
            const int32_t format = configs->data.i32[i];
            const char* name = rawFormatName(format);
            if (!name || configs->data.i32[i + 3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) continue;
            const auto w = uint32_t(configs->data.i32[i + 1]), h = uint32_t(configs->data.i32[i + 2]);
            const bool supported = (format == AIMAGE_FORMAT_RAW16 || format == AIMAGE_FORMAT_RAW10) &&
                                   geometry::isSupportedRawGeometry(w, h) &&
                                   (format != AIMAGE_FORMAT_RAW10 || w % 4u == 0u);
            if (!first) out += ',';
            first = false;
            out += "{\"format\":\"" + std::string(name) + "\",\"width\":" + std::to_string(w) +
                   ",\"height\":" + std::to_string(h) + ",\"supported\":" + (supported ? "true" : "false") + "}";
        }
    }
    out += ']';

    const auto black = entry(chars, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN);
    out += ",\"black\":";
    appendNumbers(out, black ? black->data.i32 : static_cast<const int32_t*>(nullptr), black ? black->count : 0);
    const auto white = entry(chars, ACAMERA_SENSOR_INFO_WHITE_LEVEL);
    out += ",\"white\":" + std::to_string(white && white->data.i32 ? white->data.i32[0] : 0) + ",";
    appendSessionKeys(out, chars);
    out += '}';
}

std::string failure(const std::string& error) {
    std::string out = "{\"error\":";
    appendJsonString(out, error);
    out += '}';
    return out;
}

}  // namespace

std::string probeCamerasJson() {
    ManagerPtr manager(ACameraManager_create(), ACameraManager_delete);
    if (!manager) return failure("Camera manager unavailable");
    const auto enumerated = enumeratedCameraIds(manager.get());
    std::set<std::string> listed;
    std::string out = "{\"cameras\":[";
    bool first = true;
    const auto add = [&](const std::string& id, const std::string& parent, const ACameraMetadata* chars) {
        if (!first) out += ',';
        first = false;
        appendCamera(out, id, parent, enumerated.count(id) > 0, chars);
    };
    for (const auto& id : cameraDiscoveryCandidates(manager.get())) {
        const auto chars = characteristics(manager.get(), id);
        if (!chars || listed.count(id)) continue;
        listed.insert(id);
        add(id, "", chars.get());
        for (const auto& physical : physicalIds(chars.get())) {
            const auto physicalChars = characteristics(manager.get(), physical);
            if (physicalChars) add(physical, id, physicalChars.get());
        }
    }
    out += "],\"error\":\"\"}";
    return out;
}

std::string probeCameraKeysJson(const std::string& cameraId, const std::vector<std::string>& names) {
    ManagerPtr manager(ACameraManager_create(), ACameraManager_delete);
    if (!manager) return failure("Camera manager unavailable");
    const auto chars = characteristics(manager.get(), cameraId);
    if (!chars) return failure("Camera " + cameraId + " has no characteristics");

    ACameraDevice_StateCallbacks callbacks{};
    callbacks.onDisconnected = [](void*, ACameraDevice*) {};
    callbacks.onError = [](void*, ACameraDevice*, int) {};
    ACameraDevice* device = nullptr;
    const camera_status_t opened = ACameraManager_openCamera(manager.get(), cameraId.c_str(), &callbacks, &device);
    if (opened != ACAMERA_OK || !device)
        return failure("Camera " + cameraId + " could not be opened (status " + std::to_string(opened) +
                       "); close the camera screen and retry");
    std::unique_ptr<ACameraDevice, decltype(&ACameraDevice_close)> deviceGuard(device, ACameraDevice_close);

    ACaptureRequest* request = nullptr;
    if (ACameraDevice_createCaptureRequest(device, TEMPLATE_PREVIEW, &request) != ACAMERA_OK || !request)
        return failure("Camera " + cameraId + " default request unavailable");
    std::unique_ptr<ACaptureRequest, decltype(&ACaptureRequest_free)> requestGuard(request, ACaptureRequest_free);

    std::string out = "{\"tags\":[";
    int32_t count = 0;
    const uint32_t* tags = nullptr;
    if (ACaptureRequest_getAllTags(request, &count, &tags) == ACAMERA_OK && tags) {
        bool first = true;
        for (int32_t i = 0; i < count; ++i) {
            ACameraMetadata_const_entry e{};
            if (ACaptureRequest_getConstEntry(request, tags[i], &e) != ACAMERA_OK) continue;
            if (!first) out += ',';
            first = false;
            out += "{\"tag\":" + std::to_string(tags[i]) + ",\"type\":\"" + typeName(e.type) +
                   "\",\"count\":" + std::to_string(e.count) + ",\"values\":";
            appendEntryValues(out, e);
            out += '}';
        }
    }
    out += "],\"names\":[";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ',';
        out += "{\"name\":";
        appendJsonString(out, names[i]);
        const auto tag = resolveMetadataTagByNameCompat(chars.get(), cameraId.c_str(), names[i].c_str(), nullptr);
        out += ",\"tag\":" + (tag ? std::to_string(*tag) : std::string("null")) + '}';
    }
    out += "],";
    appendSessionKeys(out, chars.get());
    out += ",\"error\":\"\"}";
    return out;
}

}  // namespace rawrcam::camera
