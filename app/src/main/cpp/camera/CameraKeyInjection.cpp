#include "camera/CameraKeyInjection.h"

#include <camera/NdkCameraMetadataTags.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

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

using KeyType = CameraKeySetting::Type;

camera_status_t setEntryAs(ACaptureRequest* request, uint32_t tag, KeyType type, const std::vector<double>& values) {
    const auto n = static_cast<uint32_t>(values.size());
    switch (type) {
        case KeyType::Byte: {
            std::vector<uint8_t> v(values.begin(), values.end());
            return ACaptureRequest_setEntry_u8(request, tag, n, v.data());
        }
        case KeyType::Int32: {
            std::vector<int32_t> v(values.begin(), values.end());
            return ACaptureRequest_setEntry_i32(request, tag, n, v.data());
        }
        case KeyType::Int64: {
            std::vector<int64_t> v(values.begin(), values.end());
            return ACaptureRequest_setEntry_i64(request, tag, n, v.data());
        }
        case KeyType::Float: {
            std::vector<float> v(values.begin(), values.end());
            return ACaptureRequest_setEntry_float(request, tag, n, v.data());
        }
        case KeyType::Double:
            return ACaptureRequest_setEntry_double(request, tag, n, values.data());
    }
    return ACAMERA_ERROR_INVALID_PARAMETER;
}

const char* keyTypeName(KeyType t) {
    switch (t) {
        case KeyType::Byte: return "byte";
        case KeyType::Int32: return "int32";
        case KeyType::Int64: return "int64";
        case KeyType::Float: return "float";
        case KeyType::Double: return "double";
    }
    return "?";
}

bool representable(KeyType t, const std::vector<double>& values) {
    for (const double v : values) {
        if (t == KeyType::Float || t == KeyType::Double) continue;
        if (v != std::floor(v)) return false;
        if (t == KeyType::Byte && (v < 0 || v > 255)) return false;
        if (t == KeyType::Int32 && (v < INT32_MIN || v > INT32_MAX)) return false;
    }
    return true;
}

// Vendor tags have one HAL-defined type and the metadata layer rejects any
// other ("Mismatched tag type"). Profiles are written by hand and default to
// int32, so a key the HAL declares as byte (OPLUS engineercamera tags, ...)
// fails for no visible reason. Try the declared type first, then the others
// that can hold the values, and remember what the HAL accepted per tag.
camera_status_t setEntry(ACaptureRequest* request, uint32_t tag, const CameraKeySetting& key, KeyType* usedType) {
    static std::mutex mutex;
    static std::unordered_map<uint32_t, KeyType> accepted;
    std::vector<KeyType> order;
    const auto push = [&](KeyType t) {
        if (std::find(order.begin(), order.end(), t) == order.end()) order.push_back(t);
    };
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = accepted.find(tag); it != accepted.end()) push(it->second);
    }
    push(key.type);
    for (const KeyType t : {KeyType::Byte, KeyType::Int32, KeyType::Int64, KeyType::Float, KeyType::Double}) push(t);

    camera_status_t first = ACAMERA_OK;
    for (const KeyType t : order) {
        if (!representable(t, key.values)) continue;
        const camera_status_t status = setEntryAs(request, tag, t, key.values);
        if (status == ACAMERA_OK) {
            *usedType = t;
            std::lock_guard<std::mutex> lock(mutex);
            accepted[tag] = t;
            return status;
        }
        if (first == ACAMERA_OK) first = status;
    }
    return first == ACAMERA_OK ? ACAMERA_ERROR_INVALID_PARAMETER : first;
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
        KeyType usedType = key.type;
        if (!tag) {
            skip = "lookup:" + error;
        } else if (key.values.empty()) {
            skip = "no_values";
        } else if (session && !containsI32Tag(characteristics, ACAMERA_REQUEST_AVAILABLE_SESSION_KEYS, *tag)) {
            skip = "not_advertised_as_session_key";
        } else if (const camera_status_t s = setEntry(request, *tag, key, &usedType); s != ACAMERA_OK) {
            skip = "set_status=" + std::to_string(s);
        }
        std::ostringstream line;
        line << "CAMERA_KEY_INJECTION scope="
             << (session                                       ? "session"
                 : scope == CameraKeySetting::Scope::Request  ? "request"
                 : scope == CameraKeySetting::Scope::Decision ? "decision"
                                                              : "still")
             << " origin=" << origin
             << " tag=" << key.tag;
        if (tag) {
            line << " id=0x" << std::hex << *tag << std::dec;
            // Where the HAL lists the tag. A session-only value reaches a HAL that reads it per request
            // only if the tag is also a request key; the log makes that visible instead of guessable.
            line << " advertised=";
            bool any = false;
            for (const auto& [name, list] : {std::pair{"session", uint32_t(ACAMERA_REQUEST_AVAILABLE_SESSION_KEYS)},
                                             std::pair{"request", uint32_t(ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS)},
                                             std::pair{"result", uint32_t(ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS)}}) {
                if (!containsI32Tag(characteristics, list, *tag)) continue;
                line << (any ? "," : "") << name;
                any = true;
            }
            if (!any) line << "none";
        }
        line << " values=";
        for (size_t i = 0; i < key.values.size(); ++i) line << (i ? "," : "") << key.values[i];
        if (skip.empty()) {
            ++result.applied;
            line << " applied=true";
            if (usedType != key.type)
                line << " type=" << keyTypeName(usedType) << " declared=" << keyTypeName(key.type);
        } else {
            result.failures.push_back(key.tag + ":" + skip);
            line << " applied=false reason=" << skip;
        }
        diag(line.str());
    }
    return result;
}

