#pragma once
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

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

// Renders the per-frame replay recipe into the DNG context. Call once the
// frame to be written is known, before DngCaptureWriter::start.
inline void attachFrameRecipes(encoding::dng::DngCaptureContext& context, const imaging::RawSnapshot& frame) {
    context.frameRecipe = frameRecipe(frame.timestampNs, frame.colorState);
}

}  // namespace rawrcam::capture::persistence
