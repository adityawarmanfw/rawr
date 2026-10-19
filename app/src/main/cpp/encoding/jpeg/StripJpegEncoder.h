#pragma once

#include <cstddef>
#include <string>

namespace rawrcam::encoding::jpeg {

// Baseline JPEG of an RGBA8 image, encoded as horizontal strips on several threads.
//
// Each strip is a whole number of MCU rows, encoded by its own TurboJPEG handle with the same settings (quality,
// subsampling, accurate DCT, standard Huffman tables). The strips' entropy-coded segments are joined into one baseline
// JPEG: strip 0's headers with the full image height in SOF, a DRI marker (restart interval = one strip's MCUs) and
// RST0..7 between the segments. Restart markers only reset the DC predictors, and neither the DCT nor TurboJPEG's
// 2x2 chroma downsampling crosses an MCU-row boundary, so the decoded pixels are exactly those of a one-thread
// tj3Compress8 with the same settings; the file is a few bytes per strip larger.
//
// Returns false (and leaves *jpeg null) when the image is too small to split, the strips' tables differ, or any
// strip fails: the caller then encodes on one thread as before. On success *jpeg is allocated with tj3Alloc and must
// be released with tj3Free.
bool encodeStripsRgba8(const unsigned char* rgba, int width, int height, int quality, int tjSubsamp,
                       unsigned char** jpeg, std::size_t* jpegSize, int* stripsUsed, std::string* error);

}  // namespace rawrcam::encoding::jpeg
