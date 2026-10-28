#include <array>
#include "camera/CameraRequestPipeline.h"
#include "camera/CameraRequestCadence.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "diagnostics/logging/RuntimeTraceRecorder.h"

namespace rawrcam::camera {
namespace {
bool ok(RequestStatus status) { return status == ACAMERA_OK; }
std::string statusText(RequestStatus status) { return std::to_string(static_cast<int>(status)); }
}  // namespace
const CameraRequestProvenance* CameraRequestPipeline::attach(CameraRequest* request,
                                                             const CameraControlState& control, bool focusStart) {
    if (!request) return nullptr;
    auto provenance =
        std::make_unique<CameraRequestProvenance>(CameraRequestProvenance::from(control, nextRequestSerial_++));
    provenance->focusTriggerStart = focusStart;
    CameraRequestProvenance* const ptr = provenance.get();
    if (request->setUserContext(ptr) == ACAMERA_OK) {
        provenance_.push_back(std::move(provenance));
        return ptr;
    }
    diag("CAMERA_REQUEST_PROVENANCE_FAIL");
    return nullptr;
}
bool CameraRequestPipeline::submit(CameraSessionDriver* session, CameraRequest* request,
                                   const CameraControlState& control, const CameraMeteringRequest& metering) {
    if (!session || !request) return false;
    const auto applyResult = applyCameraControlState(request, control, metering);
    if (applyResult.focusStatus != ACAMERA_OK) {
        diag("CAMERA_FOCUS_REQUEST_FAILED status=" + statusText(applyResult.focusStatus));
        return false;
    }
    const bool priorityMode = applyResult.halPriorityMode;
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
    const RequestStatus s = session->setRepeating(*request, &sequenceId);
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
bool CameraRequestPipeline::captureBracket(CameraSessionDriver* session, const CameraRequest* repeating,
                                           const CameraControlState& control,
                                           const std::vector<BracketExposure>& exposures, uint64_t tagId,
                                           const PrepareRequest& prepare) {
    if (!session || !repeating || exposures.empty() || tagId == 0) return false;
    std::vector<std::unique_ptr<CameraRequest>> owned;
    std::vector<CameraRequest*> requests;
    for (const auto& e : exposures) {
        auto request = repeating->copy();
        if (!request) {
            diag("CAMERA_BRACKET_COPY_FAIL");
            return false;
        }
        requests.push_back(request.get());
        owned.push_back(std::move(request));
        if (!prepareBracketRequest(requests.back(), control, e, tagId, prepare)) return false;
    }
    int sequenceId = 0;
    const RequestStatus s = session->capture(requests, &sequenceId);
    if (!ok(s)) {
        diag("CAMERA_BRACKET_CAPTURE_FAILURE status=" + statusText(s));
        return false;
    }
    std::string line = "CAMERA_BRACKET_SUBMIT tag=" + std::to_string(tagId) + " sequenceId=" + std::to_string(sequenceId);
    lastCaptureSequence_ = sequenceId;
    for (const auto& e : exposures)
        line += " " + std::to_string(e.exposureTimeNs) + "ns@" + std::to_string(e.sensitivity);
    diag(line);
    return true;
}
bool CameraRequestPipeline::repeatBracket(CameraSessionDriver* session, const CameraRequest* repeating,
                                          const CameraControlState& control, const BracketExposure& exposure,
                                          uint64_t tagId, const PrepareRequest& prepare) {
    if (!session || !repeating || tagId == 0) return false;
    const auto request = repeating->copy();
    if (!request) {
        diag("CAMERA_BRACKET_COPY_FAIL");
        return false;
    }
    if (!prepareBracketRequest(request.get(), control, exposure, tagId, prepare)) return false;
    int sequenceId = 0;
    const RequestStatus s = session->setRepeating(*request, &sequenceId);
    if (!ok(s)) {
        diag("CAMERA_BRACKET_REPEATING_FAILURE status=" + statusText(s));
        return false;
    }
    diag("CAMERA_BRACKET_REPEATING tag=" + std::to_string(tagId) + " sequenceId=" + std::to_string(sequenceId) + " " +
         std::to_string(exposure.exposureTimeNs) + "ns@" + std::to_string(exposure.sensitivity));
    return true;
}
bool CameraRequestPipeline::prepareBracketRequest(CameraRequest* request, const CameraControlState& control,
                                                  const BracketExposure& e, uint64_t tagId,
                                                  const PrepareRequest& prepare) {
    const uint8_t aeOff = ACAMERA_CONTROL_AE_MODE_OFF;
    // Bracket frames run at their own exposure but at least the preview
    // cadence, so the repeating stream keeps its timing around them.
    const int64_t frameDuration = cameraBracketFrameDuration(control, e.exposureTimeNs);
    if (request->setU8(ACAMERA_CONTROL_AE_MODE, 1, &aeOff) != ACAMERA_OK ||
        request->setI64(ACAMERA_SENSOR_EXPOSURE_TIME, 1, &e.exposureTimeNs) != ACAMERA_OK ||
        request->setI32(ACAMERA_SENSOR_SENSITIVITY, 1, &e.sensitivity) != ACAMERA_OK ||
        request->setI64(ACAMERA_SENSOR_FRAME_DURATION, 1, &frameDuration) != ACAMERA_OK) {
        diag("CAMERA_BRACKET_REQUEST_FAIL");
        return false;
    }
    // Never replay a one-shot AF/AE trigger the repeating request may carry.
    const uint8_t afIdle = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
    const uint8_t precaptureIdle = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
    (void)request->setU8(ACAMERA_CONTROL_AF_TRIGGER, 1, &afIdle);
    (void)request->setU8(ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &precaptureIdle);
    if (prepare && !prepare(request)) {
        diag("CAMERA_BRACKET_PREPARE_FAIL");
        return false;
    }
    auto provenance =
        std::make_unique<CameraRequestProvenance>(CameraRequestProvenance::from(control, nextRequestSerial_++));
    provenance->exposureMode = ExposureControlMode::Manual;
    provenance->expectedAePriority = camera2_priority::kOff;
    provenance->requestedExposureTimeNs = e.exposureTimeNs;
    provenance->requestedSensitivity = e.sensitivity;
    provenance->optimizedStillRequestId = tagId;
    if (request->setUserContext(provenance.get()) != ACAMERA_OK) {
        diag("CAMERA_REQUEST_PROVENANCE_FAIL");
        return false;
    }
    provenance_.push_back(std::move(provenance));
    return true;
}
bool CameraRequestPipeline::captureAfTrigger(CameraSessionDriver* session, const CameraRequest* repeating,
                                             const CameraControlState& control, CameraFocusTrigger trigger) {
    if (!session || !repeating || trigger == CameraFocusTrigger::Idle) return false;
    if (trigger == CameraFocusTrigger::Cancel && control.capabilities.tapAfHoldsUntilNextTap) {
        diag("CAMERA_AF_CANCEL_BLOCKED reason=vendor_driver");
        return false;
    }
    const auto owned = repeating->copy();
    CameraRequest* const request = owned.get();
    if (!request) return false;
    const uint8_t value = trigger == CameraFocusTrigger::Start ? ACAMERA_CONTROL_AF_TRIGGER_START
                                                               : ACAMERA_CONTROL_AF_TRIGGER_CANCEL;
    if (request->setU8(ACAMERA_CONTROL_AF_TRIGGER, 1, &value) != ACAMERA_OK) return false;
    // vivo's camera sequence: the cancel returns to continuous AF without
    // regions; the start carries the tap region for AE as well as AF. A cancel
    // into MF keeps the repeating request's AF off and lens distance: a
    // continuous-AF frame would refocus away from the manual position.
    if (trigger == CameraFocusTrigger::Cancel) {
        if (control.focusMode != FocusControlMode::Manual) {
            const uint8_t continuous = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
            (void)request->setU8(ACAMERA_CONTROL_AF_MODE, 1, &continuous);
        }
        (void)request->setI32(ACAMERA_CONTROL_AF_REGIONS, 0, nullptr);
    } else {
        RequestEntry af{};
        RequestEntry ae{};
        const bool hasAf = request->get(ACAMERA_CONTROL_AF_REGIONS, &af) == ACAMERA_OK &&
                           af.count == 5;
        const bool hasAe = request->get(ACAMERA_CONTROL_AE_REGIONS, &ae) == ACAMERA_OK &&
                           ae.count == 5;
        if (hasAf && !hasAe) {
            const std::array<int32_t, 5> region{af.data.i32[0], af.data.i32[1], af.data.i32[2], af.data.i32[3],
                                                af.data.i32[4]};
            (void)request->setI32(ACAMERA_CONTROL_AE_REGIONS, 5, region.data());
        }
    }
    const auto* provenance = attach(request, control, trigger == CameraFocusTrigger::Start);
    if (!provenance) return false;
    int sequenceId = 0;
    const RequestStatus s = session->capture({request}, &sequenceId);
    if (!ok(s)) {
        diag("CAMERA_AF_TRIGGER_CAPTURE_FAILURE status=" + statusText(s));
        return false;
    }
    return true;
}
RequestStatus CameraRequestPipeline::applySessionCadence(CameraRequest* sessionRequest,
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
