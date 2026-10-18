#include "camera/CameraResultProcessor.h"

#include <cstdlib>
#include <utility>

#include "camera/CameraResultReader.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "metadata/CameraMetadataReader.h"
#include "metadata/MetadataDiagnostics.h"

namespace rawrcam::camera {
void CameraResultProcessor::reset() {
    resultState_.reset();
    metadataLogCount_ = frameOrdinal_ = 0;
    callbackAuditCount_ = rejectAuditCount_ = acceptAuditCount_ = 0;
}
void CameraResultProcessor::auditCallback(uint64_t callbackGeneration, uint64_t generation, bool active,
                                          bool hasContext, bool hasResult) {
    if (callbackAuditCount_++ >= 6) return;
    diag("PIPELINE_CAPTURE_RESULT_CALLBACK callbackGeneration=" + std::to_string(callbackGeneration) +
         " currentGeneration=" + std::to_string(generation) + " active=" + (active ? "true" : "false") +
         " hasCameraContext=" + (hasContext ? "true" : "false") + " hasResult=" + (hasResult ? "true" : "false"));
}
CameraResultActions CameraResultProcessor::process(const ACaptureRequest* requestCopy, const ACameraMetadata* result,
                                                   CameraControlState& control,
                                                   const metadata::CameraContextMetadataPtr& cameraContext,
                                                   const std::optional<LevelOverride>& staticLevels,
                                                   bool sensorModeOverridden, uint64_t latestSubmittedRequestSerial) {
    CameraResultActions actions{};
    const auto requestObservation = readCameraRequestObservation(requestCopy);
    const auto* provenance = requestObservation.provenance ? &*requestObservation.provenance : nullptr;

    std::string error;
    // Colour from the last valid frame of this camera context when the result has none.
    const bool fallbackUsable = lastColor_ && lastColor_->cameraContext == cameraContext;
    auto frame = metadata::readFrameMetadataSnapshot(result, cameraContext, frameOrdinal_ + 1, &error,
                                                     fallbackUsable ? &*lastColor_ : nullptr);
    if (frame && staticLevels) {
        frame->blackLevelPhysicalRggb = staticLevels->blackRggb;
        frame->effectiveWhiteLevel = staticLevels->white;
        frame->effectiveWhiteLevelSource = metadata::EffectiveWhiteLevelSource::ProfileStatic;
    }
    if (frame) {
        frame->requestedExposureTimeNs = requestObservation.exposureTimeNs;
        frame->requestedSensitivity = requestObservation.sensitivity;
        if (provenance && provenance->optimizedStillRequestId != 0) {
            frame->optimizedStillRequestId = provenance->optimizedStillRequestId;
        }
    }
    if (!frame) {
        if (rejectAuditCount_ < 6) {
            ++rejectAuditCount_;
            diag("PIPELINE_FRAME_METADATA_REJECT generation=" + std::to_string(cameraContext->cameraContextGeneration) +
                 " error=" + error);
        }
        if (metadataLogCount_ < 3) diag("FRAME_METADATA_REJECT error=" + error);
        return actions;
    }
    // Viewfinder results only: a still that borrowed colour must not seed it.
    if (!provenance || provenance->optimizedStillRequestId == 0) lastColor_ = *frame;
    if (acceptAuditCount_ < 6) {
        ++acceptAuditCount_;
        diag("PIPELINE_FRAME_METADATA_ACCEPTED generation=" + std::to_string(cameraContext->cameraContextGeneration) +
             " timestampNs=" + std::to_string(frame->timestampNs));
    }
    ++frameOrdinal_;
    auto observation = readCameraControlResult(result, control.capabilities, cameraContext->geometry);
    const auto audit = resultState_.observe(control, *frame, std::move(observation), provenance,
                                            latestSubmittedRequestSerial, sensorModeOverridden);
    if (audit) {
        if (audit->shouldLog) diag(describeCameraPriorityAudit(*audit));
        if (audit->fallbackToAuto) {
            const auto failedMode = control.exposureMode;
            diag("CAMERA_AE_PRIORITY_FALLBACK_TO_AUTO mode=" + std::to_string(static_cast<int>(failedMode)) +
                 " serial=" + std::to_string(provenance->requestSerial) +
                 " reason=" + (audit->priorityMismatch ? std::string("priority,") : std::string()) +
                 (audit->ownedAxisMismatch ? std::string("owned_axis,") : std::string()));
            actions.fallbackToAuto = true;
            rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
                rawrcam::diagnostics::RuntimeTraceStage::ExposureMode, frame->timestampNs, frame->frameOrdinal, -1,
                frame->exposureTimeNs, frame->sensitivity, 0x80000000u | static_cast<std::uint32_t>(failedMode),
                static_cast<std::int64_t>(ExposureControlMode::Auto), std::atoi(control.capabilities.cameraId.c_str()));
        }
    }
    if (metadataLogCount_ < 3) {
        diag(metadata::describe(*frame));
        ++metadataLogCount_;
    }
    actions.optimizedStill = provenance && provenance->optimizedStillRequestId != 0;
    actions.frame = std::move(frame);
    return actions;
}
}  // namespace rawrcam::camera
