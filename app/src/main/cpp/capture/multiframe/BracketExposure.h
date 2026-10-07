#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "metadata/FrameMetadataSnapshot.h"

// Exposure bookkeeping for HDR+ bracketed bursts (merge algorithm 3): ZSL
// frames at the preview exposure plus darker post-shutter frames. Only ratios
// between frames of one burst are used, so the unit is arbitrary.
namespace rawrcam::capture::multiframe {

inline constexpr std::uint32_t kMergeAlgorithmHdrPlusBracketed = 3u;

// Linear RAW exposure of one delivered frame: exposure time x sensor gain
// (the post-RAW boost is applied after RAW, so it is not part of the signal).
// Reported (CaptureResult) values, falling back to the request copy; 0 when
// neither is known.
inline double linearExposure(const metadata::FrameMetadataSnapshot& m) noexcept {
    const std::int64_t time = m.exposureTimeNs > 0 ? m.exposureTimeNs : m.requestedExposureTimeNs.value_or(0);
    const std::int32_t iso = m.sensitivity > 0 ? m.sensitivity : m.requestedSensitivity.value_or(0);
    if (time <= 0 || iso <= 0) return 0.0;
    return double(time) * double(iso);
}

// Frames within 1/8 EV of the darkest count as the darkest group (sensor
// exposure steps are not exact).
inline std::vector<std::uint32_t> darkestFrames(const std::vector<metadata::FrameMetadataSnapshot>& metadata) {
    double darkest = 0.0;
    for (const auto& m : metadata) {
        const double e = linearExposure(m);
        if (e > 0.0 && (darkest == 0.0 || e < darkest)) darkest = e;
    }
    std::vector<std::uint32_t> out;
    if (darkest <= 0.0) return out;
    for (std::uint32_t i = 0; i < metadata.size(); ++i) {
        const double e = linearExposure(metadata[i]);
        if (e > 0.0 && e <= darkest * 1.0905) out.push_back(i);  // 2^(1/8)
    }
    return out;
}

// True when the burst holds frames more than 1/8 EV apart.
inline bool isExposureBracketed(const std::vector<metadata::FrameMetadataSnapshot>& metadata) {
    const auto dark = darkestFrames(metadata);
    std::size_t known = 0;
    for (const auto& m : metadata) known += linearExposure(m) > 0.0 ? 1u : 0u;
    return !dark.empty() && dark.size() < known;
}

// Reference for a bracketed burst: the darkest frame (upstream rule: brighter
// frames are scaled down to it, so nothing it can represent clips). Among
// equally dark frames, the highest score wins when scores are given, else the
// middle one of the group.
inline std::uint32_t bracketReference(const std::vector<metadata::FrameMetadataSnapshot>& metadata,
                                      const std::vector<float>& scores, std::uint32_t fallback) {
    const auto dark = darkestFrames(metadata);
    if (dark.empty()) return fallback;
    if (scores.size() == metadata.size()) {
        std::uint32_t best = dark.front();
        for (const auto i : dark)
            if (scores[i] > scores[best]) best = i;
        return best;
    }
    return dark[dark.size() / 2u];
}

// EV between the brightest frame (the preview exposure) and the reference,
// i.e. how much the merged result must be lifted to match the viewfinder.
inline float bracketLiftEv(const std::vector<metadata::FrameMetadataSnapshot>& metadata,
                           std::uint32_t referenceIndex) noexcept {
    if (referenceIndex >= metadata.size()) return 0.0f;
    const double ref = linearExposure(metadata[referenceIndex]);
    double brightest = 0.0;
    for (const auto& m : metadata) brightest = std::max(brightest, linearExposure(m));
    if (ref <= 0.0 || brightest <= ref) return 0.0f;
    return float(std::log2(brightest / ref));
}

}  // namespace rawrcam::capture::multiframe
