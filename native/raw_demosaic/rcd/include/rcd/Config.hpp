#pragma once
#include "Types.hpp"
#include <cstdint>
#include <array>

namespace rcd {

enum class InputMode : uint32_t {
    NormalizedFloatBuffer = 0,
    RawR16UintImage = 1,
    RawR32FloatImage = 2,
    PackedCfaRgba16fImage = 3,
};

struct PipelineConfig {
    uint32_t width=0;
    uint32_t height=0;
    BayerPattern pattern=BayerPattern::BGGR;
    InputMode inputMode=InputMode::RawR16UintImage;

    // Converts the 0..255 working domain to nominal 0..1 output.
    // RCD internally works in nominal 0..1 units, so the exporter applies
    // outputScale*255. Default 1/255 therefore emits nominal white == 1.
    float outputScale=1.0f/255.0f;
    float outputAlpha=1.0f;
    bool telemetry=false;

    // Production-tuned Adreno 840 defaults, frozen after the 0.8.x sweep.
    // workgroupX/Y remain the fallback for callers that explicitly clear a
    // per-stage value to zero. The shader set must be compiled with the same
    // resolved local sizes.
    uint32_t workgroupX=16;
    uint32_t workgroupY=16;
    uint32_t directionWorkgroupX=4;
    uint32_t directionWorkgroupY=32;
    uint32_t greenWorkgroupX=4;
    uint32_t greenWorkgroupY=32;
    uint32_t diagonalWorkgroupX=32;
    uint32_t diagonalWorkgroupY=8;
    uint32_t greenSitesWorkgroupX=8;
    uint32_t greenSitesWorkgroupY=32;
    uint32_t exportWorkgroupX=4;
    uint32_t exportWorkgroupY=32;
    // Robust frame balance inside RCD; export restores camera-linear RGB.
    bool autoBalance=false;
    // Fixed frame balance for tiled imports; ignored when autoBalance is true.
    std::array<float,3> inputBalance{1,1,1};
};

struct PipelineAssets {};

} // namespace rcd
