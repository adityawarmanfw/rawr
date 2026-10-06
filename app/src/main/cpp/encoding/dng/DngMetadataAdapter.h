#pragma once
#include <optional>
#include <string>

#include "encoding/dng/DngCaptureContext.h"
#include "encoding/dng/DngPixelPayload.h"
#include "encoding/dng/DngWriteParams.h"
#include "imaging/RawSnapshot.h"

namespace rawrcam::encoding::dng {

// Builds the complete TinyDNG write bundle for one frame: stored-RAW geometry,
// CFA, levels, color calibration and noise profile here, plus the lens-shading
// GainMap (DngGainMapOpcode), EXIF (DngExif), provenance (DngProvenance) and
// pixel payload (DngPixelPayload).
std::optional<TinyDngWriteParams> makeTinyDngWriteParams(const rawrcam::imaging::RawSnapshot& frame,
                                                         const DngCaptureContext& captureContext, std::string* error);

}  // namespace rawrcam::encoding::dng
