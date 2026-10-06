#pragma once
#include "encoding/dng/DngCaptureContext.h"
#include "encoding/dng/DngWriteParams.h"
#include "imaging/RawSnapshot.h"

namespace rawrcam::encoding::dng {

// Fills the EXIF sidecar (camera/lens identity, description, orientation,
// exposure, optics, zoom and capture timestamps) from frame metadata and the
// shutter-time capture context. Owned strings land in params; the caller
// rebinds afterwards.
void fillDngExif(TinyDngWriteParams& params, const rawrcam::imaging::RawSnapshot& frame,
                 const DngCaptureContext& captureContext);

}  // namespace rawrcam::encoding::dng
