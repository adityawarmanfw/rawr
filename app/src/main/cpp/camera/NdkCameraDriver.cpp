#include "camera/NdkCameraDriver.h"

namespace rawrcam::camera {
namespace {
ACaptureRequest* nativeOf(CameraRequest& request) { return static_cast<NdkRequest&>(request).native(); }
}  // namespace

NdkSessionDriver::~NdkSessionDriver() {
    // The session must have reported closed (or been orphaned) before its outputs go.
    for (auto* output : outputs_) {
        if (container_) (void)ACaptureSessionOutputContainer_remove(container_, output);
        ACaptureSessionOutput_free(output);
    }
    if (container_) ACaptureSessionOutputContainer_free(container_);
}

RequestStatus NdkSessionDriver::setRepeating(CameraRequest& request, int* sequenceId) {
    ACaptureRequest* native = nativeOf(request);
    if (context_->physicalCameraId.empty()) {
        auto captures = CameraCallbacks::captures(context_);
        return ACameraCaptureSession_setRepeatingRequest(session_, &captures, 1, &native, sequenceId);
    }
    auto captures = CameraCallbacks::logicalCaptures(context_);
    return ACameraCaptureSession_logicalCamera_setRepeatingRequest(session_, &captures, 1, &native, sequenceId);
}

RequestStatus NdkSessionDriver::capture(const std::vector<CameraRequest*>& requests, int* sequenceId) {
    std::vector<ACaptureRequest*> natives;
    natives.reserve(requests.size());
    for (auto* r : requests) natives.push_back(nativeOf(*r));
    if (context_->physicalCameraId.empty()) {
        auto captures = CameraCallbacks::captures(context_);
        return ACameraCaptureSession_capture(session_, &captures, int(natives.size()), natives.data(), sequenceId);
    }
    auto captures = CameraCallbacks::logicalCaptures(context_);
    return ACameraCaptureSession_logicalCamera_capture(session_, &captures, int(natives.size()), natives.data(),
                                                       sequenceId);
}

void NdkSessionDriver::close() {
    if (closed_) return;
    closed_ = true;
    ACameraCaptureSession_close(session_);
}

std::unique_ptr<CameraRequest> NdkDeviceDriver::createRequest(RequestTemplate t, RequestStatus* status) {
    camera_status_t s = ACAMERA_ERROR_INVALID_OPERATION;
    auto request = device_ ? NdkRequest::create(device_, static_cast<ACameraDevice_request_template>(t), &s) : nullptr;
    if (status) *status = s;
    return request;
}

std::unique_ptr<CameraSessionDriver> NdkDeviceDriver::createSession(const std::vector<SessionOutput>& outputs,
                                                                    const CameraRequest* sessionParameters,
                                                                    int32_t opMode,
                                                                    CameraSessionCallbackContext* context,
                                                                    RequestStatus* status) {
    const auto failed = [&](RequestStatus s) -> std::unique_ptr<CameraSessionDriver> {
        if (status) *status = s;
        return nullptr;
    };
    // The NDK has no operating mode and no stream use case; the routing never sends such sessions here.
    if (!device_ || opMode != 0) return failed(ACAMERA_ERROR_INVALID_PARAMETER);
    for (const auto& o : outputs)
        if (o.streamUseCase != 0 || !o.window) return failed(ACAMERA_ERROR_INVALID_PARAMETER);
    ACaptureSessionOutputContainer* container = nullptr;
    camera_status_t s = ACaptureSessionOutputContainer_create(&container);
    if (s != ACAMERA_OK) return failed(s);
    std::vector<ACaptureSessionOutput*> created;
    const auto release = [&] {
        for (auto* output : created) {
            (void)ACaptureSessionOutputContainer_remove(container, output);
            ACaptureSessionOutput_free(output);
        }
        ACaptureSessionOutputContainer_free(container);
    };
    for (const auto& o : outputs) {
        ACaptureSessionOutput* output = nullptr;
        s = o.physicalCameraId.empty() ? ACaptureSessionOutput_create(o.window, &output)
                                       : ACaptureSessionPhysicalOutput_create(o.window, o.physicalCameraId.c_str(), &output);
        if (s == ACAMERA_OK) {
            s = ACaptureSessionOutputContainer_add(container, output);
            if (s != ACAMERA_OK) ACaptureSessionOutput_free(output);
        }
        if (s != ACAMERA_OK) {
            release();
            return failed(s);
        }
        created.push_back(output);
    }
    auto state = CameraCallbacks::sessionState(context);
    ACameraCaptureSession* session = nullptr;
    const ACaptureRequest* params =
        sessionParameters ? static_cast<const NdkRequest*>(sessionParameters)->native() : nullptr;
    s = params ? ACameraDevice_createCaptureSessionWithSessionParameters(device_, container, params, &state, &session)
               : ACameraDevice_createCaptureSession(device_, container, &state, &session);
    if (s != ACAMERA_OK || !session) {
        release();
        return failed(s != ACAMERA_OK ? s : ACAMERA_ERROR_UNKNOWN);
    }
    if (status) *status = ACAMERA_OK;
    return std::make_unique<NdkSessionDriver>(session, container, std::move(created), context);
}

void NdkDeviceDriver::close() {
    if (!device_) return;
    ACameraDevice_close(device_);
    device_ = nullptr;
}

}  // namespace rawrcam::camera
