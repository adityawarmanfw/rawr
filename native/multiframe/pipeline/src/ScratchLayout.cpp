#include <rawr/raw_gpu_pipeline/ScratchLayout.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace rawr::raw_gpu_pipeline {
std::uint32_t bytesPerPixel(PixelStorage s) noexcept {
    switch (s) {
        case PixelStorage::R8Uint:
            return 1;
        case PixelStorage::R16Uint:
        case PixelStorage::R16Float:
            return 2;
        case PixelStorage::R32Float:
            return 4;
        case PixelStorage::RG32Float:
            return 8;
        case PixelStorage::RGBA16Float:
            return 8;
        case PixelStorage::RGBA32Float:
            return 16;
    }
    return 0;
}
}  // namespace rawr::raw_gpu_pipeline
