#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace rawrcam::camera {

struct ShutterAngleChoice {
    double degrees;
    int64_t exposureTimeNs;
};

// Camera request cadence belongs to the native camera owner. Recording
// acceptance is supplied by the controller before committing this state.
class CameraCadencePolicy final {
   public:
    void setPhotoFloor(int fps) { photoFloor_ = fps; }
    void setVideoMode(bool value, int fps) {
        videoMode_ = value;
        videoFps_ = fps > 0 ? fps : 30;
        if (!videoMode_) shutterAngle_.reset();
    }
    void setRecordingFps(int fps) { recordingFps_ = fps > 0 ? fps : 0; }
    bool recording() const { return recordingFps_ > 0; }
    bool videoMode() const { return videoMode_; }
    int videoFps() const { return videoFps_; }
    std::optional<double> shutterAngle() const { return shutterAngle_; }
    int previewFloor() const { return videoMode_ ? videoFps_ : photoFloor_; }
    int64_t capExposure(int64_t exposure) const {
        const auto fps = recording() ? recordingFps_ : videoMode_ ? videoFps_ : 0;
        return fps > 0 ? std::min(exposure, int64_t{1000000000} / fps) : exposure;
    }
    std::vector<ShutterAngleChoice> shutterChoices(int64_t minNs, int64_t maxNs) const {
        std::vector<ShutterAngleChoice> choices;
        if (!videoMode_ || minNs <= 0 || maxNs < minNs) return choices;
        const auto fps = recording() ? recordingFps_ : videoFps_;
        if (minNs > int64_t{1000000000} / fps) return choices;
        choices.push_back({(static_cast<double>(minNs) * fps / 1000000000.0) * 360.0, minNs});
        for (const auto angle : kAngles) {
            const auto ns = exposureForAngle(angle);
            if (ns > minNs && ns <= maxNs) choices.push_back({angle, ns});
        }
        return choices;
    }
    // Bind a new manual/priority entry or camera context to its nearest legal
    // angle once. Subsequent FPS changes retain the semantic angle.
    void bindShutter(int64_t exposure, int64_t minNs, int64_t maxNs) {
        const auto choices = shutterChoices(minNs, maxNs);
        if (choices.empty()) {
            shutterAngle_.reset();
            return;
        }
        shutterAngle_ = std::min_element(choices.begin(), choices.end(), [exposure](const auto& a, const auto& b) {
                            return std::abs(static_cast<double>(a.exposureTimeNs) - exposure) <
                                   std::abs(static_cast<double>(b.exposureTimeNs) - exposure);
                        })->degrees;
    }
    bool selectShutterAngle(double angle, int64_t minNs, int64_t maxNs) {
        const auto choices = shutterChoices(minNs, maxNs);
        if (!std::isfinite(angle) || std::none_of(choices.begin(), choices.end(),
                                                  [angle](const auto& choice) { return choice.degrees == angle; }))
            return false;
        shutterAngle_ = angle;
        return true;
    }
    void clearShutterAngle() { shutterAngle_.reset(); }
    int64_t heldExposure(int64_t requested, int64_t minNs, int64_t maxNs) const {
        if (minNs <= 0 || maxNs < minNs) return requested;
        auto exposure = videoMode_ && shutterAngle_ ? exposureForAngle(*shutterAngle_) : requested;
        if (videoMode_ && shutterAngle_) {
            for (const auto& choice : shutterChoices(minNs, maxNs)) {
                if (choice.degrees == *shutterAngle_) {
                    exposure = choice.exposureTimeNs;
                    break;
                }
            }
        }
        return std::clamp(capExposure(exposure), minNs, maxNs);
    }
    int64_t resolveHeldExposure(int64_t requested, int64_t minNs, int64_t maxNs) {
        if (videoMode_ && (!shutterAngle_ || !selectShutterAngle(*shutterAngle_, minNs, maxNs)))
            bindShutter(heldExposure(requested, minNs, maxNs), minNs, maxNs);
        return heldExposure(requested, minNs, maxNs);
    }

   private:
    int64_t exposureForAngle(double angle) const {
        const auto fps = recording() ? recordingFps_ : videoFps_;
        return std::max(int64_t{1}, static_cast<int64_t>((1000000000.0 / fps) * (angle / 360.0)));
    }
    inline static constexpr std::array<double, 11> kAngles{1.40625, 2.8125, 5.625, 11.25, 22.5, 45, 90, 135, 180, 270, 360};
    int photoFloor_ = 15;
    bool videoMode_ = false;
    int videoFps_ = 30;
    int recordingFps_ = 0;
    std::optional<double> shutterAngle_;
};

}  // namespace rawrcam::camera
