#include "camera/CameraKeyInjection.h"

#include <camera/NdkCameraMetadataTags.h>

#include <cstdlib>
#include <optional>
#include <sstream>

#include "camera/CameraMetadataTags.h"

namespace rawrcam::camera {
namespace {

std::optional<uint32_t> resolveTag(const ACameraMetadata* characteristics, const char* cameraId, const std::string& tag, std::string* error) {
    if (!tag.empty() && tag[0] >= '0' && tag[0] <= '9') {
        char* end = nullptr;
        const unsigned long value = std::strtoul(tag.c_str(), &end, 0);
        if (end && *end == '\0') return static_cast<uint32_t>(value);
        if (error) *error = "invalid_numeric_tag";
        return std::nullopt;
    }
    return resolveMetadataTagByNameCompat(characteristics, cameraId, tag.c_str(), error);
}

camera_status_t setEntry(ACaptureRequest* request, uint32_t tag, const CameraKeySetting& key) {
    const auto n = static_cast<uint32_t>(key.values.size());
    switch (key.type) {
        case CameraKeySetting::Type::Byte: {
            std::vector<uint8_t> v(key.values.begin(), key.values.end());
            return ACaptureRequest_setEntry_u8(request, tag, n, v.data());
        }
        case CameraKeySetting::Type::Int32: {
            std::vector<int32_t> v(key.values.begin(), key.values.end());
            return ACaptureRequest_setEntry_i32(request, tag, n, v.data());
        }
        case CameraKeySetting::Type::Int64: {
            std::vector<int64_t> v(key.values.begin(), key.values.end());
            return ACaptureRequest_setEntry_i64(request, tag, n, v.data());
        }
        case CameraKeySetting::Type::Float: {
            std::vector<float> v(key.values.begin(), key.values.end());
            return ACaptureRequest_setEntry_float(request, tag, n, v.data());
        }
        case CameraKeySetting::Type::Double:
            return ACaptureRequest_setEntry_double(request, tag, n, key.values.data());
    }
    return ACAMERA_ERROR_INVALID_PARAMETER;
}

}  // namespace

CameraKeyInjectionResult applyCameraKeySettings(ACaptureRequest* request, const ACameraMetadata* characteristics,
                                                const std::vector<CameraKeySetting>& keys, const char* cameraId,
                                                CameraKeySetting::Scope scope, const char* origin,
                                                const std::function<void(const std::string&)>& diag) {
    CameraKeyInjectionResult result;
    const bool session = scope == CameraKeySetting::Scope::Session;
    for (const auto& key : keys) {
        if (!key.enabled || key.scope != scope) continue;
        std::string error;
        const auto tag = resolveTag(characteristics, cameraId, key.tag, &error);
        std::string skip;
        if (!tag) {
            skip = "lookup:" + error;
        } else if (key.values.empty()) {
            skip = "no_values";
        } else if (session && !containsI32Tag(characteristics, ACAMERA_REQUEST_AVAILABLE_SESSION_KEYS, *tag)) {
            skip = "not_advertised_as_session_key";
        } else if (const camera_status_t s = setEntry(request, *tag, key); s != ACAMERA_OK) {
            skip = "set_status=" + std::to_string(s);
        }
        std::ostringstream line;
        line << "CAMERA_KEY_INJECTION scope="
             << (session ? "session" : scope == CameraKeySetting::Scope::Request ? "request" : "still") << " origin=" << origin
             << " tag=" << key.tag;
        if (tag) line << " id=0x" << std::hex << *tag << std::dec;
        line << " values=";
        for (size_t i = 0; i < key.values.size(); ++i) line << (i ? "," : "") << key.values[i];
        if (skip.empty()) {
            ++result.applied;
            line << " applied=true";
        } else {
            result.failures.push_back(key.tag + ":" + skip);
            line << " applied=false reason=" << skip;
        }
        diag(line.str());
    }
    return result;
}

}  // namespace rawrcam::camera
