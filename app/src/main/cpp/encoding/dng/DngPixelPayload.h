#pragma once
#include <tinydng.h>

#include <cstdint>
#include <string>

#include "encoding/dng/DngCaptureContext.h"
#include "encoding/dng/DngWriteParams.h"
#include "imaging/RawSnapshot.h"

namespace rawrcam::encoding::dng {

// Tile edge for lossless-JPEG DNGs (multiple of 16, the TIFF tile rule).
inline constexpr uint32_t kLjpegTileSize = 256;

// Binds the 16-bit CFA payload (tightening padded rows) and sets the image,
// compression and tile/strip layout. Returns false with *error on bad strides.
bool bindDngPixelPayload(TinyDngWriteParams& params, const rawrcam::imaging::RawSnapshot& frame,
                         DngCompression compression, std::string* error);

// Feeds params.pixelData to a streaming writer created with params.tiling:
// edge-cropped tiles (tiled layout) or row strips.
tinydng_status writeTinyDngPayload(tinydng_writer* writer, const TinyDngWriteParams& params, tinydng_error* err);

}  // namespace rawrcam::encoding::dng
