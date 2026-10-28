#include "camera/NdkRequest.h"

namespace rawrcam::camera {

std::unique_ptr<NdkRequest> NdkRequest::create(ACameraDevice* device, ACameraDevice_request_template requestTemplate,
                                               camera_status_t* status) {
    ACaptureRequest* request = nullptr;
    const camera_status_t s = ACameraDevice_createCaptureRequest(device, requestTemplate, &request);
    if (status) *status = s;
    if (s != ACAMERA_OK || !request) return nullptr;
    return std::unique_ptr<NdkRequest>(new NdkRequest(request, true));
}

NdkRequest::~NdkRequest() {
    for (auto& [window, target] : targets_) ACameraOutputTarget_free(target);
    if (owned_ && request_) ACaptureRequest_free(request_);
}

RequestStatus NdkRequest::set(uint32_t tag, RequestType type, uint32_t count, const void* data) {
    switch (type) {
        case RequestType::Byte: return ACaptureRequest_setEntry_u8(request_, tag, count, static_cast<const uint8_t*>(data));
        case RequestType::Int32: return ACaptureRequest_setEntry_i32(request_, tag, count, static_cast<const int32_t*>(data));
        case RequestType::Float: return ACaptureRequest_setEntry_float(request_, tag, count, static_cast<const float*>(data));
        case RequestType::Int64: return ACaptureRequest_setEntry_i64(request_, tag, count, static_cast<const int64_t*>(data));
        case RequestType::Double: return ACaptureRequest_setEntry_double(request_, tag, count, static_cast<const double*>(data));
        case RequestType::Rational:
            return ACaptureRequest_setEntry_rational(request_, tag, count, static_cast<const ACameraMetadata_rational*>(data));
    }
    return kRequestInvalidParameter;
}

RequestStatus NdkRequest::get(uint32_t tag, RequestEntry* out) const {
    if (!out) return kRequestInvalidParameter;
    ACameraMetadata_const_entry e{};
    const camera_status_t s = ACaptureRequest_getConstEntry(request_, tag, &e);
    if (s != ACAMERA_OK) return s;
    out->type = static_cast<RequestType>(e.type);
    out->count = e.count;
    out->data.raw = e.data.u8;
    return kRequestOk;
}

RequestStatus NdkRequest::tags(int32_t* count, const uint32_t** tags) const {
    return ACaptureRequest_getAllTags(request_, count, tags);
}

RequestStatus NdkRequest::addTarget(ANativeWindow* window) {
    if (!window) return kRequestInvalidParameter;
    auto it = targets_.find(window);
    if (it == targets_.end()) {
        ACameraOutputTarget* target = nullptr;
        const camera_status_t s = ACameraOutputTarget_create(window, &target);
        if (s != ACAMERA_OK || !target) return s != ACAMERA_OK ? s : kRequestInvalidParameter;
        it = targets_.emplace(window, target).first;
    }
    return ACaptureRequest_addTarget(request_, it->second);
}

RequestStatus NdkRequest::removeTarget(ANativeWindow* window) {
    if (!window) return kRequestInvalidParameter;
    // Targets are matched by window; a request copy has no target object of its own to remove with.
    ACameraOutputTarget* target = nullptr;
    const auto it = targets_.find(window);
    const bool temporary = it == targets_.end();
    if (temporary) {
        const camera_status_t s = ACameraOutputTarget_create(window, &target);
        if (s != ACAMERA_OK || !target) return s != ACAMERA_OK ? s : kRequestInvalidParameter;
    } else {
        target = it->second;
    }
    const camera_status_t removed = ACaptureRequest_removeTarget(request_, target);
    if (temporary) ACameraOutputTarget_free(target);
    return removed;
}

RequestStatus NdkRequest::setUserContext(void* context) { return ACaptureRequest_setUserContext(request_, context); }

RequestStatus NdkRequest::userContext(void** context) const { return ACaptureRequest_getUserContext(request_, context); }

std::unique_ptr<CameraRequest> NdkRequest::copy() const {
    ACaptureRequest* copied = ACaptureRequest_copy(request_);
    if (!copied) return nullptr;
    return std::unique_ptr<CameraRequest>(new NdkRequest(copied, true));
}

}  // namespace rawrcam::camera
