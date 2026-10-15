#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "metadata/FrameMetadataSnapshot.h"

// Exposure bookkeeping for HDR+ bracketed bursts (merge algorithm 3): a ZSL
// burst underexposed below Camera2 AE for highlight headroom plus brighter
// post-shutter long frames (older captures: ZSL frames at the preview
// exposure plus darker post-shutter frames). Only ratios between exposures
// of one capture are used, so the unit is arbitrary.
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
// Older dark-frame captures only; see headroomLiftEv.
inline float bracketLiftEv(const std::vector<metadata::FrameMetadataSnapshot>& metadata,
                           std::uint32_t referenceIndex) noexcept {
    if (referenceIndex >= metadata.size()) return 0.0f;
    const double ref = linearExposure(metadata[referenceIndex]);
    double brightest = 0.0;
    for (const auto& m : metadata) brightest = std::max(brightest, linearExposure(m));
    if (ref <= 0.0 || brightest <= ref) return 0.0f;
    return float(std::log2(brightest / ref));
}

// EV the reference must be lifted by to reach the Camera2 AE exposure the
// underexposed burst was taken below (MultiframeTuning::aeLinearExposure);
// 0 when unknown or not underexposed.
inline float headroomLiftEv(float aeLinearExposure, const metadata::FrameMetadataSnapshot& reference) noexcept {
    const double exposure = linearExposure(reference);
    if (!(aeLinearExposure > 0.0f) || !(exposure > 0.0)) return 0.0f;
    return std::max(0.0f, float(std::log2(double(aeLinearExposure) / exposure)));
}

// Variance scale of an HDR+ merge relative to its reference frame, for the
// merged DNG's NoiseProfile (variance = S*x + O in the reference's normalized
// units): {shot scale for S, read scale for O}. Frame i is scaled by
// 1/ef_i to the reference and weighted by ef_i (exposure-weighted mean), and
// its noise follows the reference profile at its own gain g_i (S ~ g, O ~ g^2):
//   shot = sum(ef_i g_i) / sum(ef_i)^2,  read = sum(g_i^2) / sum(ef_i)^2.
// A uniform burst of N frames gives {1/N, 1/N}. This is the static-scene
// ideal; rejected (moving) regions keep more noise.
inline std::array<double, 2> mergedNoiseVarianceScale(const std::vector<metadata::FrameMetadataSnapshot>& metadata,
                                                      std::uint32_t referenceIndex) noexcept {
    if (referenceIndex >= metadata.size()) return {1.0, 1.0};
    const auto& refMeta = metadata[referenceIndex];
    const double ref = linearExposure(refMeta);
    const std::int32_t refIso = refMeta.sensitivity > 0 ? refMeta.sensitivity : refMeta.requestedSensitivity.value_or(0);
    if (ref <= 0.0 || refIso <= 0) return {1.0, 1.0};
    double sumEf = 0.0, sumShot = 0.0, sumRead = 0.0;
    for (const auto& m : metadata) {
        const double e = linearExposure(m);
        const std::int32_t iso = m.sensitivity > 0 ? m.sensitivity : m.requestedSensitivity.value_or(0);
        if (e <= 0.0 || iso <= 0) continue;
        const double ef = e / ref;
        const double g = double(iso) / double(refIso);
        sumEf += ef;
        sumShot += ef * g;
        sumRead += g * g;
    }
    if (!(sumEf > 0.0)) return {1.0, 1.0};
    const double norm = 1.0 / (sumEf * sumEf);
    return {std::min(1.0, sumShot * norm), std::min(1.0, sumRead * norm)};
}

}  // namespace rawrcam::capture::multiframe
