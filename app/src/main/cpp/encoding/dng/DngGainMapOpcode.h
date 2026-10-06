#pragma once
#include <cstdint>
#include <string>

#include "encoding/dng/DngWriteParams.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::encoding::dng {

// Active-area rectangle in stored-image pixels plus the CFA code at the
// stored-image origin; the GainMap phase per channel derives from both.
struct DngGainMapArea {
    uint32_t top = 0, left = 0, height = 0, width = 0;
    uint32_t storedPattern = 0;
};

// Appends four GainMap opcodes (OpcodeList2, one per Camera2 lens-shading
// channel R/G_even/G_odd/B) built from metadata.lensShadingMap. Returns false
// with *error on an invalid map or unresolvable CFA phase.
bool appendLensShadingGainMaps(TinyDngWriteParams& params, const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                               const DngGainMapArea& area, std::string* error);

}  // namespace rawrcam::encoding::dng
