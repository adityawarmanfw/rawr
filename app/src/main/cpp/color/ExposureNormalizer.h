#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace rawrcam::color {

// Viewfinder exposure normalization for HDR+ Bracketed. Camera2 AE runs with
// a headroom compensation the viewfinder lifts back; the compensation a frame
// reports changes before its sensor exposure does, so lifting by it bumps the
// brightness on every step. Instead the lift is reference / actual exposure:
//
//   lift(frame) = ref - log2(exposure x gain of that frame)
//
// where ref is the "normal" exposure (Camera2 AE without headroom), updated
// only from frames whose AE has settled (exposure no longer moving) as
// log2(exposure) - reported headroom. Exposure steps, ours or AE's, cancel on
// the frame they land; scene changes reach the viewfinder once AE settles,
// eased over ~0.4 s (faster for large changes). Lifts are kept per frame so a
// still gets exactly the lift its reference frame was shown with.
class ExposureNormalizer {
   public:
    static constexpr float kTauSec = 0.4f;
    static constexpr float kFastTauSec = 0.15f;      // errors above kFastErrorEv
    static constexpr float kFastErrorEv = 1.5f;
    static constexpr float kStableEv = 0.05f;       // frame-to-frame exposure change still "settled"
    static constexpr float kUnsettledTrackSec = 1.5f;  // AE that never settles (flicker) is tracked anyway
    // A compensation change reaches the result before the exposure follows.
    static constexpr std::uint64_t kHeadroomSettleNs = 250'000'000ull;
    static constexpr float kMinLiftEv = -4.0f, kMaxLiftEv = 6.0f;

    // linearExposure: exposure time x sensor gain of the frame (> 0).
    // reportedHeadroomEv: the frame's own compensation headroom (<= 0).
    // Returns the frame's lift; repeated calls for one timestamp return the
    // first answer.
    float update(std::uint64_t timestampNs, double linearExposure, float reportedHeadroomEv, bool aeSettled) noexcept {
        if (const auto cached = liftFor(timestampNs)) return *cached;
        if (!(linearExposure > 0.0) || !std::isfinite(reportedHeadroomEv)) return last_;
        // An older frame (a still's reference no longer kept) does not move the state.
        if (primed_ && timestampNs < lastNs_) return last_;
        const float cur = float(std::log2(linearExposure));
        const float normal = cur - std::min(0.0f, reportedHeadroomEv);
        if (!primed_) {
            primed_ = true;
            ref_ = normal;
            lastNs_ = timestampNs;
            lastCur_ = cur;
            unsettledSec_ = 0.0f;
            lastHeadroom_ = reportedHeadroomEv;
        }
        if (std::abs(reportedHeadroomEv - lastHeadroom_) > 1e-4f) {
            lastHeadroom_ = reportedHeadroomEv;
            headroomChangedNs_ = timestampNs;
        }
        const float dt = timestampNs > lastNs_ ? std::min(float(timestampNs - lastNs_) * 1e-9f, 0.25f) : 0.0f;
        const bool steady = aeSettled && std::abs(cur - lastCur_) < kStableEv &&
                            timestampNs >= headroomChangedNs_ + kHeadroomSettleNs;
        unsettledSec_ = steady ? 0.0f : unsettledSec_ + dt;
        if (steady || unsettledSec_ > kUnsettledTrackSec) {
            const float error = normal - ref_;
            const float tau = std::abs(error) > kFastErrorEv ? kFastTauSec : kTauSec;
            ref_ += error * (1.0f - std::exp(-dt / tau));
        }
        lastNs_ = std::max(lastNs_, timestampNs);
        lastCur_ = cur;
        last_ = std::clamp(ref_ - cur, kMinLiftEv, kMaxLiftEv);
        ring_[next_] = {timestampNs, last_};
        next_ = (next_ + 1) % ring_.size();
        return last_;
    }
    [[nodiscard]] std::optional<float> liftFor(std::uint64_t timestampNs) const noexcept {
        if (timestampNs == 0) return std::nullopt;
        for (const auto& e : ring_)
            if (e.timestampNs == timestampNs) return e.liftEv;
        return std::nullopt;
    }
    [[nodiscard]] float lastLift() const noexcept { return last_; }
    [[nodiscard]] bool primed() const noexcept { return primed_; }
    void reset() noexcept { *this = ExposureNormalizer{}; }

   private:
    struct Entry {
        std::uint64_t timestampNs = 0;
        float liftEv = 0.0f;
    };
    std::array<Entry, 256> ring_{};  // ~8 s at 30 fps: covers the ZSL reference
    std::size_t next_ = 0;
    float ref_ = 0.0f, lastCur_ = 0.0f, last_ = 0.0f, unsettledSec_ = 0.0f, lastHeadroom_ = 0.0f;
    std::uint64_t lastNs_ = 0, headroomChangedNs_ = 0;
    bool primed_ = false;
};

}  // namespace rawrcam::color
