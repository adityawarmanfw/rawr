#pragma once

#include <cmath>

#include "camera/CameraControlResult.h"
#include "camera/CameraRequestProvenance.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::camera {

struct CameraPriorityAudit {
    CameraRequestProvenance request;
    bool latestRequest = false;
    bool forcedSensorMode = false;
    std::optional<uint8_t> resultPriority;
    std::optional<std::array<int32_t, 2>> resultFps;
    int64_t appliedExposureTimeNs = 0;
    int32_t appliedSensitivity = 0;
    std::optional<double> reportedPerRequest;
    std::optional<int32_t> expectedReportedSensitivity;
    std::optional<int32_t> sensitivityTolerance;
    bool priorityMismatch = false;
    bool ownedAxisMismatch = false;
    bool fpsMismatch = false;
    bool shouldLog = false;
    bool fallbackToAuto = false;
};

std::string describeCameraPriorityAudit(const CameraPriorityAudit& audit);

// Owns observations and result policy for one camera session. Plain values only:
// caller validates generation, holds the camera mutex, and executes any returned
// fallback by changing request mode and submitting through the controller.
class CameraResultState {
   public:
    void reset();
    std::optional<double> sensitivityReportedPerRequest() const { return sensitivityRatio_; }
    // A ratio measured earlier for the same camera and sensor mode.
    void seedSensitivityRatio(double ratio) {
        if (!sensitivityRatio_ && std::isfinite(ratio) && ratio > 0.0) sensitivityRatio_ = ratio;
    }
    std::optional<CameraPriorityAudit> observe(CameraControlState& state, const metadata::FrameMetadataSnapshot& frame,
                                               CameraControlResult result, const CameraRequestProvenance* provenance,
                                               uint64_t latestSubmittedSerial, bool forcedSensorMode);

   private:
    uint64_t lastMonitorTimestampNs_ = 0;
    double viewfinderFpsEma_ = 0;
    std::optional<double> sensitivityRatio_;
    uint64_t priorityAuditSerial_ = 0;
    uint32_t priorityAuditFrames_ = 0;
    uint32_t priorityConsecutiveViolations_ = 0;
};

}  // namespace rawrcam::camera
