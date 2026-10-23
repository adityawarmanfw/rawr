#pragma once
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>

#include <string>

#include "camera/CameraCallbackLifetime.h"
namespace rawrcam::camera {
class CameraEventSink {
   public:
    virtual ~CameraEventSink() = default;
    virtual void cameraDeviceEvent(uint64_t generation, const std::string& message) = 0;
    virtual void cameraSessionClosed(uint64_t generation, ACameraCaptureSession* session) = 0;
    virtual void cameraCaptureCompleted(uint64_t generation, const ACaptureRequest* request,
                                        const ACameraMetadata* result) = 0;
    // The sensor started exposing a request. Runs on a camera thread.
    virtual void cameraCaptureStarted(uint64_t /*generation*/, const ACaptureRequest* /*request*/, int64_t /*timestamp*/) {}
    // Every failed capture or lost buffer, unthrottled. Runs on a camera thread.
    virtual void cameraCaptureFailed(uint64_t /*generation*/) {}
    virtual void cameraStillFailed(uint64_t /*generation*/, const ACaptureRequest* /*request*/) {}
    virtual void cameraDeviceFailed(uint64_t /*generation*/) {}
    virtual void cameraSequenceEnded(uint64_t /*generation*/, int /*sequenceId*/, bool /*aborted*/) {}
};
using CameraEventLifetime = CameraCallbackLifetime<CameraEventSink>;
struct CameraSessionCallbackContext {
    std::shared_ptr<CameraEventLifetime> lifetime;
    uint64_t generation;
    // Non-empty when the RAW output streams from a physical sub-camera: its
    // per-physical result (matching the frame timestamps) replaces the
    // logical result.
    std::string physicalCameraId = {};
};
struct CameraDeviceCallbackContext {
    std::shared_ptr<CameraEventLifetime> lifetime;
    uint64_t generation;
};
class CameraCallbacks final {
   public:
    explicit CameraCallbacks(CameraEventSink& sink) { lifetime_->owner = &sink; }
    ~CameraCallbacks() { revoke(); }
    CameraCallbacks(const CameraCallbacks&) = delete;
    CameraCallbacks& operator=(const CameraCallbacks&) = delete;
    void revoke() noexcept;
    CameraDeviceCallbackContext* deviceContext(uint64_t generation) const;
    CameraSessionCallbackContext* sessionContext(uint64_t generation) const;
    static ACameraDevice_StateCallbacks deviceState(CameraDeviceCallbackContext*);
    static ACameraCaptureSession_stateCallbacks sessionState(CameraSessionCallbackContext*);
    static ACameraCaptureSession_captureCallbacks captures(CameraSessionCallbackContext*);
    static ACameraCaptureSession_logicalCamera_captureCallbacks logicalCaptures(CameraSessionCallbackContext*);

   private:
    static void onDisconnected(void*, ACameraDevice*);
    static void onError(void*, ACameraDevice*, int);
    static void onClosed(void*, ACameraCaptureSession*);
    static void onReady(void*, ACameraCaptureSession*) {}
    static void onActive(void*, ACameraCaptureSession*) {}
    static void onStarted(void*, ACameraCaptureSession*, const ACaptureRequest*, int64_t);
    static void onCompleted(void*, ACameraCaptureSession*, ACaptureRequest*, const ACameraMetadata*);
    static void onLogicalCompleted(void*, ACameraCaptureSession*, ACaptureRequest*, const ACameraMetadata*, size_t,
                                   const char**, const ACameraMetadata**);
    // Failures only feed diagnostics; a HAL that fails every RAW capture is
    // otherwise indistinguishable from one that never answers.
    static void onFailed(void*, ACameraCaptureSession*, ACaptureRequest*, ACameraCaptureFailure*);
    static void onLogicalFailed(void*, ACameraCaptureSession*, ACaptureRequest*, ALogicalCameraCaptureFailure*);
    static void onBufferLost(void*, ACameraCaptureSession*, ACaptureRequest*, ACameraWindowType*, int64_t);
    static void onSequenceCompleted(void*, ACameraCaptureSession*, int, int64_t);
    static void onSequenceAborted(void*, ACameraCaptureSession*, int);
    std::shared_ptr<CameraEventLifetime> lifetime_ = std::make_shared<CameraEventLifetime>();
};
}  // namespace rawrcam::camera
