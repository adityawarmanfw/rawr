#pragma once
#include <camera/NdkCameraManager.h>

#include "camera/CameraBackend.h"
#include "camera/CameraCallbacks.h"
#include "camera/NdkRequest.h"

namespace rawrcam::camera {

class NdkSessionDriver final : public CameraSessionDriver {
   public:
    NdkSessionDriver(ACameraCaptureSession* session, ACaptureSessionOutputContainer* container,
                     std::vector<ACaptureSessionOutput*> outputs, CameraSessionCallbackContext* context)
        : session_(session), container_(container), outputs_(std::move(outputs)), context_(context) {}
    ~NdkSessionDriver() override;
    RequestStatus setRepeating(CameraRequest& request, int* sequenceId) override;
    RequestStatus capture(const std::vector<CameraRequest*>& requests, int* sequenceId) override;
    RequestStatus stopRepeating() override { return ACameraCaptureSession_stopRepeating(session_); }
    void close() override;
    const void* identity() const override { return session_; }

   private:
    ACameraCaptureSession* session_;
    ACaptureSessionOutputContainer* container_;
    std::vector<ACaptureSessionOutput*> outputs_;
    CameraSessionCallbackContext* context_;
    bool closed_ = false;
};

class NdkDeviceDriver final : public CameraDeviceDriver {
   public:
    explicit NdkDeviceDriver(ACameraDevice* device) : device_(device) {}
    ~NdkDeviceDriver() override { close(); }
    const char* backendName() const override { return "native"; }
    std::unique_ptr<CameraRequest> createRequest(RequestTemplate, RequestStatus* status) override;
    std::unique_ptr<CameraSessionDriver> createSession(const std::vector<SessionOutput>& outputs,
                                                       const CameraRequest* sessionParameters, int32_t opMode,
                                                       CameraSessionCallbackContext* context,
                                                       RequestStatus* status) override;
    void close() override;

   private:
    ACameraDevice* device_;
};

}  // namespace rawrcam::camera
