#include "ColorRenderProfile.h"

namespace rawrcam::tonemap_integration {
const char* colorRenderProfileName(ColorRenderProfile profile) noexcept {
    switch (profile) {
        case ColorRenderProfile::RawrBase: return "rawr_base";
        case ColorRenderProfile::UserLut: return "user_lut";
        case ColorRenderProfile::SRgb: return "srgb";
        case ColorRenderProfile::Rec709: return "rec709";
        case ColorRenderProfile::LogC3: return "logc3";
        case ColorRenderProfile::SLog3: return "slog3";
        case ColorRenderProfile::VLog: return "vlog";
        case ColorRenderProfile::FLog2C: return "flog2c";
        case ColorRenderProfile::DaVinciIntermediate: return "davinci_intermediate";
        case ColorRenderProfile::AgX: return "agx";
        case ColorRenderProfile::AgXRec709: return "agx_rec709";
    }
    return "unknown";
}

bool colorRenderProfileFromId(std::uint32_t id, ColorRenderProfile* out) noexcept {
    if (!out || id > 10) return false;
    *out = static_cast<ColorRenderProfile>(id);
    return true;
}

bool isAgxProfile(ColorRenderProfile profile) noexcept {
    return profile == ColorRenderProfile::AgX || profile == ColorRenderProfile::AgXRec709;
}

bool agxFromControls(std::uint32_t preset, const float* values, tonemap::AgxParams* out) noexcept {
    if (!values || !out || preset >= tonemap::kAgxPresetCount) return false;
    tonemap::AgxParams p = tonemap::agxPreset(static_cast<tonemap::AgxPreset>(preset));
    p.whiteEv = values[0];
    p.shoulderPower = values[1];
    p.toePower = values[2];
    p.primaries.masterOutset = values[3];
    p.preserveHue = values[4];
    if (!tonemap::validAgx(p)) return false;
    *out = p;
    return true;
}

void applyRenderProfile(ColorRenderProfile profile, tonemap::TonemapParams& params) noexcept {
    using namespace tonemap;
    using namespace tonemap::color;
    params.renderTransform = RenderTransform::Existing;
    params.outputSpace = {};
    switch (profile) {
        case ColorRenderProfile::RawrBase:
        case ColorRenderProfile::UserLut: return;
        case ColorRenderProfile::SRgb:
            params.renderTransform = RenderTransform::SRgb;
            return;
        case ColorRenderProfile::AgX:
            params.renderTransform = RenderTransform::AgX;
            return;
        case ColorRenderProfile::AgXRec709:
            params.renderTransform = RenderTransform::AgX;
            params.outputSpace = {Gamut::SRgbRec709, TransferFunction::Rec709};
            return;
        case ColorRenderProfile::Rec709:
            params.renderTransform = RenderTransform::Rec709;
            params.outputSpace = {Gamut::SRgbRec709, TransferFunction::Rec709};
            return;
        case ColorRenderProfile::LogC3:
            params.outputSpace = {Gamut::ArriWideGamut3, TransferFunction::LogC3}; break;
        case ColorRenderProfile::SLog3:
            params.outputSpace = {Gamut::SonySGamut3Cine, TransferFunction::SLog3}; break;
        case ColorRenderProfile::VLog:
            params.outputSpace = {Gamut::PanasonicVGamut, TransferFunction::VLog}; break;
        case ColorRenderProfile::FLog2C:
            params.outputSpace = {Gamut::FujifilmFGamutC, TransferFunction::FLog2C}; break;
        case ColorRenderProfile::DaVinciIntermediate:
            params.outputSpace = {Gamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate}; break;
    }
    params.renderTransform = RenderTransform::Log;
}
}  // namespace rawrcam::tonemap_integration
