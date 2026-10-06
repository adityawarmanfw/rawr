#pragma once
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

#include "develop/render/DenoiseProfile.h"
#include "encoding/dng/DngCaptureContext.h"
#include "imaging/RawSnapshot.h"

namespace rawrcam::capture::persistence {

// com.rawrcam.processing.frame.v1: the frame's WB + camera->linear sRGB, so a
// replay renders with the exact color state the capture used.
inline std::string frameRecipe(uint64_t timestampNs, const color::FrameColorTransform& colorState) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9) << "{\"version\":1,\"timestampNs\":" << timestampNs << ",\"whiteBalanceRggb\":[";
    for (size_t i = 0; i < colorState.baselineWbRggb.size(); ++i) out << (i ? "," : "") << colorState.baselineWbRggb[i];
    out << "],\"cameraToLinearSrgbRowMajor\":[";
    for (size_t i = 0; i < colorState.cameraToLinearSrgbRowMajor.size(); ++i)
        out << (i ? "," : "") << colorState.cameraToLinearSrgbRowMajor[i];
    out << "]}";
    return out.str();
}

// com.rawrcam.processing.denoise.v1: the denoise intent plus the noise model
// resolved from this frame's metadata ("enabled":false when unresolvable).
inline std::string denoiseRecipe(const metadata::FrameMetadataSnapshot& metadata, float strength, float detail) {
    float a = 0.0f, b = 0.0f;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(4) << "{\"version\":1,\"enabled\":";
    if (develop::rendered::resolveDenoiseNoise(metadata, a, b)) {
        out << "true,\"strength\":" << strength << ",\"detail\":" << detail << std::setprecision(9) << ",\"a\":" << a
            << ",\"b\":" << b << "}";
    } else {
        out << "false,\"strength\":" << strength << ",\"detail\":" << detail << "}";
    }
    return out.str();
}

// Renders the per-frame replay recipes into the DNG context. Call once the
// frame to be written is known, before DngCaptureWriter::start.
inline void attachFrameRecipes(encoding::dng::DngCaptureContext& context, const imaging::RawSnapshot& frame) {
    context.frameRecipe = frameRecipe(frame.timestampNs, frame.colorState);
    if (context.denoiseEnabled)
        context.denoiseRecipe = denoiseRecipe(frame.metadata, context.denoiseStrength, context.denoiseDetail);
}

}  // namespace rawrcam::capture::persistence
