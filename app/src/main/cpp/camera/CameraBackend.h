#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "camera/CameraRequest.h"

struct ANativeWindow;
namespace rawrcam::camera {
struct CameraSessionCallbackContext;

// Numeric values of ACameraDevice_request_template.
enum class RequestTemplate : int32_t { Preview = 1, StillCapture = 2 };

// One stream of a capture session.
struct SessionOutput {
    ANativeWindow* window = nullptr;
    std::string physicalCameraId;  // empty: the opened (logical) camera
    int64_t streamUseCase = 0;     // 0: default; nonzero needs the Java backend
};

// A configured capture session. Requests must come from the same driver's device.
class CameraSessionDriver {
   public:
    virtual ~CameraSessionDriver() = default;
    virtual RequestStatus setRepeating(CameraRequest& request, int* sequenceId) = 0;
    virtual RequestStatus capture(const std::vector<CameraRequest*>& requests, int* sequenceId) = 0;
    virtual RequestStatus stopRepeating() = 0;
    // Starts closing; CameraEventSink::cameraSessionClosed follows with identity().
    virtual void close() = 0;
    // What CameraEventSink::cameraSessionClosed reports for this session.
    virtual const void* identity() const = 0;
};

// An open camera.
class CameraDeviceDriver {
   public:
    virtual ~CameraDeviceDriver() = default;
    virtual const char* backendName() const = 0;
    virtual std::unique_ptr<CameraRequest> createRequest(RequestTemplate, RequestStatus* status) = 0;
    // Configures the streams. sessionParameters may be null; opMode 0 is the regular session. Callbacks report
    // through context, which the session owns until it reports closed.
    virtual std::unique_ptr<CameraSessionDriver> createSession(const std::vector<SessionOutput>& outputs,
                                                               const CameraRequest* sessionParameters, int32_t opMode,
                                                               CameraSessionCallbackContext* context,
                                                               RequestStatus* status) = 0;
    // Closes the device; may block on a wedged HAL, so callers run it off the controller thread.
    virtual void close() = 0;
};

}  // namespace rawrcam::camera
