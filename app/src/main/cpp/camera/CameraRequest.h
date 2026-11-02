#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// A capture request as settings plus targets, independent of the camera API that will carry it. The NDK driver
// wraps an ACaptureRequest; the Java driver builds a Camera2 request from a RequestData copy. Statuses use the
// numeric values of camera_status_t so call sites compare against ACAMERA_OK unchanged.
struct ANativeWindow;
namespace rawrcam::camera {

using RequestStatus = int;
inline constexpr RequestStatus kRequestOk = 0;
inline constexpr RequestStatus kRequestInvalidParameter = -10001;
inline constexpr RequestStatus kRequestNotFound = -10004;

// Numeric values of ACAMERA_TYPE_*.
enum class RequestType : uint8_t { Byte = 0, Int32 = 1, Float = 2, Int64 = 3, Double = 4, Rational = 5 };
struct RequestRational {
    int32_t numerator;
    int32_t denominator;
};

// One request entry as read back; the data lives as long as the request is unchanged. The members mirror
// ACameraMetadata_const_entry, so reading code looks the same on either API.
struct RequestEntry {
    RequestType type = RequestType::Byte;
    uint32_t count = 0;
    union Data {
        const uint8_t* u8;
        const int32_t* i32;
        const float* f;
        const int64_t* i64;
        const double* d;
        const RequestRational* r;
        const void* raw;
    } data{nullptr};
};

class CameraRequest {
   public:
    virtual ~CameraRequest() = default;
    // count 0 clears the tag, so the template (or HAL) default applies again.
    virtual RequestStatus set(uint32_t tag, RequestType type, uint32_t count, const void* data) = 0;
    virtual RequestStatus get(uint32_t tag, RequestEntry* out) const = 0;
    // Tags currently set. The array stays valid until the request changes.
    virtual RequestStatus tags(int32_t* count, const uint32_t** tags) const = 0;
    virtual RequestStatus addTarget(ANativeWindow* window) = 0;
    virtual RequestStatus removeTarget(ANativeWindow* window) = 0;
    // Carried through to the capture callbacks with the request.
    virtual RequestStatus setUserContext(void* context) = 0;
    virtual RequestStatus userContext(void** context) const = 0;
    virtual std::unique_ptr<CameraRequest> copy() const = 0;
    // The name a vendor tag was resolved from. A backend that sets entries by name (Java) needs it for tags the
    // camera doesn't list.
    virtual void noteTagName(uint32_t /*tag*/, const std::string& /*name*/) {}

    RequestStatus setU8(uint32_t tag, uint32_t count, const uint8_t* data) { return set(tag, RequestType::Byte, count, data); }
    RequestStatus setI32(uint32_t tag, uint32_t count, const int32_t* data) { return set(tag, RequestType::Int32, count, data); }
    RequestStatus setI64(uint32_t tag, uint32_t count, const int64_t* data) { return set(tag, RequestType::Int64, count, data); }
    RequestStatus setFloat(uint32_t tag, uint32_t count, const float* data) { return set(tag, RequestType::Float, count, data); }
    RequestStatus setDouble(uint32_t tag, uint32_t count, const double* data) { return set(tag, RequestType::Double, count, data); }
    RequestStatus setRational(uint32_t tag, uint32_t count, const RequestRational* data) {
        return set(tag, RequestType::Rational, count, data);
    }
};

// Settings held in memory: the request of a backend whose camera API takes them as data.
class DataRequest final : public CameraRequest {
   public:
    // Template the request is built from (an ACameraDevice_request_template value).
    explicit DataRequest(int32_t requestTemplate = 0) : template_(requestTemplate) {}
    RequestStatus set(uint32_t tag, RequestType type, uint32_t count, const void* data) override;
    RequestStatus get(uint32_t tag, RequestEntry* out) const override;
    RequestStatus tags(int32_t* count, const uint32_t** tags) const override;
    RequestStatus addTarget(ANativeWindow* window) override;
    RequestStatus removeTarget(ANativeWindow* window) override;
    RequestStatus setUserContext(void* context) override {
        userContext_ = context;
        return kRequestOk;
    }
    RequestStatus userContext(void** context) const override {
        if (!context) return kRequestInvalidParameter;
        *context = userContext_;
        return kRequestOk;
    }
    std::unique_ptr<CameraRequest> copy() const override { return std::make_unique<DataRequest>(*this); }
    void noteTagName(uint32_t tag, const std::string& name) override { names_[tag] = name; }
    const std::map<uint32_t, std::string>& tagNames() const { return names_; }

    int32_t requestTemplate() const { return template_; }
    const std::vector<ANativeWindow*>& targets() const { return targets_; }
    struct Value {
        RequestType type = RequestType::Byte;
        uint32_t count = 0;
        std::vector<uint64_t> words;  // 8-byte aligned storage for any entry type
    };
    // Entries in tag order. A cleared tag is kept with count 0: the Java builder must unset a template default.
    const std::map<uint32_t, Value>& entries() const { return entries_; }

   private:
    int32_t template_;
    std::map<uint32_t, Value> entries_;
    std::vector<ANativeWindow*> targets_;
    std::map<uint32_t, std::string> names_;
    void* userContext_ = nullptr;
    mutable std::vector<uint32_t> tagList_;
};

inline uint32_t requestTypeSize(RequestType type) {
    switch (type) {
        case RequestType::Byte: return 1;
        case RequestType::Int32:
        case RequestType::Float: return 4;
        case RequestType::Int64:
        case RequestType::Double:
        case RequestType::Rational: return 8;
    }
    return 0;
}

}  // namespace rawrcam::camera
