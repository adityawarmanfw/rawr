#include <cassert>
#include <cmath>
#include <string>

#include "capture/ReducedRawMetadata.h"
#include "raw_denoise/DenoisePipeline.hpp"
using namespace rawrcam;
int main() {
    auto c = std::make_shared<metadata::CameraContextMetadata>();
    c->geometry.rawBufferWidth = 16320;
    c->geometry.rawBufferHeight = 12288;
    c->geometry.pixelArrayWidth = 4080;
    c->geometry.pixelArrayHeight = 3072;
    c->geometry.activeArray = {0, 0, 4080, 3072, true};
    c->cfaBlock = c->photoCfaBlock = 4;
    c->photoReduced = c->photoPhaseFit = true;
    c->binnedFallbackContext = std::make_shared<metadata::CameraContextMetadata>();
    c->baselineBlackLevelPhysicalRggb = {63, 64, 65, 66};
    c->baselineWhiteLevel = 1023;
    metadata::FrameMetadataSnapshot m;
    m.cameraContext = c;
    m.blackLevelPhysicalRggb = c->baselineBlackLevelPhysicalRggb;
    m.staticWhiteLevel = 1023;
    m.effectiveWhiteLevel = 1000;
    m.reportedDynamicWhiteLevel = 1000;
    m.sensorNoiseProfile.assign(8, .01);
    m.hotPixelMap = {14, 16};
    m.sensitivity = 400;
    m.exposureTimeNs = 10000000;
    m.lensShadingMap = {1, 2, 3, 4};
    capture::applyReducedRawMetadata(m);
    assert(c->geometry.rawBufferWidth == 16320 && c->baselineWhiteLevel == 1023 && c->photoReduced);
    assert(m.cameraContext->geometry.rawBufferWidth == 8160 && m.cameraContext->geometry.rawBufferHeight == 6144);
    assert(m.cameraContext->geometry.pixelArrayWidth == 4080 && m.cameraContext->geometry.activeArray.right == 4080);
    assert(m.cameraContext->cfaBlock == 1 && !m.cameraContext->binnedFallbackContext);
    assert(m.staticWhiteLevel == 4092 && m.effectiveWhiteLevel == 4000 && *m.reportedDynamicWhiteLevel == 4000);
    for (unsigned i = 0; i < 4; ++i) {
        assert(m.blackLevelPhysicalRggb[i] == 4 * c->baselineBlackLevelPhysicalRggb[i]);
        // Normalized flat-field brightness is invariant under quarter-DN storage.
        const float before =
            (300 - c->baselineBlackLevelPhysicalRggb[i]) / (1000 - c->baselineBlackLevelPhysicalRggb[i]);
        const float after =
            (1200 - m.blackLevelPhysicalRggb[i]) / (m.effectiveWhiteLevel - m.blackLevelPhysicalRggb[i]);
        assert(std::abs(before - after) < 1e-7f);
    }
    assert(m.hotPixelMap.empty());
    // The noise profile is kept, and the denoise mapping then scales a by 1/4 with the x4 levels.
    assert(m.sensorNoiseProfile.size() == 8 && m.sensorNoiseProfile[0] == .01);
    {
        const double profile[8] = {.002, 1e-6, .002, 1e-6, .002, 1e-6, .002, 1e-6};
        float a0 = 0, b0 = 0, a1 = 0, b1 = 0;
        const float black0 = c->baselineBlackLevelPhysicalRggb[1], black1 = m.blackLevelPhysicalRggb[1];
        assert(raw_denoise::GreenNoiseToNormalized(profile, 8, 0, black0, 1023.f, a0, b0));
        assert(raw_denoise::GreenNoiseToNormalized(profile, 8, 0, black1, 4092.f, a1, b1));
        assert(std::abs(a1 * 4.f / a0 - 1.f) < 1e-3f);
    }
    assert(m.sensitivity == 400 && m.exposureTimeNs == 10000000 && m.lensShadingMap.size() == 4);
    bool rejected = false;
    try {
        capture::applyReducedRawMetadata(m);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);  // calibration cannot be scaled twice
    // Support decision made once at configure time (shared by stills and bursts).
    {
        auto d = std::make_shared<metadata::CameraContextMetadata>();
        d->cfaBlock = d->photoCfaBlock = 4;
        d->photoReduced = true;
        d->binnedFallbackContext = std::make_shared<metadata::CameraContextMetadata>();
        d->baselineWhiteLevel = 1023;
        assert(capture::reducedRawUnsupportedReason(*d, 16320, 12288) == nullptr);
        assert(capture::reducedRawFitFits(1023) && !capture::reducedRawFitFits(8712) && !capture::reducedRawFitFits(0));
        d->baselineWhiteLevel = 0;  // dynamic levels: unknown, each frame is checked later
        assert(capture::reducedRawUnsupportedReason(*d, 16320, 12288) == nullptr);
        d->baselineWhiteLevel = 8712;  // 14-bit: reduces (x4 fits R16), only the fit is out
        assert(capture::reducedRawUnsupportedReason(*d, 16320, 12288) == nullptr);
        d->baselineWhiteLevel = 65535;  // 16-bit: sums overflow R16
        assert(std::string(capture::reducedRawUnsupportedReason(*d, 16320, 12288)) == "white_level");
        d->baselineWhiteLevel = 1023;
        assert(std::string(capture::reducedRawUnsupportedReason(*d, 16324, 12288)) == "geometry");
        d->photoReduced = false;
        assert(std::string(capture::reducedRawUnsupportedReason(*d, 16320, 12288)) == "not_requested");
        d->photoReduced = true;
        d->cfaBlock = 2;  // quad has no reduced output
        assert(std::string(capture::reducedRawUnsupportedReason(*d, 16320, 12288)) == "not_requested");
    }
    // Bayer (linear bins) at 50 MP (2x) and 22 MP (direct 3x): sizes, x4 levels, the bin's per-site noise.
    for (const bool quarter : {false, true}) {
        auto b = std::make_shared<metadata::CameraContextMetadata>(*c);
        b->cfaBlock = b->photoCfaBlock = 1;
        b->photoPhaseFit = false;
        b->photoReduced25 = quarter;
        assert(capture::reducedRawRequested(*b));
        const auto size = capture::reducedRawSize(*b, 16320, 12288);
        assert(quarter ? (size.width == 5440 && size.height == 4096) : (size.width == 8160 && size.height == 6144));
        assert(capture::reducedRawDirect3x(*b) == quarter);
        if (quarter) {  // a height that is not a multiple of 3: a third rounded down to a multiple of 8, centred
            const auto f35 = capture::reducedRawSize(*b, 16320, 12256);
            const auto bin = rawr::raw_ingress::bayerBinOutputSize(16320, 12256, 3);
            assert(f35.width == 5440 && f35.height == 4080 && bin.height == 4080 && bin.offsetY == 8 && bin.offsetX == 0);
        }
        assert(size.width % 8 == 0 && size.height % 8 == 0);
        metadata::FrameMetadataSnapshot f;
        f.cameraContext = b;
        f.blackLevelPhysicalRggb = b->baselineBlackLevelPhysicalRggb;
        f.staticWhiteLevel = f.effectiveWhiteLevel = 1023;
        f.sensorNoiseProfile.assign(8, .01);
        capture::applyReducedRawMetadata(f);
        assert(f.cameraContext->geometry.rawBufferWidth == size.width &&
               f.cameraContext->geometry.rawBufferHeight == size.height);
        assert(!f.cameraContext->photoReduced && !f.cameraContext->photoReduced25);
        assert(f.effectiveWhiteLevel == 4092 && f.blackLevelPhysicalRggb[0] == 4 * 63);
        // Per CFA site (RGGB here; no white balance in the metadata: gains 1): the bin's flat-area variance
        // against a four-sample sum (the x4 levels). At 2x green averages more samples than a sum of four,
        // red and blue (kept apart from the other colours) about as many; the 3x bin averages more.
        static constexpr unsigned kColour[4] = {0, 1, 1, 2};
        for (unsigned p = 0; p < 4; ++p) {
            const double scale = rawr::raw_ingress::bayerBinNoiseVarianceGain(kColour[p], {1, 1, 1}, quarter ? 3 : 2) / 0.25;
            assert(scale > 0.1 && scale < (quarter ? 1.0 : 1.5));
            assert(std::abs(f.sensorNoiseProfile[2 * p] - .01 * scale) < 1e-12);
            assert(std::abs(f.sensorNoiseProfile[2 * p + 1] - .01 * scale) < 1e-12);
        }
    }
    // Hexadeca at 25 MP: only the resample's reduction.
    {
        auto h = std::make_shared<metadata::CameraContextMetadata>(*c);
        h->photoReduced25 = true;
        for (unsigned p = 0; p < 4; ++p) assert(capture::reducedRawNoiseScale(*h, {2, 1, 1.5f}, p) == 0.40);
        assert(capture::reducedRawSize(*h, 16320, 12256).height == 4328);
    }
}
