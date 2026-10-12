#pragma once
#include <cstddef>
#include <cstdint>
#include <tonemap/TonemapMath.h>

namespace rawrcam::tonemap_integration {

enum class ColorRenderProfile : std::uint8_t {
    RawrBase = 0,
    UserLut = 1,
    SRgb = 2,
    Rec709 = 3,
    LogC3 = 4,
    SLog3 = 5,
    VLog = 6,
    FLog2C = 7,
    DaVinciIntermediate = 8,
    // AgX writes sRGB for stills/preview and Rec.709 for video.
    AgX = 9,
    AgXRec709 = 10,
};

// AgX values sent from the app, in tonemap::AgxControl order.
inline constexpr std::size_t kAgxControlCount = tonemap::kAgxControlCount;

void applyRenderProfile(ColorRenderProfile profile, tonemap::TonemapParams& params) noexcept;

[[nodiscard]] const char* colorRenderProfileName(ColorRenderProfile profile) noexcept;
[[nodiscard]] bool colorRenderProfileFromId(std::uint32_t id, ColorRenderProfile* out) noexcept;
[[nodiscard]] bool isAgxProfile(ColorRenderProfile profile) noexcept;
// AgX parameters from the app's kAgxControlCount controls; false when invalid.
[[nodiscard]] bool agxFromControls(const float* values, tonemap::AgxParams* out) noexcept;

}  // namespace rawrcam::tonemap_integration
