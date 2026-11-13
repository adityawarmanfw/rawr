#pragma once
#include <rawr/raw_merge_common/RawMergeCommon.h>

#include <cstdint>
#include <optional>

namespace rawr::raw_gpu_pipeline {

// Per-block layout written by noise_estimate.comp: 4 CFA sites (row-major
// physical site order) x (count, sumLevel, sumDiff, sumDiffSquared).
inline constexpr std::uint32_t kNoiseBlockSize = 32u;
inline constexpr std::uint32_t kNoiseBlockFloats = 16u;

struct BurstNoiseFit {
    rawr::raw_merge_common::CfaNoiseProfile profile{};
    std::uint32_t usableBlocks = 0;
    std::uint32_t bins = 0;
};

// Fits variance = slope * x + offset per CFA site from the difference of two
// consecutive burst frames. Each brightness bin keeps a low quantile of the
// block variances (flat, static blocks; texture, misalignment and motion only
// add variance) corrected for that quantile's bias. Returns nullopt when the
// frame does not span enough usable levels for a stable fit.
std::optional<BurstNoiseFit> fitBurstNoise(const float* blockStats, std::uint32_t blockCount);

}  // namespace rawr::raw_gpu_pipeline
