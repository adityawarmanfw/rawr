#pragma once
#include <cstdint>
#include <vector>

#include "tonemap/lut/Lut3D.h"
namespace tonemap::lut {

enum class LutAfterAction : uint32_t { UseDirectly = 0, ConvertToSrgb = 1 };

enum class LutPlacement : uint32_t {
    RenderTransform = 0,  // scene-linear AP1 -> LUT input -> LUT(s) -> declared output -> encoded sRGB
    PostRender = 1,       // encoded sRGB -> LUT input -> LUT(s) -> declared output -> encoded sRGB
};

struct LutChain {
    color::ColorSpace inputSpace{};
    color::ColorSpace outputSpace{};
    std::vector<Lut3D> stages;  // one or more; never insert implicit CSTs between stages
    // PostRender: blend with the incoming render. RenderTransform: the LUT
    // replaces RAWR NTRL outright when > 0 (no blend; NTRL is not computed).
    float intensity = 1.0f;
    LutPlacement placement = LutPlacement::PostRender;
    LutAfterAction afterAction = LutAfterAction::ConvertToSrgb;

    // Pure LUT-chain evaluation in the LUT's own wire domains. Intensity and
    // external CSTs are intentionally handled by the caller because blending
    // across different input/output color spaces is not well-defined here.
    color::Vec3 evaluateStages(color::Vec3 value) const;
};

constexpr uint32_t kMaxGpuLutStages = 8;

}  // namespace tonemap::lut
