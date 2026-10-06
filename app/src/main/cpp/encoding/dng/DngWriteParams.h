#pragma once
#include <tinydng.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rawrcam::encoding::dng {

// Owned TinyDNG write parameters. All tinydng_* structs borrow from the
// owning vectors/strings below; the whole bundle outlives the write.
struct TinyDngWriteParams {
    tinydng_cfa cfa{};
    tinydng_raw_info raw{};
    tinydng_exif exif{};
    tinydng_write_image image{};
    tinydng_write_options options{};
    tinydng_tiling tiling{};

    // Pixel payload: points into the snapshot when tightly packed, else into
    // packedPixels.
    const uint8_t* pixelData = nullptr;
    size_t pixelBytes = 0;
    std::vector<uint8_t> packedPixels;

    std::vector<tinydng_opcode> opcodes;
    std::vector<std::vector<uint8_t>> opcodeParams;
    std::vector<uint8_t> privateData;

    // Owned string storage backing tinydng char* fields.
    std::string uniqueModel, make, model, software, datetime, description;
    std::string dateOriginal, dateDigitized, offsetTime, offsetOrig, offsetDig;
    std::string subsec, subsecOrig, subsecDig;
    std::string bodySerial, lensMake, lensModel, lensSerial, cameraSerial;

    // Re-point all tinydng char*/data pointers at owned storage. Must be
    // called after any move (short strings may live inline and not survive
    // relocation); vectors never SSO so their pointers survive moves.
    void rebind() noexcept;
};

}  // namespace rawrcam::encoding::dng
