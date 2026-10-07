#include "camera/CameraCallbacks.h"

#include <atomic>

#include "diagnostics/logging/RuntimeTraceRecorder.h"
namespace rawrcam::camera {
namespace {
std::atomic<uint32_t> gFailureDiagnostics{0};
void reportFailure(CameraSessionCallbackContext* callback, const char* kind, int64_t frameNumber, int reason,
                   int sequenceId) {
    auto& trace = diagnostics::RuntimeTraceRecorder::instance();
    if (sequenceId >= 0)
        trace.record(diagnostics::RuntimeTraceStage::CameraCaptureFailed, 0, callback->generation, sequenceId, 0, 0,
                     static_cast<uint32_t>(reason), frameNumber);
    else
        trace.record(diagnostics::RuntimeTraceStage::CameraBufferLost, 0, callback->generation, -1, 0, 0, 0u,
                     frameNumber);
    if (gFailureDiagnostics.fetch_add(1, std::memory_order_relaxed) >= 6) return;
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner)
        guard.owner->cameraDeviceEvent(callback->generation, std::string(kind) + " frame=" +
                                                                 std::to_string(frameNumber) + " reason=" +
                                                                 std::to_string(reason) +
                                                                 " sequenceId=" + std::to_string(sequenceId));
}
}  // namespace
void CameraCallbacks::revoke() noexcept {
    std::unique_lock<std::mutex> lock(lifetime_->mutex);
    lifetime_->owner = nullptr;
    lifetime_->idle.wait(lock, [this] { return lifetime_->inFlight == 0; });
}
CameraDeviceCallbackContext* CameraCallbacks::deviceContext(uint64_t generation) const {
    return new CameraDeviceCallbackContext{lifetime_, generation};
}
CameraSessionCallbackContext* CameraCallbacks::sessionContext(uint64_t generation) const {
    return new CameraSessionCallbackContext{lifetime_, generation};
}
ACameraDevice_StateCallbacks CameraCallbacks::deviceState(CameraDeviceCallbackContext* context) {
    ACameraDevice_StateCallbacks state{};
    state.context = context;
    state.onDisconnected = onDisconnected;
    state.onError = onError;
    return state;
}
ACameraCaptureSession_stateCallbacks CameraCallbacks::sessionState(CameraSessionCallbackContext* context) {
    ACameraCaptureSession_stateCallbacks state{};
    state.context = context;
    state.onClosed = onClosed;
    state.onReady = onReady;
    state.onActive = onActive;
    return state;
}
ACameraCaptureSession_captureCallbacks CameraCallbacks::captures(CameraSessionCallbackContext* context) {
    ACameraCaptureSession_captureCallbacks captures{};
    captures.context = context;
    captures.onCaptureCompleted = onCompleted;
    captures.onCaptureFailed = onFailed;
    captures.onCaptureBufferLost = onBufferLost;
    return captures;
}
ACameraCaptureSession_logicalCamera_captureCallbacks CameraCallbacks::logicalCaptures(
    CameraSessionCallbackContext* context) {
    ACameraCaptureSession_logicalCamera_captureCallbacks captures{};
    captures.context = context;
    captures.onLogicalCameraCaptureCompleted = onLogicalCompleted;
    captures.onLogicalCameraCaptureFailed = onLogicalFailed;
    captures.onCaptureBufferLost = onBufferLost;
    return captures;
}
void CameraCallbacks::onDisconnected(void* context, ACameraDevice*) {
    auto* callback = static_cast<CameraDeviceCallbackContext*>(context);
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner) guard.owner->cameraDeviceEvent(callback->generation, "CAMERA_NDK_DISCONNECTED");
}
void CameraCallbacks::onError(void* context, ACameraDevice*, int error) {
    auto* callback = static_cast<CameraDeviceCallbackContext*>(context);
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner)
        guard.owner->cameraDeviceEvent(callback->generation, "CAMERA_NDK_ERROR error=" + std::to_string(error));
}
void CameraCallbacks::onClosed(void* context, ACameraCaptureSession* session) {
    auto* callback = static_cast<CameraSessionCallbackContext*>(context);
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner) guard.owner->cameraSessionClosed(callback->generation, session);
    delete callback;
}
void CameraCallbacks::onCompleted(void* context, ACameraCaptureSession*, ACaptureRequest* request,
                                  const ACameraMetadata* result) {
    auto* callback = static_cast<CameraSessionCallbackContext*>(context);
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner) guard.owner->cameraCaptureCompleted(callback->generation, request, result);
}
void CameraCallbacks::onLogicalCompleted(void* context, ACameraCaptureSession*, ACaptureRequest* request,
                                         const ACameraMetadata* result, size_t physicalResultCount,
                                         const char** physicalCameraIds, const ACameraMetadata** physicalResults) {
    auto* callback = static_cast<CameraSessionCallbackContext*>(context);
    const ACameraMetadata* chosen = result;
    for (size_t i = 0; i < physicalResultCount; ++i) {
        if (physicalCameraIds && physicalResults && physicalCameraIds[i] &&
            callback->physicalCameraId == physicalCameraIds[i]) {
            chosen = physicalResults[i];
            break;
        }
    }
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner) guard.owner->cameraCaptureCompleted(callback->generation, request, chosen);
}
void CameraCallbacks::onFailed(void* context, ACameraCaptureSession*, ACaptureRequest*,
                               ACameraCaptureFailure* failure) {
    if (!failure) return;
    reportFailure(static_cast<CameraSessionCallbackContext*>(context), "CAMERA_CAPTURE_FAILED", failure->frameNumber,
                  failure->reason, failure->sequenceId);
}
void CameraCallbacks::onLogicalFailed(void* context, ACameraCaptureSession*, ACaptureRequest*,
                                      ALogicalCameraCaptureFailure* failure) {
    if (!failure) return;
    const auto& f = failure->captureFailure;
    reportFailure(static_cast<CameraSessionCallbackContext*>(context), "CAMERA_CAPTURE_FAILED", f.frameNumber,
                  f.reason, f.sequenceId);
}
void CameraCallbacks::onBufferLost(void* context, ACameraCaptureSession*, ACaptureRequest*, ACameraWindowType*,
                                   int64_t frameNumber) {
    reportFailure(static_cast<CameraSessionCallbackContext*>(context), "CAMERA_BUFFER_LOST", frameNumber, 0, -1);
}
}  // namespace rawrcam::camera
