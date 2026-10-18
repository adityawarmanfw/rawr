#pragma once

#include "camera/CameraMeteringRegions.h"
#include "camera/TapFocusPolicy.h"

namespace rawrcam::camera {

enum class CameraFocusTrigger { Idle, Start, Cancel };
struct CameraMeteringRequest {
    std::optional<CameraMeteringRegion> afRegion;
    std::optional<CameraMeteringRegion> aeRegion;
    CameraFocusTrigger trigger = CameraFocusTrigger::Idle;
};
struct CameraTapFocusPlan {
    CameraMeteringRegion region;
    bool cancelFirst;
};
enum class FaceFocusChange { None, Updated, Cleared };

// Native focus intent, region ownership and trigger/dwell policy. Caller holds
// the controller mutex and applies/submits requests; this owner has no NDK handles.
class CameraFocusControls {
   public:
    void reset(CameraControlState& state);
    bool requestMode(CameraControlState& state, FocusControlMode mode, uint64_t requestId);
    bool requestManualFocus(CameraControlState& state, float normalized, uint64_t requestId);
    std::optional<CameraTapFocusPlan> planTap(const CameraControlState& state, const metadata::SensorGeometry& geometry,
                                              SensorPoint point) const;
    void acceptTap(CameraControlState& state, const CameraTapFocusPlan& plan, int64_t nowMs);
    void cancelTrigger() { trigger_ = CameraFocusTrigger::Cancel; }
    // Ends a tap; an AF cancel goes out only if a tap scan may be in play.
    void clearTap(CameraControlState& state, bool cancelScan = true);
    bool expireTap(CameraControlState& state, int64_t nowMs);
    bool finishTrigger();
    FaceFocusChange steerFaces(const CameraControlState& state, const metadata::SensorGeometry* geometry,
                               bool requestReady);
    CameraMeteringRequest request(std::optional<CameraMeteringRegion> aeRegion) const {
        return {region_, aeRegion, trigger_};
    }
    // The current tap or face autofocus area (sensor coordinates), if any.
    const std::optional<CameraMeteringRegion>& afRegion() const { return region_; }

   private:
    TapFocusPolicy tapPolicy_;
    std::optional<CameraMeteringRegion> region_;
    CameraFocusTrigger trigger_ = CameraFocusTrigger::Idle;
    bool faceSteering_ = false;
    FaceDetection lastFace_;
};

}  // namespace rawrcam::camera
