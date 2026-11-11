#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "capture/single/StillBinnedCorner.h"
#include "capture/single/StillCfaRemosaic.h"

namespace {
using namespace rawrcam;
constexpr uint32_t kBase = 64;

// Constant colour planes in either Bayer or a same-colour block mosaic.
std::vector<uint8_t> mosaic(uint32_t size, uint32_t block) {
    std::vector<uint8_t> bytes(size_t(size) * size * 2);
    auto* pixels = reinterpret_cast<uint16_t*>(bytes.data());
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
            pixels[size_t(y) * size + x] = (y / block) % 2 == 0
                ? ((x / block) % 2 == 0 ? 900 : 500) : ((x / block) % 2 == 0 ? 500 : 100);
    return bytes;
}

struct Shot {
    std::shared_ptr<metadata::CameraContextMetadata> base, photo;
    imaging::RawSnapshot frame;
};

Shot shot(uint32_t scale, uint32_t baseBlock, uint32_t photoBlock, uint32_t cornerSize) {
    Shot out;
    out.base = std::make_shared<metadata::CameraContextMetadata>();
    out.base->geometry.pixelArrayWidth = out.base->geometry.pixelArrayHeight = kBase;
    // The configured RAW buffer may include padding outside the sensor array.
    out.base->geometry.rawBufferWidth = kBase + 16;
    out.base->geometry.rawBufferHeight = kBase;
    out.base->cfaBlock = baseBlock;
    out.base->cfaPhotoQuality = 1;
    out.photo = std::make_shared<metadata::CameraContextMetadata>(*out.base);
    out.photo->binnedFallbackContext = out.base;
    out.photo->cfaBlock = out.photo->photoCfaBlock = photoBlock;
    out.photo->geometry.rawBufferWidth = out.photo->geometry.rawBufferHeight = kBase * scale;
    auto& frame = out.frame;
    frame.width = frame.height = kBase * scale;
    frame.packedRowStrideBytes = frame.width * 2;
    frame.metadata.cameraContext = out.photo;
    frame.raw16.resize(size_t(frame.width) * frame.height * 2);
    const auto corner = mosaic(cornerSize, cornerSize == frame.width ? photoBlock : baseBlock);
    for (uint32_t y = 0; y < cornerSize; ++y)
        std::memcpy(frame.raw16.data() + size_t(y) * frame.width * 2,
                    corner.data() + size_t(y) * cornerSize * 2, size_t(cornerSize) * 2);
    return out;
}

void testKnownBinnedLayout() {
    for (uint32_t scale : {2u, 4u}) {
        for (uint32_t baseBlock : {1u, 2u}) {
            auto s = shot(scale, baseBlock, scale, kBase);
            auto expected = mosaic(kBase, baseBlock);
            imaging::remosaicBlockCfa(expected, kBase, kBase, baseBlock, 0, 1);
            const auto before = capture::fullSizeFallbacks().binnedStills.load();
            capture::keepBinnedCorner(s.frame);
            assert(s.frame.width == kBase && s.frame.height == kBase && s.frame.packedRowStrideBytes == kBase * 2);
            const auto& cropped = s.frame.metadata.cameraContext;
            assert(cropped != s.photo && cropped != s.base);
            assert(cropped->cfaBlock == baseBlock && cropped->geometry.rawBufferWidth == kBase);
            capture::remosaicBlockCfaStill(s.frame);
            assert(s.frame.raw16 == expected);
            assert(capture::fullSizeFallbacks().binnedStills.load() == before + 1);
            // Only this snapshot changes; the shared session layouts stay intact.
            assert(s.photo->cfaBlock == scale && s.photo->geometry.rawBufferWidth == kBase * scale);
            assert(s.base->cfaBlock == baseBlock && s.base->geometry.rawBufferWidth == kBase + 16);
        }
    }
}

void testFullResolutionStillRemosaics() {
    for (uint32_t block : {2u, 4u}) {
        auto s = shot(block, 1, block, kBase * block);
        const auto original = s.frame.raw16;
        auto expected = original;
        imaging::remosaicBlockCfa(expected, s.frame.width, s.frame.height, block, 0, 1);
        const auto before = capture::fullSizeFallbacks().binnedStills.load();
        capture::keepBinnedCorner(s.frame);
        assert(s.frame.metadata.cameraContext == s.photo && s.frame.raw16 == original);
        capture::remosaicBlockCfaStill(s.frame);
        assert(s.frame.raw16 == expected && s.frame.raw16 != original);
        assert(s.frame.width == kBase * block);
        assert(capture::fullSizeFallbacks().binnedStills.load() == before);
    }
}

void testUnusualCornerSizeKeepsLegacyCrop() {
    // The old detector accepts a half-size corner even in a 4x photo buffer.
    auto s = shot(4, 1, 4, kBase * 2);
    const auto expected = mosaic(kBase * 2, 1);
    capture::keepBinnedCorner(s.frame);
    assert(s.frame.width == kBase * 2 && s.frame.height == kBase * 2);
    assert(s.frame.packedRowStrideBytes == kBase * 4);
    assert(s.frame.metadata.cameraContext->cfaBlock == 1);
    capture::remosaicBlockCfaStill(s.frame);
    assert(s.frame.raw16 == expected);
    assert(s.photo->cfaBlock == 4);
}

void testNoSeparatePhotoContextKeepsExistingCfa() {
    auto s = shot(2, 1, 1, kBase);
    s.photo->binnedFallbackContext.reset();
    const auto expected = mosaic(kBase, 1);
    capture::keepBinnedCorner(s.frame);
    assert(s.frame.width == kBase && s.frame.height == kBase);
    assert(s.frame.metadata.cameraContext != s.photo);
    assert(s.frame.metadata.cameraContext->cfaBlock == 1);
    capture::remosaicBlockCfaStill(s.frame);
    assert(s.frame.raw16 == expected);
}
}  // namespace

int main() {
    testKnownBinnedLayout();
    testFullResolutionStillRemosaics();
    testUnusualCornerSizeKeepsLegacyCrop();
    testNoSeparatePhotoContextKeepsExistingCfa();
    std::cout << "still_binned_fallback_test passed\n";
}
