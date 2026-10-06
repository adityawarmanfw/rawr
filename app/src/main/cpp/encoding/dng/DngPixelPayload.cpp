#include "encoding/dng/DngPixelPayload.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rawrcam::encoding::dng {

bool bindDngPixelPayload(TinyDngWriteParams& p, const rawrcam::imaging::RawSnapshot& frame,
                         DngCompression compression, std::string* error) {
    // --- Pixels (tighten stride when padded) ---
    const uint64_t tightRow = uint64_t(frame.width) * 2u;
    if (frame.packedRowStrideBytes == tightRow) {
        p.pixelData = frame.raw16.data();
        p.pixelBytes = frame.raw16.size();
    } else {
        if (frame.packedRowStrideBytes < tightRow) {
            if (error) *error = "packed row stride smaller than row";
            return false;
        }
        p.packedPixels.resize(size_t(tightRow) * frame.height);
        for (uint32_t y = 0; y < frame.height; ++y) {
            std::memcpy(p.packedPixels.data() + size_t(y) * size_t(tightRow),
                        frame.raw16.data() + size_t(y) * size_t(frame.packedRowStrideBytes), size_t(tightRow));
        }
        p.pixelData = p.packedPixels.data();
        p.pixelBytes = p.packedPixels.size();
    }
    if (p.pixelBytes < size_t(tightRow) * frame.height) {
        if (error) *error = "RAW buffer smaller than geometry";
        return false;
    }

    // --- Image + options ---
    tinydng_write_image& img = p.image;
    img.width = frame.width;
    img.height = frame.height;
    img.samples_per_pixel = 1;
    img.bits_per_sample = 16;
    img.photometric = 0;  // auto -> CFA
    img.data = p.pixelData;
    img.data_size = size_t(tightRow) * frame.height;
    img.cfa = &p.cfa;
    img.raw = &p.raw;
    img.exif = &p.exif;

    const bool ljpeg = compression != DngCompression::Uncompressed;
    p.options.big_endian = 0;
    p.options.as_dng = 1;
    p.options.compression = ljpeg ? 7 : 1;
    p.options.ljpeg_predictor = 1;
    const uint32_t rowBytes = uint32_t(tightRow);
    if (ljpeg) {
        // Lossless JPEG uses 256x256 tiles like Adobe DNG Converter: valid
        // multi-strip LJPEG decodes in the Adobe SDK, but LibRaw/RawTherapee
        // only read the first strip of a compressed DNG.
        p.tiling.tile_width = kLjpegTileSize;
        p.tiling.tile_length = kLjpegTileSize;
        p.tiling.rows_per_strip = 0;
    } else {
        // Uncompressed keeps the legacy 64KB/strip layout.
        p.tiling.tile_width = 0;
        p.tiling.tile_length = 0;
        p.tiling.rows_per_strip = std::max<uint32_t>(1u, 65536u / rowBytes);
        if (p.tiling.rows_per_strip > frame.height) p.tiling.rows_per_strip = frame.height;
    }
    return true;
}

tinydng_status writeTinyDngPayload(tinydng_writer* writer, const TinyDngWriteParams& params, tinydng_error* err) {
    const uint32_t width = params.image.width, height = params.image.height;
    const uint64_t tightRow = uint64_t(width) * 2u;
    tinydng_status st = TINYDNG_OK;
    if (params.tiling.tile_width > 0u && params.tiling.tile_length > 0u) {
        const uint32_t tw = params.tiling.tile_width, th = params.tiling.tile_length;
        const uint32_t across = (width + tw - 1u) / tw, down = (height + th - 1u) / th;
        std::vector<uint8_t> tile(size_t(tw) * th * 2u);
        for (uint32_t t = 0; t < across * down && st == TINYDNG_OK; ++t) {
            const uint32_t x = (t % across) * tw, y = (t / across) * th;
            const uint32_t w = std::min(tw, width - x), h = std::min(th, height - y);
            for (uint32_t r = 0; r < h; ++r)
                std::memcpy(tile.data() + size_t(r) * w * 2u, params.pixelData + uint64_t(y + r) * tightRow + uint64_t(x) * 2u,
                            size_t(w) * 2u);
            st = tinydng_writer_write_tile(writer, t, tile.data(), err);
        }
        return st;
    }
    const uint32_t rowsPerStrip = params.tiling.rows_per_strip ? params.tiling.rows_per_strip : height;
    const uint32_t stripCount = (height + rowsPerStrip - 1u) / rowsPerStrip;
    for (uint32_t s = 0; s < stripCount && st == TINYDNG_OK; ++s)
        st = tinydng_writer_write_strip(writer, s, params.pixelData + uint64_t(s) * rowsPerStrip * tightRow, err);
    return st;
}

}  // namespace rawrcam::encoding::dng
