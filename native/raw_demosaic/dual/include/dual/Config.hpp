#pragma once
#include "Types.hpp"
#include <cstdint>
namespace dual {
inline constexpr float kRawTherapeeDualContrastMinPercent = 0.0f;
inline constexpr float kRawTherapeeDualContrastMaxPercent = 100.0f;
inline constexpr float kRawTherapeeDualContrastDefaultPercent = 20.0f;
enum class InputMode : uint32_t { NormalizedFloatBuffer=0, RawR16UintImage=1, RawR32FloatImage=2, PackedCfaRgba16fImage=3 };
// Frozen production modes. VNG export+blend fusion is the production default; Baseline is retained only as a validation/reference path.
enum class OptimizationMode : uint32_t {
    Baseline=0,
    VngExportBlend=1
};
struct PipelineConfig {
    uint32_t width=0,height=0;
    BayerPattern pattern=BayerPattern::BGGR;
    InputMode inputMode=InputMode::RawR16UintImage;
    // Rawr/RCD/VNG4 output convention. Internal demosaicers always emit nominal white=1;
    // this scale is applied only after the RCD/VNG4 blend.
    float outputScale=1.0f/255.0f;
    float outputAlpha=1.0f;
    // RawTherapee dual-demosaic contrast slider, in percent.
    // Reference UI range is 0..100, default 20. 0 means RCD-only exactly.
    float contrastPercent=kRawTherapeeDualContrastDefaultPercent;
    // RawTherapee-compatible automatic threshold detection. When enabled, contrastPercent is
    // retained only as the user's manual fallback/override value; the GPU detector resolves
    // the threshold from the current RCD luminance image for each recorded frame.
    bool autoContrast=true;
    bool telemetry=false;
    // Diagnostic-only branch preservation. When false, production allocation/work is unchanged.
    bool diagnosticBranchOutputs=false;
    OptimizationMode optimizationMode=OptimizationMode::VngExportBlend;
    // Blend stages. Demosaicer workgroups retain the frozen upstream package defaults.
    uint32_t pixelWorkgroupX=16,pixelWorkgroupY=16;
    // Same input conditioning as standalone RCD. Production callers enable it;
    // false retains the unbalanced input contract of frozen reference fixtures.
    bool autoBalance=false;
};
struct PipelineAssets {};
}
