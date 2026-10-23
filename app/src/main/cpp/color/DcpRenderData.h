#pragma once
#include "color/DcpProfile.h"
namespace rawrcam::color {
// Packed float storage consumed by the GPU DCP stage. Offsets count floats.
std::vector<float> dcpRenderData(const DcpProfile& profile);
}  // namespace rawrcam::color
