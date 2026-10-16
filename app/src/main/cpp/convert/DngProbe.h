#pragma once
// Metadata probe and RAW thumbnail for the renderer's DNG library.
#include <cstdint>
#include <string>
#include <vector>

namespace rawrcam::convert {

struct DngInfo {
    bool ok = false;
    std::string error;
    uint32_t width = 0, height = 0;  // largest CFA image
    uint16_t bitsPerSample = 0, compression = 0;
    uint16_t orientation = 1;  // EXIF 1..8
    bool rawr = false;         // carries Rawr provenance
    bool convertible = false;  // Bayer 8-16 bit, uncompressed or lossless JPEG
    std::string make, model;
    std::string dateTimeOriginal, offsetTimeOriginal;  // EXIF strings, may be empty
};

DngInfo probeDng(const std::string& path);

// sRGB thumbnail of the RAW, longest side maxSide, unrotated (apply
// DngInfo::orientation). Returns RGB bytes; width/height receive the size.
// Empty on failure.
std::vector<uint8_t> thumbnailDng(const std::string& path, uint32_t maxSide, uint32_t* width, uint32_t* height);

}  // namespace rawrcam::convert
