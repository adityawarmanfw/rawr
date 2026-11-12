#pragma once
#include <rawr/raw_gpu_pipeline/ResourceArena.h>
#include <rawr/raw_gpu_pipeline/VulkanExecutor.h>
#include <rawr/raw_merge_common/RawMergeCommon.h>

#include <array>
#include <cstdint>

namespace rawr::raw_gpu_pipeline {

// Black / white level of one RAW frame (black per raster phase).
struct RawNormalization {
    std::array<float, 4> blackByPhase{{0, 0, 0, 0}};
    float whiteLevel = 1.0f;
};

struct MultiframeFrameParameters {
    RawNormalization normalization{};
    std::array<float, 3> whiteBalance{{1.0f, 1.0f, 1.0f}};
};

}  // namespace rawr::raw_gpu_pipeline
