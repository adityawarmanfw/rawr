#pragma once

#include "encoding/jpeg/JpegCaptureContext.h"
#include <vector>

namespace rawrcam::encoding::jpeg {
std::vector<unsigned char> buildExif(const JpegCaptureContext& context, uint32_t width, uint32_t height);
}