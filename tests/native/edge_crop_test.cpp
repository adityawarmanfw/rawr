// Padded raw edges (lens profile image.cropEdges): the packed RAW a single still keeps, and the merged JPEG's RGBA.
#include <cassert>
#include <cstring>
#include <memory>
#include <vector>

#include "capture/multiframe/MergedEdgeCrop.h"
#include "capture/single/StillEdgeCrop.h"

using namespace rawrcam;

static uint16_t pixelAt(const imaging::RawSnapshot& f, uint32_t x, uint32_t y) {
    uint16_t v;
    std::memcpy(&v, f.raw16.data() + size_t(y) * f.packedRowStrideBytes + size_t(x) * 2, 2);
    return v;
}

static imaging::RawSnapshot snapshot(uint32_t w, uint32_t h, uint32_t left, uint32_t top, uint32_t right,
                                     uint32_t bottom) {
    imaging::RawSnapshot f;
    f.requestId = 7;
    f.width = w;
    f.height = h;
    f.packedRowStrideBytes = w * 2;
    f.raw16.resize(size_t(w) * h * 2);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint16_t v = uint16_t(y * 100 + x);
            std::memcpy(f.raw16.data() + (size_t(y) * w + x) * 2, &v, 2);
        }
    auto context = std::make_shared<metadata::CameraContextMetadata>();
    context->cropLeft = left;
    context->cropTop = top;
    context->cropRight = right;
    context->cropBottom = bottom;
    context->geometry.rawBufferWidth = w;
    context->geometry.rawBufferHeight = h;
    context->geometry.pixelArrayWidth = w;
    context->geometry.pixelArrayHeight = h;
    context->geometry.preCorrectionActiveArray = {0, 0, int32_t(w), int32_t(h), true};
    context->geometry.activeArray = {0, 0, int32_t(w), int32_t(h), true};
    f.metadata.cameraContext = context;
    return f;
}

int main() {
    // ---- a single still: drops the edges, keeps the colour order, moves the geometry with the pixels
    {
        auto f = snapshot(16, 12, 2, 4, 2, 2);
        capture::keepCroppedEdges(f);
        assert(f.width == 12 && f.height == 6 && f.packedRowStrideBytes == 24 && f.raw16.size() == 12 * 6 * 2);
        assert(pixelAt(f, 0, 0) == 4 * 100 + 2 && pixelAt(f, 11, 5) == 9 * 100 + 13);  // top-left and bottom-right kept
        const auto& c = *f.metadata.cameraContext;
        assert(c.cropLeft == 0 && c.cropTop == 0 && c.cropRight == 0 && c.cropBottom == 0);
        assert(c.geometry.rawBufferWidth == 12 && c.geometry.rawBufferHeight == 6);
        assert(c.geometry.pixelArrayWidth == 12 && c.geometry.pixelArrayHeight == 6);
        assert(c.geometry.activeArray.right == 12 && c.geometry.activeArray.bottom == 6 && c.geometry.activeArray.left == 0);
        capture::keepCroppedEdges(f);  // idempotent: the crop is spent
        assert(f.width == 12 && f.height == 6);
    }
    // no crop, no context, or edges that leave nothing: untouched
    {
        auto none = snapshot(16, 12, 0, 0, 0, 0);
        const auto before = none.raw16;
        capture::keepCroppedEdges(none);
        assert(none.width == 16 && none.raw16 == before);
        imaging::RawSnapshot bare;
        bare.width = 16;
        bare.height = 12;
        capture::keepCroppedEdges(bare);
        assert(bare.width == 16);
        auto absurd = snapshot(16, 12, 8, 0, 8, 0);
        capture::keepCroppedEdges(absurd);
        assert(absurd.width == 16 && absurd.metadata.cameraContext->cropLeft == 8);
    }

    // ---- the merged JPEG's RGBA8: scaled from raw edges to the image's own size
    {
        std::vector<uint8_t> rgba(16 * 12 * 4);
        for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = uint8_t(i);
        using capture::multiframe::keepRgba8Edges;
        // nothing dropped: the very same buffer, no copy
        const auto same = keepRgba8Edges(rgba.data(), rgba.size(), 16, 12, 16, 12, 0, 0, 0, 0);
        assert(!same.cropped && same.data == rgba.data() && same.width == 16 && same.height == 12);
        // rows only (the 17U / Xiaomi 15 blank bottom rows): the kept rows are contiguous, so no copy either
        const auto rows = keepRgba8Edges(rgba.data(), rgba.size(), 16, 12, 16, 12, 0, 2, 0, 4);
        assert(rows.cropped && rows.width == 16 && rows.height == 6 && rows.bytes == 16 * 6 * 4);
        assert(rows.data == rgba.data() + 2 * 16 * 4 && rows.storage.empty());
        // columns: copied, row by row
        const auto cols = keepRgba8Edges(rgba.data(), rgba.size(), 16, 12, 16, 12, 2, 0, 4, 0);
        assert(cols.cropped && cols.width == 10 && cols.height == 12 && cols.bytes == 10 * 12 * 4);
        assert(static_cast<const uint8_t*>(cols.data)[0] == rgba[2 * 4] &&
               static_cast<const uint8_t*>(cols.data)[10 * 4] == rgba[16 * 4 + 2 * 4]);
        // a quarter-size gain map of the same frame lands on the same window
        std::vector<uint8_t> map(4 * 3 * 4, 1);
        const auto small = keepRgba8Edges(map.data(), map.size(), 4, 3, 16, 12, 0, 4, 0, 4);
        assert(small.cropped && small.width == 4 && small.height == 1);
        // edges that leave nothing, or a buffer that is not rows of this width: unchanged
        assert(!keepRgba8Edges(rgba.data(), rgba.size(), 16, 12, 16, 12, 0, 6, 0, 6).cropped);
        assert(!keepRgba8Edges(rgba.data(), rgba.size() - 1, 16, 12, 16, 12, 0, 2, 0, 0).cropped);
        assert(!keepRgba8Edges(nullptr, 0, 16, 12, 16, 12, 0, 2, 0, 0).cropped);
    }
    return 0;
}
