#pragma once
#include <cstdint>
#include <vector>

#include "encoding/dng/DngCaptureContext.h"
#include "imaging/RawSnapshot.h"

namespace rawrcam::encoding::dng {

// Serializes the DNGPrivateData provenance block: the replay recipes carried
// by captureContext (embedded verbatim) plus sensor/capture facts from the
// frame. geometryMode names the stored-RAW geometry mapping.
std::vector<uint8_t> buildDngProvenance(const rawrcam::imaging::RawSnapshot& frame,
                                        const DngCaptureContext& captureContext, const char* geometryMode);

}  // namespace rawrcam::encoding::dng
