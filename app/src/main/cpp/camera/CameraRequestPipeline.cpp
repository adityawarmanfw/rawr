#include "camera/CameraRequestPipeline.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "diagnostics/logging/RuntimeTraceRecorder.h"

namespace rawrcam::camera {
namespace {
bool ok(camera_status_t status) { return status == ACAMERA_OK; }
std::string statusText(camera_status_t status) { return std::to_string(static_cast<int>(status)); }
}  // namespace
const CameraRequestProvenance* CameraRequestPipeline::attach(ACaptureRequest* request,
                                                             const CameraControlState& control) {
    if (!request) return nullptr;
    auto provenance =
        std::make_unique<CameraRequestProvenance>(CameraRequestProvenance::from(control, nextRequestSerial_++));
    CameraRequestProvenance* const ptr = provenance.get();
    if (ACaptureRequest_setUserContext(request, ptr) == ACAMERA_OK) {
        provenance_.push_back(std::move(provenance));
        return ptr;
    }
    diag("CAMERA_REQUEST_PROVENANCE_FAIL");
    return nullptr;
}
bool CameraRequestPipeline::submit(ACameraCaptureSession* session, ACaptureRequest* request,
                                   CameraSessionCallbackContext* context, const CameraControlState& control,
                                   const CameraMeteringRequest& metering) {
    if (!session || !request || !context) return false;
    const auto applyResult = applyCameraControlState(request, control, metering);
    const bool priorityMode = control.exposureMode == ExposureControlMode::ShutterPriority ||
                              control.exposureMode == ExposureControlMode::IsoPriority;
    if (priorityMode && (!applyResult.aePriorityRequested || applyResult.aePriorityStatus != ACAMERA_OK)) {
        diag("CAMERA_AE_PRIORITY_REQUEST_FAIL mode=" + std::to_string(static_cast<int>(control.exposureMode)) +
             " status=" + statusText(applyResult.aePriorityStatus));
        diag("CAMERA_AE_PRIORITY_SUBMIT_BLOCKED mode=" + std::to_string(static_cast<int>(control.exposureMode)) +
             " reason=priority_tag status=" + statusText(applyResult.aePriorityStatus));
        return false;
    }
    if ((priorityMode || control.recordingFps > 0) && applyResult.aeTargetFpsRequested &&
        applyResult.aeTargetFpsStatus != ACAMERA_OK) {
        diag("CAMERA_AE_PRIORITY_SUBMIT_BLOCKED mode=" + std::to_string(static_cast<int>(control.exposureMode)) +
             " reason=target_fps status=" + statusText(applyResult.aeTargetFpsStatus));
        return false;
    }
    if (applyResult.wbManualRequested && applyResult.wbManualFallbackToAuto) {
        diag(std::string("CAMERA_WB_MANUAL_FALLBACK_TO_AUTO reason=") +
             (!control.capabilities.manualGainsSupported ? "unsupported" : "no_transform_seed") +
             " status=" + statusText(applyResult.wbStatus));
    }
    const CameraRequestProvenance* const provenance = attach(request, control);
    if (!provenance) return false;
    int sequenceId = 0;
    camera_status_t s = ACAMERA_OK;
    if (context->physicalCameraId.empty()) {
        auto captures = CameraCallbacks::captures(context);
        s = ACameraCaptureSession_setRepeatingRequest(session, &captures, 1, &request, &sequenceId);
    } else {
        auto captures = CameraCallbacks::logicalCaptures(context);
        s = ACameraCaptureSession_logicalCamera_setRepeatingRequest(session, &captures, 1, &request, &sequenceId);
    }
    if (!ok(s)) {
        diag("CAMERA_CONTROL_REPEATING_FAILURE status=" + statusText(s));
        return false;
    }
    latestSubmittedRequestSerial_ = provenance->requestSerial;
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
        rawrcam::diagnostics::RuntimeTraceStage::CameraRequestSubmit, 0, provenance->requestSerial, -1,
        provenance->requestedExposureTimeNs, provenance->requestedSensitivity, 0u, sequenceId,
        std::atoi(control.capabilities.cameraId.c_str()));
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
        rawrcam::diagnostics::RuntimeTraceStage::ExposureMode, 0, provenance->requestSerial, -1,
        provenance->requestedExposureTimeNs, provenance->requestedSensitivity,
        static_cast<std::uint32_t>(provenance->exposureMode), static_cast<std::int64_t>(provenance->exposureMode),
        std::atoi(control.capabilities.cameraId.c_str()));
    if (priorityMode) {
        diag("CAMERA_AE_PRIORITY_SUBMIT serial=" + std::to_string(provenance->requestSerial) +
             " mode=" + std::to_string(static_cast<int>(provenance->exposureMode)) +
             " priority=" + std::to_string(provenance->expectedAePriority) +
             " shutterNs=" + std::to_string(provenance->requestedExposureTimeNs) + " sensitivity=" +
             std::to_string(provenance->requestedSensitivity) + " fps=" + std::to_string(provenance->targetFpsMin) +
             ".." + std::to_string(provenance->targetFpsMax) + " sequenceId=" + std::to_string(sequenceId));
    }
    return true;
}
bool CameraRequestPipeline::captureBracket(ACameraCaptureSession* session, const ACaptureRequest* repeating,
                                           CameraSessionCallbackContext* context, const CameraControlState& control,
                                           const std::vector<BracketExposure>& exposures, uint64_t tagId) {
    if (!session || !repeating || !context || exposures.empty() || tagId == 0) return false;
    std::vector<ACaptureRequest*> requests;
    struct FreeRequests {
        std::vector<ACaptureRequest*>& requests;
        ~FreeRequests() {
            for (auto* r : requests) ACaptureRequest_free(r);
        }
    } freeRequests{requests};
    for (const auto& e : exposures) {
        ACaptureRequest* request = ACaptureRequest_copy(repeating);
        if (!request) {
            diag("CAMERA_BRACKET_COPY_FAIL");
            return false;
        }
        requests.push_back(request);
        const uint8_t aeOff = ACAMERA_CONTROL_AE_MODE_OFF;
        // Bracket frames run at their own exposure but at least the preview
        // cadence, so the repeating stream keeps its timing around them.
        constexpr int64_t kThirtyFpsFrameNs = 33333333LL;
        int64_t frameDuration = std::max(kThirtyFpsFrameNs, e.exposureTimeNs);
        if (control.capabilities.maxFrameDurationNs > 0)
            frameDuration = std::min(frameDuration, control.capabilities.maxFrameDurationNs);
        if (ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &aeOff) != ACAMERA_OK ||
            ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &e.exposureTimeNs) != ACAMERA_OK ||
            ACaptureRequest_setEntry_i32(request, ACAMERA_SENSOR_SENSITIVITY, 1, &e.sensitivity) != ACAMERA_OK ||
            ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_FRAME_DURATION, 1, &frameDuration) != ACAMERA_OK) {
            diag("CAMERA_BRACKET_REQUEST_FAIL");
            return false;
        }
        // Never replay a one-shot AF/AE trigger the repeating request may carry.
        const uint8_t afIdle = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
        const uint8_t precaptureIdle = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
        (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_TRIGGER, 1, &afIdle);
        (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &precaptureIdle);
        auto provenance =
            std::make_unique<CameraRequestProvenance>(CameraRequestProvenance::from(control, nextRequestSerial_++));
        provenance->exposureMode = ExposureControlMode::Manual;
        provenance->expectedAePriority = camera2_priority::kOff;
        provenance->requestedExposureTimeNs = e.exposureTimeNs;
        provenance->requestedSensitivity = e.sensitivity;
        provenance->optimizedStillRequestId = tagId;
        if (ACaptureRequest_setUserContext(request, provenance.get()) != ACAMERA_OK) {
            diag("CAMERA_REQUEST_PROVENANCE_FAIL");
            return false;
        }
        provenance_.push_back(std::move(provenance));
    }
    int sequenceId = 0;
    camera_status_t s = ACAMERA_OK;
    if (context->physicalCameraId.empty()) {
        auto captures = CameraCallbacks::captures(context);
        s = ACameraCaptureSession_capture(session, &captures, int(requests.size()), requests.data(), &sequenceId);
    } else {
        auto captures = CameraCallbacks::logicalCaptures(context);
        s = ACameraCaptureSession_logicalCamera_capture(session, &captures, int(requests.size()), requests.data(),
                                                        &sequenceId);
    }
    if (!ok(s)) {
        diag("CAMERA_BRACKET_CAPTURE_FAILURE status=" + statusText(s));
        return false;
    }
    std::string line = "CAMERA_BRACKET_SUBMIT tag=" + std::to_string(tagId) + " sequenceId=" + std::to_string(sequenceId);
    for (const auto& e : exposures)
        line += " " + std::to_string(e.exposureTimeNs) + "ns@" + std::to_string(e.sensitivity);
    diag(line);
    return true;
}
camera_status_t CameraRequestPipeline::applySessionCadence(ACaptureRequest* sessionRequest,
                                                           const CameraControlState& control) {
    const auto result = applyCameraSessionCadence(sessionRequest, control);
    if (!result.fpsRange) return result.status;
    const bool priority = control.exposureMode == ExposureControlMode::ShutterPriority ||
                          control.exposureMode == ExposureControlMode::IsoPriority;
    if (control.recordingFps == 0) {
        diag(std::string(priority ? "CAMERA_AE_PRIORITY_SESSION_FPS" : "CAMERA_AE_AUTO_SESSION_FPS") +
             " requested=" + std::to_string((*result.fpsRange)[0]) + ".." + std::to_string((*result.fpsRange)[1]) +
             " status=" + statusText(result.status));
    }
    return result.status;
}
}  // namespace rawrcam::camera