void logCarriedKeys(const ACaptureRequest* sessionParameters, const ACaptureRequest* repeating,
                    const ACameraMetadata* characteristics, const std::vector<CameraKeySetting>& keys,
                    const char* cameraId, const std::function<void(const std::string&)>& diag) {
    const auto carries = [](const ACaptureRequest* request, uint32_t tag) {
        ACameraMetadata_const_entry e{};
        return request && ACaptureRequest_getConstEntry(request, tag, &e) == ACAMERA_OK;
    };
    for (const auto& key : keys) {
        if (!key.enabled) continue;
        std::string error;
        const auto tag = resolveTag(characteristics, cameraId, key.tag, &error);
        if (!tag) continue;
        diag("CAMERA_KEY_CARRIED tag=" + key.tag + " session=" + (carries(sessionParameters, *tag) ? "yes" : "no") +
             " repeating=" + (carries(repeating, *tag) ? "yes" : "no"));
    }
}

void dumpCaptureRequest(const ACaptureRequest* request, const char* kind,
                        const std::function<void(const std::string&)>& diag) {
    if (!request) return;
    int32_t count = 0;
    const uint32_t* tags = nullptr;
    if (ACaptureRequest_getAllTags(request, &count, &tags) != ACAMERA_OK || !tags) return;
    std::vector<uint32_t> sorted(tags, tags + count);
    std::sort(sorted.begin(), sorted.end());
    for (const uint32_t tag : sorted) {
        ACameraMetadata_const_entry e{};
        if (ACaptureRequest_getConstEntry(request, tag, &e) != ACAMERA_OK) continue;
        std::ostringstream line;
        line << "CAMERA_REQUEST_DUMP kind=" << kind << " tag=0x" << std::hex << tag << std::dec
             << " type=" << int(e.type) << " count=" << e.count << " values=";
        const uint32_t shown = std::min<uint32_t>(e.count, 16);
        for (uint32_t i = 0; i < shown; ++i) {
            if (i) line << ',';
            switch (e.type) {
                case ACAMERA_TYPE_BYTE: line << int(e.data.u8[i]); break;
                case ACAMERA_TYPE_INT32: line << e.data.i32[i]; break;
                case ACAMERA_TYPE_INT64: line << e.data.i64[i]; break;
                case ACAMERA_TYPE_FLOAT: line << e.data.f[i]; break;
                case ACAMERA_TYPE_DOUBLE: line << e.data.d[i]; break;
                case ACAMERA_TYPE_RATIONAL: line << e.data.r[i].numerator << '/' << e.data.r[i].denominator; break;
                default: line << '?';
            }
        }
        if (e.count > shown) line << ",...";
        diag(line.str());
    }
}

}  // namespace rawrcam::camera
