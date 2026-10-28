#include "camera/CameraCallbacks.h"
#include "camera/NdkRequest.h"

#include <mutex>

#include "diagnostics/logging/RuntimeTraceRecorder.h"
namespace rawrcam::camera {
namespace {
// Failure lines per capture session: enough to show a pattern without flooding the log when every frame fails.
// Counted per session so a failing session early on cannot hide the failures of later ones.
constexpr uint32_t kFailureLinesPerSession = 8;
std::mutex gFailureLinesMutex;
uint64_t gFailureLinesGeneration = 0;
uint32_t gFailureLines = 0;
bool takeFailureLine(uint64_t generation) {
    std::lock_guard<std::mutex> lock(gFailureLinesMutex);
    if (generation != gFailureLinesGeneration) {
        gFailureLinesGeneration = generation;
        gFailureLines = 0;
    }
    return gFailureLines++ < kFailureLinesPerSession;
}
void reportFailure(CameraSessionCallbackContext* callback, const char* kind, int64_t frameNumber, int reason,
                   int sequenceId, const CameraRequest* request) {
    auto& trace = diagnostics::RuntimeTraceRecorder::instance();
    if (sequenceId >= 0)
        trace.record(diagnostics::RuntimeTraceStage::CameraCaptureFailed, 0, callback->generation, sequenceId, 0, 0,
                     static_cast<uint32_t>(reason), frameNumber);
    else
        trace.record(diagnostics::RuntimeTraceStage::CameraBufferLost, 0, callback->generation, -1, 0, 0, 0u,
                     frameNumber);
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner) guard.owner->cameraCaptureFailed(callback->generation);
    if (guard.owner) guard.owner->cameraStillFailed(callback->generation, request, kind, frameNumber, reason);
    if (!takeFailureLine(callback->generation)) return;
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
    captures.onCaptureStarted = onStarted;
    captures.onCaptureCompleted = onCompleted;
    captures.onCaptureFailed = onFailed;
    captures.onCaptureBufferLost = onBufferLost;
    captures.onCaptureSequenceCompleted = onSequenceCompleted;
    captures.onCaptureSequenceAborted = onSequenceAborted;
    return captures;
}
ACameraCaptureSession_logicalCamera_captureCallbacks CameraCallbacks::logicalCaptures(
    CameraSessionCallbackContext* context) {
    ACameraCaptureSession_logicalCamera_captureCallbacks captures{};
    captures.context = context;
    captures.onCaptureStarted = onStarted;
    captures.onLogicalCameraCaptureCompleted = onLogicalCompleted;
    captures.onLogicalCameraCaptureFailed = onLogicalFailed;
    captures.onCaptureBufferLost = onBufferLost;
    captures.onCaptureSequenceCompleted = onSequenceCompleted;
    captures.onCaptureSequenceAborted = onSequenceAborted;
    return captures;
}
void CameraCallbacks::deviceDisconnected(CameraDeviceCallbackContext* callback) {
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner) guard.owner->cameraDeviceEvent(callback->generation, "CAMERA_NDK_DISCONNECTED");
    if (guard.owner) guard.owner->cameraDeviceFailed(callback->generation);
}
void CameraCallbacks::deviceError(CameraDeviceCallbackContext* callback, int error) {
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner)
        guard.owner->cameraDeviceEvent(callback->generation, "CAMERA_NDK_ERROR error=" + std::to_string(error));
    if (guard.owner) guard.owner->cameraDeviceFailed(callback->generation);
}
void CameraCallbacks::sessionClosed(CameraSessionCallbackContext* callback, const void* session) {
    {
        CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
        if (guard.owner) guard.owner->cameraSessionClosed(callback->generation, session);
    }
    delete callback;
}
void CameraCallbacks::captureStarted(CameraSessionCallbackContext* callback, const CameraRequest* request,
                                     int64_t timestamp) {
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner) guard.owner->cameraCaptureStarted(callback->generation, request, timestamp);
}
void CameraCallbacks::captureCompleted(CameraSessionCallbackContext* callback, const CameraRequest* request,
                                       const ACameraMetadata* result) {
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner) guard.owner->cameraCaptureCompleted(callback->generation, request, result);
}
void CameraCallbacks::captureFailed(CameraSessionCallbackContext* callback, const char* kind, int64_t frameNumber,
                                    int reason, int sequenceId, const CameraRequest* request) {
    reportFailure(callback, kind, frameNumber, reason, sequenceId, request);
}
void CameraCallbacks::sequenceEnded(CameraSessionCallbackContext* callback, int sequenceId, bool aborted) {
    CameraCallbackGuard<CameraEventSink> guard(callback->lifetime);
    if (guard.owner) guard.owner->cameraSequenceEnded(callback->generation, sequenceId, aborted);
}
void CameraCallbacks::onDisconnected(void* context, ACameraDevice*) {
    deviceDisconnected(static_cast<CameraDeviceCallbackContext*>(context));
}
void CameraCallbacks::onError(void* context, ACameraDevice*, int error) {
    deviceError(static_cast<CameraDeviceCallbackContext*>(context), error);
}
void CameraCallbacks::onClosed(void* context, ACameraCaptureSession* session) {
    sessionClosed(static_cast<CameraSessionCallbackContext*>(context), session);
}
void CameraCallbacks::onStarted(void* context, ACameraCaptureSession*, const ACaptureRequest* request, int64_t timestamp) {
    const NdkRequest wrapped(request);
    captureStarted(static_cast<CameraSessionCallbackContext*>(context), &wrapped, timestamp);
}
void CameraCallbacks::onCompleted(void* context, ACameraCaptureSession*, ACaptureRequest* request,
                                  const ACameraMetadata* result) {
    const NdkRequest wrapped(request);
    captureCompleted(static_cast<CameraSessionCallbackContext*>(context), &wrapped, result);
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
    const NdkRequest wrapped(request);
    captureCompleted(callback, &wrapped, chosen);
}
void CameraCallbacks::onFailed(void* context, ACameraCaptureSession*, ACaptureRequest* request,
                               ACameraCaptureFailure* failure) {
    if (!failure) return;
    const NdkRequest wrapped(request);
    reportFailure(static_cast<CameraSessionCallbackContext*>(context), "CAMERA_CAPTURE_FAILED", failure->frameNumber,
                  failure->reason, failure->sequenceId, &wrapped);
}
void CameraCallbacks::onLogicalFailed(void* context, ACameraCaptureSession*, ACaptureRequest* request,
                                      ALogicalCameraCaptureFailure* failure) {
    if (!failure) return;
    const auto& f = failure->captureFailure;
    const NdkRequest wrapped(request);
    reportFailure(static_cast<CameraSessionCallbackContext*>(context), "CAMERA_CAPTURE_FAILED", f.frameNumber, f.reason,
                  f.sequenceId, &wrapped);
}
void CameraCallbacks::onBufferLost(void* context, ACameraCaptureSession*, ACaptureRequest* request, ACameraWindowType*,
                                   int64_t frameNumber) {
    const NdkRequest wrapped(request);
    reportFailure(static_cast<CameraSessionCallbackContext*>(context), "CAMERA_BUFFER_LOST", frameNumber, 0, -1,
                  &wrapped);
}
void CameraCallbacks::onSequenceCompleted(void* context, ACameraCaptureSession*, int id, int64_t) {
    sequenceEnded(static_cast<CameraSessionCallbackContext*>(context), id, false);
}
void CameraCallbacks::onSequenceAborted(void* context, ACameraCaptureSession*, int id) {
    sequenceEnded(static_cast<CameraSessionCallbackContext*>(context), id, true);
}
}  // namespace rawrcam::camera
