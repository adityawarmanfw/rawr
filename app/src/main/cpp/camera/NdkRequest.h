#pragma once
#include <camera/NdkCameraDevice.h>

#include <map>

#include "camera/CameraRequest.h"

namespace rawrcam::camera {

// A CameraRequest over an ACaptureRequest. Owning requests free it; a borrowed one wraps the framework's copy
// handed to a capture callback and is read-only in practice.
class NdkRequest final : public CameraRequest {
   public:
    static std::unique_ptr<NdkRequest> create(ACameraDevice* device, ACameraDevice_request_template requestTemplate,
                                              camera_status_t* status = nullptr);
    // Wraps the framework's request copy handed to a capture callback, without owning it.
    explicit NdkRequest(const ACaptureRequest* borrowed) : request_(const_cast<ACaptureRequest*>(borrowed)), owned_(false) {}
    ~NdkRequest() override;
    RequestStatus set(uint32_t tag, RequestType type, uint32_t count, const void* data) override;
    RequestStatus get(uint32_t tag, RequestEntry* out) const override;
    RequestStatus tags(int32_t* count, const uint32_t** tags) const override;
    RequestStatus addTarget(ANativeWindow* window) override;
    RequestStatus removeTarget(ANativeWindow* window) override;
    RequestStatus setUserContext(void* context) override;
    RequestStatus userContext(void** context) const override;
    std::unique_ptr<CameraRequest> copy() const override;
    // For the NDK driver only.
    ACaptureRequest* native() const noexcept { return request_; }

   private:
    NdkRequest(ACaptureRequest* request, bool owned) : request_(request), owned_(owned) {}
    ACaptureRequest* request_;
    bool owned_;
    std::map<ANativeWindow*, ACameraOutputTarget*> targets_;
};

}  // namespace rawrcam::camera
