#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "camera/CameraCallbacks.h"
#include "camera/CameraRequestControls.h"
#include "camera/CameraRequestProvenance.h"
namespace rawrcam::camera {
class CameraRequestPipeline final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    explicit CameraRequestPipeline(Diagnostic diagnostic) : diagnostic_(std::move(diagnostic)) {}
    bool submit(ACameraCaptureSession*, ACaptureRequest*, CameraSessionCallbackContext*, const CameraControlState&,
                const CameraMeteringRequest&);
    camera_status_t applySessionCadence(ACaptureRequest*, const CameraControlState&);
    // One-off burst of AE-off copies of the repeating request (HDR+ bracket
    // dark frames), tagged with optimizedStillRequestId = tagId. The
    // repeating request keeps running and resumes after the burst.
    struct BracketExposure {
        int64_t exposureTimeNs = 0;
        int32_t sensitivity = 0;  // request coordinate
    };
    // prepare (optional) adjusts each copy before submit, e.g. full-size still
    // targets and keys; false aborts the burst.
    using PrepareRequest = std::function<bool(ACaptureRequest*)>;
    bool captureBracket(ACameraCaptureSession*, const ACaptureRequest* repeating, CameraSessionCallbackContext*,
                        const CameraControlState&, const std::vector<BracketExposure>&, uint64_t tagId,
                        const PrepareRequest& prepare = {});
    // One-shot copy of the repeating request carrying an AF trigger. The
    // repeating request itself stays at IDLE: a trigger left in it repeats on
    // every frame until replaced and restarts the AF scan (vivo X300 Ultra:
    // the provider crashes in the restarted low-light scan).
    bool captureAfTrigger(ACameraCaptureSession*, const ACaptureRequest* repeating, CameraSessionCallbackContext*,
                          const CameraControlState&, CameraFocusTrigger trigger);
    uint64_t latestSubmittedSerial() const noexcept { return latestSubmittedRequestSerial_; }
    int lastCaptureSequence() const noexcept { return lastCaptureSequence_; }
    void retire() noexcept { provenance_.clear(); }

   private:
    const CameraRequestProvenance* attach(ACaptureRequest*, const CameraControlState&, bool focusStart = false);
    void diag(const std::string& line) const {
        if (diagnostic_) diagnostic_(line);
    }
    Diagnostic diagnostic_;
    int lastCaptureSequence_ = -1;
    uint64_t nextRequestSerial_ = 1, latestSubmittedRequestSerial_ = 0;
    std::vector<std::unique_ptr<CameraRequestProvenance>> provenance_;
};
}  // namespace rawrcam::camera
