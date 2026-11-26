// Host-test stand-in for BlockRemosaicGpu: the still-capture tests never run a block-CFA remosaic on the GPU,
// but SingleShotSlot reaches the class through remosaicBlockCfaBuffer, so the symbols must exist to link.
#include "capture/BlockRemosaicGpu.h"

namespace rawrcam::capture {
bool BlockRemosaicGpu::supports(uint32_t, imaging::CfaRemosaicQuality) { return false; }
void BlockRemosaicGpu::run(std::vector<uint8_t>&, uint32_t, uint32_t, imaging::CfaRemosaicQuality, float,
                           const imaging::QuadGainField*, bool) {
    throw std::logic_error("BlockRemosaicGpu is not available in host tests");
}
}  // namespace rawrcam::capture
