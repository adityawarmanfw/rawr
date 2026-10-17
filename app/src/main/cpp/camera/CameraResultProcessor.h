#pragma once
#include <camera/NdkCaptureRequest.h>

#include <functional>

#include "camera/CameraResultState.h"
#include "camera/CameraRouting.h"
namespace rawrcam::camera {
struct CameraResultActions {
    std::optional<metadata::FrameMetadataSnapshot> frame;
    bool fallbackToAuto = false;
    bool optimizedStill = false;
};
class CameraResultProcessor final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    explicit CameraResultProcessor(Diagnostic diagnostic) : diagnostic_(std::move(diagnostic)) {}
    void reset();
    void auditCallback(uint64_t callbackGeneration, uint64_t generation, bool active, bool hasContext, bool hasResult);
    CameraResultActions process(const ACaptureRequest*, const ACameraMetadata*, CameraControlState&,
                                const metadata::CameraContextMetadataPtr&, const std::optional<LevelOverride>& staticLevels,
                                bool sensorModeOverridden, uint64_t latestSubmittedSerial);
    std::optional<double> sensitivityReportedPerRequest() const { return resultState_.sensitivityReportedPerRequest(); }
    void seedSensitivityRatio(double ratio) { resultState_.seedSensitivityRatio(ratio); }

   private:
    void diag(const std::string& line) const {
        if (diagnostic_) diagnostic_(line);
    }
    Diagnostic diagnostic_;
    CameraResultState resultState_;
    uint64_t metadataLogCount_ = 0, frameOrdinal_ = 0;
    uint32_t callbackAuditCount_ = 0, rejectAuditCount_ = 0, acceptAuditCount_ = 0;
};
}  // namespace rawrcam::camera
