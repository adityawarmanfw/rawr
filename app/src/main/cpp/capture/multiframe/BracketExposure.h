#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "metadata/FrameMetadataSnapshot.h"

// Exposure bookkeeping shared by the merges: the linear exposure of a frame and the merged noise scale. Only ratios
// between exposures of one capture are used, so the unit is arbitrary.
namespace rawrcam::capture::multiframe {

inline constexpr std::uint32_t kMergeAlgorithmHdrPlusQuality = 2u;
// Rexture (native/multiframe/merge_rexture): equal-exposure bursts at native resolution. One merge window holds up
// to kRextureMaxPixels (~72 bytes of arena per pixel: ~0.91 GiB at 13 MP, ~3.45 GiB at 50 MP on the device, between
// HDR+ Quality). Larger frames merge in strips of that size (MfsrCaptureJob, DNG only), like the other
// merges; keep this equal to the strip budget there.
inline constexpr std::uint32_t kMergeAlgorithmRexture = 4u;
inline constexpr std::uint64_t kRextureMaxPixels = 56'000'000ull;

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

// Variance scale of an HDR+ Quality merge relative to its reference frame, for the
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
