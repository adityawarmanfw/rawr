#include "camera/CameraRequest.h"

#include <algorithm>
#include <cstring>

namespace rawrcam::camera {

RequestStatus DataRequest::set(uint32_t tag, RequestType type, uint32_t count, const void* data) {
    if (count > 0 && !data) return kRequestInvalidParameter;
    Value value;
    value.type = type;
    value.count = count;
    const size_t bytes = size_t(count) * requestTypeSize(type);
    value.words.assign((bytes + 7) / 8, 0);
    if (bytes) std::memcpy(value.words.data(), data, bytes);
    entries_[tag] = std::move(value);
    return kRequestOk;
}

RequestStatus DataRequest::get(uint32_t tag, RequestEntry* out) const {
    if (!out) return kRequestInvalidParameter;
    const auto it = entries_.find(tag);
    // A cleared tag reads like an absent one, as on a native request.
    if (it == entries_.end() || it->second.count == 0) return kRequestNotFound;
    out->type = it->second.type;
    out->count = it->second.count;
    out->data.raw = it->second.words.data();
    return kRequestOk;
}

RequestStatus DataRequest::tags(int32_t* count, const uint32_t** tags) const {
    if (!count || !tags) return kRequestInvalidParameter;
    tagList_.clear();
    for (const auto& [tag, value] : entries_)
        if (value.count > 0) tagList_.push_back(tag);
    *count = int32_t(tagList_.size());
    *tags = tagList_.data();
    return kRequestOk;
}

RequestStatus DataRequest::addTarget(ANativeWindow* window) {
    if (!window) return kRequestInvalidParameter;
    if (std::find(targets_.begin(), targets_.end(), window) == targets_.end()) targets_.push_back(window);
    return kRequestOk;
}

RequestStatus DataRequest::removeTarget(ANativeWindow* window) {
    if (!window) return kRequestInvalidParameter;
    targets_.erase(std::remove(targets_.begin(), targets_.end(), window), targets_.end());
    return kRequestOk;
}

}  // namespace rawrcam::camera
