#pragma once
#include <algorithm>
#include "tonemap/TonemapMath.h"

namespace tonemap {
// CPU reference for neutral direct display profiles, AgX and LOG (which
// ignores creative controls). Input is calibrated scene-linear AP1 before AE gain.
inline color::Vec3 renderDirectReference(color::Vec3 ap1, const TonemapParams& p) {
    const bool log = p.renderTransform == RenderTransform::Log;
    if (p.renderTransform == RenderTransform::Existing)
        throw std::invalid_argument("direct reference requires a direct transform");
    const float gain = p.aePostGain * (log ? 1.0f : std::exp2(p.exposureEV));
    for (float& v : ap1) v *= gain;
    const color::ColorSpace source{color::Gamut::AcesCgAp1, color::TransferFunction::Linear};
    if (log) return color::convert(ap1, source, p.outputSpace);
    const bool agx = p.renderTransform == RenderTransform::AgX;
    auto linear = agx ? agxRenderReference(ap1, agxWithHighlights(p.agx, p.highlightBiasEV))
                      : color::convert(ap1, source, {color::Gamut::SRgbRec709, color::TransferFunction::Linear});
    for (float& v : linear) v = std::clamp(v, 0.0f, 1.0f);
    const bool rec709 = p.renderTransform == RenderTransform::Rec709 ||
                        (agx && p.outputSpace.transfer == color::TransferFunction::Rec709);
    return color::encodeTransfer(linear, rec709 ? color::TransferFunction::Rec709 : color::TransferFunction::SRgb);
}
}  // namespace tonemap
