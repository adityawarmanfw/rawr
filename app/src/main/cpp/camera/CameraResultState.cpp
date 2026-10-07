#include "camera/CameraResultState.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "camera/CameraWhiteBalanceControls.h"

namespace rawrcam::camera {

void CameraResultState::reset() { *this = CameraResultState{}; }

std::optional<CameraPriorityAudit> CameraResultState::observe(CameraControlState& state,
                                                              const metadata::FrameMetadataSnapshot& frame,
                                                              CameraControlResult result,
                                                              const CameraRequestProvenance* provenance,
                                                              uint64_t latestSubmittedSerial, bool forcedSensorMode) {
    const bool latestRequestResult = provenance && provenance->requestSerial == latestSubmittedSerial;
    const bool optimizedStillResult = provenance && provenance->optimizedStillRequestId != 0;
    // Every accepted repeating result advances telemetry, including older request
    // serials. One-shot results must not replace preview observations or WB seeds.
    if (!optimizedStillResult) {
        state.appliedExposureTimeNs = frame.exposureTimeNs;
        state.appliedSensitivity = frame.sensitivity;
        state.appliedPostRawSensitivityBoost = std::max(100, frame.postRawSensitivityBoost);
        if (result.frameDurationNs && *result.frameDurationNs > 0) {
            state.appliedRawFps = 1e9 / static_cast<double>(*result.frameDurationNs);
        } else if (result.targetFpsRange && (*result.targetFpsRange)[1] > 0) {
            state.appliedRawFps = static_cast<double>((*result.targetFpsRange)[1]);
        }
        const uint64_t ts = frame.timestampNs;
        if (lastMonitorTimestampNs_ != 0 && ts > lastMonitorTimestampNs_) {
            const uint64_t dt = ts - lastMonitorTimestampNs_;
            if (dt >= 1000000ULL && dt <= 1000000000ULL) {
                const double instant = 1e9 / static_cast<double>(dt);
                viewfinderFpsEma_ = viewfinderFpsEma_ <= 0 ? instant : 0.9 * viewfinderFpsEma_ + 0.1 * instant;
                state.measuredViewfinderFps = viewfinderFpsEma_;
            } else if (dt > 1000000000ULL) {
                viewfinderFpsEma_ = 0;
                state.measuredViewfinderFps.reset();
            }
        }
        if (ts != 0) lastMonitorTimestampNs_ = ts;
        observeWhiteBalanceResult(state, frame);
        if (result.evSteps) state.appliedEvSteps = result.evSteps;
        if (result.afState) state.afState = result.afState;
        if (result.focusDistance) state.appliedFocusDistance = result.focusDistance;
        state.faceDetections = std::move(result.faces);
    }

    // Learn the active sensitivity coordinate conversion only from frames whose
    // request provenance actually owns sensitivity. Camera2 Auto/S priority does
    // not own that axis and must never be used to calibrate it.
    if (provenance && provenance->requestedSensitivity > 0 && frame.sensitivity > 0 &&
        provenance->exposureMode == ExposureControlMode::Manual &&
        // Endpoint requests can be clamped in forced sensor modes. Their
        // result/request quotient is not a measurement of the gain scale.
        // These are REQUEST limits: Auto/report ISO may legitimately exceed
        // the manual range and must not be rejected against that range.
        provenance->requestedSensitivity > state.capabilities.sensitivityMin &&
        (state.capabilities.sensitivityMax <= 0 ||
         provenance->requestedSensitivity < state.capabilities.sensitivityMax)) {
        const double observedRatio =
            static_cast<double>(frame.sensitivity) / static_cast<double>(provenance->requestedSensitivity);
        if (std::isfinite(observedRatio) && observedRatio > 0.0) {
            sensitivityRatio_ = observedRatio;
        }
    }

    if (provenance && (provenance->exposureMode == ExposureControlMode::ShutterPriority ||
                       provenance->exposureMode == ExposureControlMode::IsoPriority)) {
        if (priorityAuditSerial_ != provenance->requestSerial) {
            priorityAuditSerial_ = provenance->requestSerial;
            priorityAuditFrames_ = 0;
            priorityConsecutiveViolations_ = 0;
        }
        ++priorityAuditFrames_;

        const auto resultPriority = result.aePriority;
        const auto resultFps = result.targetFpsRange;

        // Missing result echo is not by itself a contract failure: some HALs do
        // not mirror every request control into CaptureResult. A present-but-wrong
        // priority value is a real failure. The owned sensor axis is authoritative.
        const bool priorityMismatch = resultPriority && *resultPriority != provenance->expectedAePriority;
        bool ownedAxisMismatch = false;
        std::optional<int32_t> expectedReportedSensitivity;
        std::optional<int32_t> sensitivityTolerance;
        if (provenance->exposureMode == ExposureControlMode::IsoPriority) {
            if (sensitivityRatio_ && *sensitivityRatio_ > 0.0) {
                expectedReportedSensitivity = static_cast<int32_t>(
                    std::llround(static_cast<double>(provenance->requestedSensitivity) * *sensitivityRatio_));
                sensitivityTolerance = std::max<int32_t>(
                    2, static_cast<int32_t>(std::llround(std::abs(*expectedReportedSensitivity) * 0.02)));
                ownedAxisMismatch = std::abs(frame.sensitivity - *expectedReportedSensitivity) > *sensitivityTolerance;
            } else if (!forcedSensorMode) {
                expectedReportedSensitivity = provenance->requestedSensitivity;
                sensitivityTolerance = 1;
                ownedAxisMismatch = std::abs(frame.sensitivity - provenance->requestedSensitivity) > 1;
            }
            // In forced-DCG mode, a missing learned mapping is not evidence of
            // failure. Priority tag/fps checks still apply, but do not compare
            // incompatible request/result ISO coordinates and falsely snap back.
        } else {
            const int64_t delta = std::llabs(frame.exposureTimeNs - provenance->requestedExposureTimeNs);
            const int64_t tolerance = std::max<int64_t>(100000LL, provenance->requestedExposureTimeNs / 100);
            ownedAxisMismatch = delta > tolerance;
        }
        const bool fpsMismatch =
            provenance->targetFpsMin > 0 && provenance->targetFpsMax > 0 &&
            (!resultFps || (*resultFps)[0] != provenance->targetFpsMin || (*resultFps)[1] != provenance->targetFpsMax);
        const bool violation = priorityMismatch || ownedAxisMismatch || fpsMismatch;
        if (latestRequestResult && violation) {
            ++priorityConsecutiveViolations_;
        } else if (latestRequestResult) {
            priorityConsecutiveViolations_ = 0;
        }

        CameraPriorityAudit audit;
        audit.request = *provenance;
        audit.latestRequest = latestRequestResult;
        audit.forcedSensorMode = forcedSensorMode;
        audit.resultPriority = resultPriority;
        audit.resultFps = resultFps;
        audit.appliedExposureTimeNs = frame.exposureTimeNs;
        audit.appliedSensitivity = frame.sensitivity;
        audit.reportedPerRequest = sensitivityRatio_;
        audit.expectedReportedSensitivity = expectedReportedSensitivity;
        audit.sensitivityTolerance = sensitivityTolerance;
        audit.priorityMismatch = priorityMismatch;
        audit.ownedAxisMismatch = ownedAxisMismatch;
        audit.fpsMismatch = fpsMismatch;
        audit.shouldLog = violation || priorityAuditFrames_ <= 12;
        // Missing echoes/FPS mismatch alone do not force mode fallback.
        audit.fallbackToAuto =
            latestRequestResult && (priorityMismatch || ownedAxisMismatch) && priorityConsecutiveViolations_ >= 2;
        if (audit.fallbackToAuto) priorityConsecutiveViolations_ = 0;
        return audit;
    }
    return std::nullopt;
}

std::string describeCameraPriorityAudit(const CameraPriorityAudit& audit) {
    const bool violation = audit.priorityMismatch || audit.ownedAxisMismatch || audit.fpsMismatch;
    return std::string(violation ? "CAMERA_AE_PRIORITY_VIOLATION" : "CAMERA_AE_PRIORITY_RESULT") +
           " serial=" + std::to_string(audit.request.requestSerial) +
           " latest=" + (audit.latestRequest ? std::string("true") : std::string("false")) +
           " requestedMode=" + std::to_string(static_cast<int>(audit.request.exposureMode)) +
           " expectedPriority=" + std::to_string(audit.request.expectedAePriority) + " resultPriority=" +
           (audit.resultPriority ? std::to_string(*audit.resultPriority) : std::string("missing")) +
           " requestedShutterNs=" + std::to_string(audit.request.requestedExposureTimeNs) +
           " appliedShutterNs=" + std::to_string(audit.appliedExposureTimeNs) +
           " requestedSensitivity=" + std::to_string(audit.request.requestedSensitivity) +
           " appliedSensitivity=" + std::to_string(audit.appliedSensitivity) + " expectedReportedSensitivity=" +
           (audit.expectedReportedSensitivity ? std::to_string(*audit.expectedReportedSensitivity)
                                              : std::string("unavailable")) +
           " sensitivityTolerance=" +
           (audit.sensitivityTolerance ? std::to_string(*audit.sensitivityTolerance) : std::string("unavailable")) +
           " reportedPerRequest=" +
           (audit.reportedPerRequest ? std::to_string(*audit.reportedPerRequest) : std::string("unavailable")) +
           " requestedFps=" + std::to_string(audit.request.targetFpsMin) + ".." +
           std::to_string(audit.request.targetFpsMax) + " resultFps=" +
           (audit.resultFps ? std::to_string((*audit.resultFps)[0]) + ".." + std::to_string((*audit.resultFps)[1])
                            : std::string("missing")) +
           " reasons=" + (audit.priorityMismatch ? std::string("priority,") : std::string()) +
           (audit.ownedAxisMismatch ? std::string("owned_axis,") : std::string()) +
           (audit.fpsMismatch ? std::string("fps,") : std::string()) +
           " dcg=" + (audit.forcedSensorMode ? std::string("on") : std::string("off"));
}

}  // namespace rawrcam::camera
