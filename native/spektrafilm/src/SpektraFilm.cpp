// Record-only core-path engine. Vulkan orchestration is original work for
// this module; table contents/values mirror the upstream reference
// (src/SpektraVulkanRenderer.cpp) per the spec in NOTICE.md.
#include "spektrafilm/SpektraFilm.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "DirCouplers.h"
#include "SpektraTables.h"
#include "spektrafilm/SpektraParameters.h"
#include "spektrafilm/SpektraProfileCurves.h"

namespace spektrafilm_native {
namespace {

constexpr uint32_t kCorePushBytes = 120;
constexpr uint32_t kIoPushBytes = 64;

struct CorePush {
    uint32_t width = 0;
    uint32_t height = 0;
    float filmExposureEv = 0.0f;
    float filmGamma = 1.0f;
    uint32_t exposureCount = 0;
    int32_t inputColorSpace = 15;
    int32_t rgbToRawMethod = 2;
    uint32_t colorSpaceCount = 26;
    uint32_t transferLutSize = 4096;
    float colorDecodeMin = 0.0f;
    float colorDecodeMax = 0.0f;
    uint32_t hanatosWidth = 0;
    uint32_t hanatosHeight = 0;
    uint32_t op = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
    int32_t filmPushPullMode = 0;
    float filmPushPullStops = 0.0f;
    uint32_t fullWidth = 0;
    uint32_t fullHeight = 0;
    uint32_t tileOriginX = 0;
    uint32_t tileOriginY = 0;
    uint32_t activeOriginX = 0;
    uint32_t activeOriginY = 0;
    uint32_t activeWidth = 0;
    uint32_t activeHeight = 0;
    float adaptiveTone[4] = {0.0f, -4.0f, 0.0f, 1.0f};
};
static_assert(sizeof(CorePush) == kCorePushBytes, "core push layout");

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("SpektraFilm: " + message);
}

void checkVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        fail(std::string(what) + " (" + std::to_string(result) + ")");
    }
}

uint32_t findMemoryType(VkPhysicalDevice pd, uint32_t bits,
                        VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(pd, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    fail("no suitable memory type");
}

bool validSpirv(const uint32_t* words, size_t bytes) {
    return words && bytes >= 4 && bytes % 4 == 0 && words[0] == 0x07230203u;
}

// Mirrors upstream makeDirFloatParams (SpektraVulkanRenderer.cpp): packs the
// 15 user DIR params + per-stock density maximums/type into the 18 floats
// the DIR shader reads at binding 27.
void fillDirFloats(const spektrafilm::ProfileCurveSet& film, const FilmLook& look,
                   float pixelSizeUm, float* out) {
    const float amount = dir::effectiveAmount(film, look);
    const float sameLayer = std::max(look.dirCouplersInhibitionSameLayer, 0.0f);
    const float interlayer = std::max(look.dirCouplersInhibitionInterlayer, 0.0f);
    float densityMaximums[3] = {0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < film.exposureCount; ++i) {
        for (int c = 0; c < 3; ++c) {
            densityMaximums[c] =
                std::max(densityMaximums[c], film.densityCurves[i * 3u + c]);
        }
    }
    out[0] = look.dirCouplersGammaSameLayerR * sameLayer * amount;
    out[4] = look.dirCouplersGammaSameLayerG * sameLayer * amount;
    out[8] = look.dirCouplersGammaSameLayerB * sameLayer * amount;
    out[1] = look.dirCouplersGammaRToG * interlayer * amount;
    out[2] = look.dirCouplersGammaRToB * interlayer * amount;
    out[3] = look.dirCouplersGammaGToR * interlayer * amount;
    out[5] = look.dirCouplersGammaGToB * interlayer * amount;
    out[6] = look.dirCouplersGammaBToR * interlayer * amount;
    out[7] = look.dirCouplersGammaBToG * interlayer * amount;
    out[9] = densityMaximums[0];
    out[10] = densityMaximums[1];
    out[11] = densityMaximums[2];
    const float pixel = std::max(pixelSizeUm, 1.0e-6f);
    out[12] = std::max(look.dirCouplersDiffusionUm, 0.0f) / pixel;
    out[13] = std::max(look.dirCouplersDiffusionTailUm, 0.0f) * 0.5360f / pixel;
    out[14] = std::max(look.dirCouplersDiffusionTailUm, 0.0f) * 1.5236f / pixel;
    out[15] = std::max(look.dirCouplersDiffusionTailUm, 0.0f) * 2.7684f / pixel;
    out[16] = std::clamp(look.dirCouplersDiffusionTailWeight, 0.0f, 1.0f);
    out[17] =
        (film.type && std::strcmp(film.type, "positive") == 0) ? 1.0f : 0.0f;
}

// Mirrors upstream interpLinearDensityCurve + makeDirCorrectedDensityCurves:
// re-resolves the stock density curves against DIR-shifted log exposures.
float interpLinearDensityCurve(const std::vector<float>& x, const float* y,
                               uint32_t channel, float target) {
    if (x.empty() || !y) {
        return 0.0f;
    }
    if (x.size() == 1u) {
        return y[channel];
    }
    const bool ascending = x.back() >= x.front();
    if ((ascending && target <= x.front()) ||
        (!ascending && target >= x.front())) {
        return y[channel];
    }
    if ((ascending && target >= x.back()) ||
        (!ascending && target <= x.back())) {
        return y[(x.size() - 1u) * 3u + channel];
    }
    uint32_t lo = 0u;
    uint32_t hi = static_cast<uint32_t>(x.size() - 1u);
    while (hi - lo > 1u) {
        const uint32_t mid = (lo + hi) >> 1u;
        if ((ascending && x[mid] <= target) ||
            (!ascending && x[mid] >= target)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const float denom = std::max(std::abs(x[hi] - x[lo]), 1.0e-9f);
    const float t = std::clamp((target - x[lo]) / denom, 0.0f, 1.0f);
    return y[lo * 3u + channel] + (y[hi * 3u + channel] - y[lo * 3u + channel]) * t;
}

// Mirrors upstream scannerSigmaUmFromMtf50: Gaussian MTF blur sigma.
float scannerSigmaUmFromMtf50(float mtf50LpMm) {
    if (!std::isfinite(mtf50LpMm) || mtf50LpMm <= 0.0f) {
        return 0.0f;
    }
    constexpr float kPi = 3.14159265358979323846f;
    return 1000.0f * std::sqrt(std::log(2.0f) / (2.0f * kPi * kPi)) / mtf50LpMm;
}

// Mirrors upstream scanIlluminantToOutputRgb: paper scan-illuminant color
// for the print-glare tint, resolved to the active output space.
void scanGlareRgb(const spektrafilm::ProfileCurveSet& paper, int32_t outputColorSpace,
                  float* outRgb) {
    const float* cmfs = spektrafilm::standardObserverCmfs();
    if (paper.wavelengthCount == 0u || !paper.scanIlluminant ||
        !paper.scanToOutputRgb || !cmfs) {
        outRgb[0] = outRgb[1] = outRgb[2] = 1.0f;
        return;
    }
    float xyz[3] = {0.0f, 0.0f, 0.0f};
    float normalization = 0.0f;
    for (uint32_t w = 0; w < paper.wavelengthCount; ++w) {
        const float illuminant = paper.scanIlluminant[w];
        xyz[0] += illuminant * cmfs[w * 3u];
        xyz[1] += illuminant * cmfs[w * 3u + 1u];
        xyz[2] += illuminant * cmfs[w * 3u + 2u];
        normalization += illuminant * cmfs[w * 3u + 1u];
    }
    const float inv = 1.0f / std::max(normalization, 1.0e-10f);
    xyz[0] *= inv;
    xyz[1] *= inv;
    xyz[2] *= inv;
    const uint32_t colorSpace =
        static_cast<uint32_t>(std::clamp(outputColorSpace, 0, 25));
    const float* matrix = paper.scanToOutputRgb + static_cast<size_t>(colorSpace) * 9u;
    outRgb[0] = matrix[0] * xyz[0] + matrix[1] * xyz[1] + matrix[2] * xyz[2];
    outRgb[1] = matrix[3] * xyz[0] + matrix[4] * xyz[1] + matrix[5] * xyz[2];
    outRgb[2] = matrix[6] * xyz[0] + matrix[7] * xyz[1] + matrix[8] * xyz[2];
}

void fillDirCorrectedCurves(const spektrafilm::ProfileCurveSet& film,
                            const float* dirFloats, float* out) {    const bool positive = dirFloats[17] > 0.5f;
    for (uint32_t receiver = 0; receiver < 3u; ++receiver) {
        std::vector<float> logExposure0(film.exposureCount, 0.0f);
        for (uint32_t i = 0; i < film.exposureCount; ++i) {
            const float d0 = film.densityCurves[i * 3u];
            const float d1 = film.densityCurves[i * 3u + 1u];
            const float d2 = film.densityCurves[i * 3u + 2u];
            const float silver0 = positive ? dirFloats[9] - d0 : d0;
            const float silver1 = positive ? dirFloats[10] - d1 : d1;
            const float silver2 = positive ? dirFloats[11] - d2 : d2;
            float amount = 0.0f;
            if (receiver == 0u) {
                amount = silver0 * dirFloats[0] + silver1 * dirFloats[3] +
                         silver2 * dirFloats[6];
            } else if (receiver == 1u) {
                amount = silver0 * dirFloats[1] + silver1 * dirFloats[4] +
                         silver2 * dirFloats[7];
            } else {
                amount = silver0 * dirFloats[2] + silver1 * dirFloats[5] +
                         silver2 * dirFloats[8];
            }
            logExposure0[i] = film.logExposure[i] - amount;
        }
        for (uint32_t i = 0; i < film.exposureCount; ++i) {
            out[i * 3u + receiver] = interpLinearDensityCurve(
                logExposure0, film.densityCurves, receiver, film.logExposure[i]);
        }
    }
}

bool validLookForRecord(const FilmLook& look, const FilmLook& baked,
                        const CameraFilters& bakedCamera,
                        const char** reason) noexcept {
    const auto reject = [&](const char* message) {
        if (reason) {
            *reason = message;
        }
        return false;
    };
    if (look.film != baked.film || look.paper != baked.paper) {
        return reject("spektrafilm: film/paper change needs recreate");
    }
    if (look.inputColorSpace != baked.inputColorSpace ||
        look.outputColorSpace != baked.outputColorSpace) {
        return reject("spektrafilm: colorspace change needs recreate");
    }
    // Camera cuts bake into static tables: the look must carry the baked
    // values (pass back bakedCameraFilters()). Change via
    // updateCameraFilters(). Filtration/preflash are live per-frame (frame
    // constants only on this path; see header).
    if (look.cameraUvFilterEnabled != bakedCamera.uvEnabled ||
        look.cameraUvCutNm != bakedCamera.uvCutNm ||
        look.cameraIrFilterEnabled != bakedCamera.irEnabled ||
        look.cameraIrCutNm != bakedCamera.irCutNm) {
        return reject("spektrafilm_native: camera filters differ; call updateCameraFilters()");
    }
    if (!std::isfinite(look.filmExposureEv) ||
        !std::isfinite(look.printExposureEv) ||
        std::abs(look.filmExposureEv) > 10.0f ||
        std::abs(look.printExposureEv) > 10.0f) {
        return reject("spektrafilm_native: exposure out of range");
    }
    if (look.rgbToRawMethod != baked.rgbToRawMethod) {
        return reject("spektrafilm_native: spectral method change needs recreate");
    }
    if (look.process < 0 || look.process > 2) {
        return reject("spektrafilm_native: process must be 0 (print), 1 (scan), or 2 (negative)");
    }
    if (look.printTiming < 0 || look.printTiming > 1) {
        return reject("spektrafilm_native: bad print timing");
    }
    if (look.printTiming == 1 && !tables::academyPrinterDensityAvailable()) {
        return reject("spektrafilm_native: APD data absent (regenerate academy tables)");
    }
    if (look.filmPushPullMode < 0 || look.filmPushPullMode > 1) {
        return reject("spektrafilm_native: bad push-pull mode");
    }
    if (!std::isfinite(look.filmPushPullStops) ||
        !std::isfinite(look.printPushPullStops) ||
        !std::isfinite(look.printShadowShape) ||
        !std::isfinite(look.printHighlightShape) ||
        !std::isfinite(look.negativeBleachBypassAmount) ||
        !std::isfinite(look.negativeLeucoCyanCoupling) ||
        !std::isfinite(look.printBleachBypassAmount) ||
        !std::isfinite(look.preflashExposure) ||
        !std::isfinite(look.preflashMFilterShift) ||
        !std::isfinite(look.preflashYFilterShift) ||
        !std::isfinite(look.printerLightsR) ||
        !std::isfinite(look.printerLightsG) ||
        !std::isfinite(look.printerLightsB) ||
        !std::isfinite(look.enlargerScale) ||
        !std::isfinite(look.enlargerOffsetXPercent) ||
        !std::isfinite(look.enlargerOffsetYPercent) ||
        !std::isfinite(look.dirCouplersAmount) ||
        !std::isfinite(look.dirCouplersDiffusionUm) ||
        !std::isfinite(look.dirCouplersDiffusionTailUm) ||
        !std::isfinite(look.dirCouplersDiffusionTailWeight) ||
        !std::isfinite(look.dirCouplersInhibitionSameLayer) ||
        !std::isfinite(look.dirCouplersInhibitionInterlayer) ||
        !std::isfinite(look.dirCouplersGammaSameLayerR) ||
        !std::isfinite(look.dirCouplersGammaSameLayerG) ||
        !std::isfinite(look.dirCouplersGammaSameLayerB) ||
        !std::isfinite(look.dirCouplersGammaRToG) ||
        !std::isfinite(look.dirCouplersGammaRToB) ||
        !std::isfinite(look.dirCouplersGammaGToR) ||
        !std::isfinite(look.dirCouplersGammaGToB) ||
        !std::isfinite(look.dirCouplersGammaBToR) ||
        !std::isfinite(look.dirCouplersGammaBToG) ||
        !std::isfinite(look.scannerWhiteLevel) ||
        !std::isfinite(look.scannerBlackLevel) ||
        !std::isfinite(look.glarePercent) ||
        !std::isfinite(look.glareRoughness) ||
        !std::isfinite(look.glareBlur) ||
        !std::isfinite(look.scannerMtf50LpMm) ||
        !std::isfinite(look.scannerUnsharpRadiusUm) ||
        !std::isfinite(look.scannerUnsharpAmount) ||
        !std::isfinite(look.scatterAmount) ||
        !std::isfinite(look.scatterScale) ||
        !std::isfinite(look.halationAmount) ||
        !std::isfinite(look.halationScale) ||
        !std::isfinite(look.halationStrengthR) ||
        !std::isfinite(look.halationStrengthG) ||
        !std::isfinite(look.halationStrengthB) ||
        !std::isfinite(look.halationFirstSigmaUmR) ||
        !std::isfinite(look.halationFirstSigmaUmG) ||
        !std::isfinite(look.halationFirstSigmaUmB) ||
        !std::isfinite(look.halationBoostEv) ||
        !std::isfinite(look.halationBoostRange) ||
        !std::isfinite(look.halationProtectEv) ||
        !std::isfinite(look.cameraDiffusionStrength) ||
        !std::isfinite(look.cameraDiffusionSpatialScale) ||
        !std::isfinite(look.cameraDiffusionHaloWarmth) ||
        !std::isfinite(look.cameraDiffusionCoreIntensity) ||
        !std::isfinite(look.cameraDiffusionCoreSize) ||
        !std::isfinite(look.cameraDiffusionHaloIntensity) ||
        !std::isfinite(look.cameraDiffusionHaloSize) ||
        !std::isfinite(look.cameraDiffusionBloomIntensity) ||
        !std::isfinite(look.cameraDiffusionBloomSize) ||
        !std::isfinite(look.printDiffusionStrength) ||
        !std::isfinite(look.printDiffusionSpatialScale) ||
        !std::isfinite(look.printDiffusionHaloWarmth) ||
        !std::isfinite(look.printDiffusionCoreIntensity) ||
        !std::isfinite(look.printDiffusionCoreSize) ||
        !std::isfinite(look.printDiffusionHaloIntensity) ||
        !std::isfinite(look.printDiffusionHaloSize) ||
        !std::isfinite(look.printDiffusionBloomIntensity) ||
        !std::isfinite(look.printDiffusionBloomSize)) {
        return reject("spektrafilm_native: non-finite look value");
    }
    if (look.filmGamma <= 0.0f || look.printGamma <= 0.0f) {
        return reject("spektrafilm_native: gamma must be positive");
    }
    if (look.negativeBleachBypassAmount < 0.0f ||
        look.printBleachBypassAmount < 0.0f || look.preflashExposure < 0.0f ||
        look.dirCouplersAmount < 0.0f || look.glarePercent < 0.0f ||
        look.scatterAmount < 0.0f || look.scatterScale < 0.0f ||
        look.halationAmount < 0.0f || look.halationScale < 0.0f ||
        look.halationStrengthR < 0.0f || look.halationStrengthG < 0.0f ||
        look.halationStrengthB < 0.0f || look.halationBoostEv < 0.0f ||
        look.halationProtectEv < 0.0f) {
        return reject("spektrafilm_native: amount must be >= 0");
    }
    if (look.cameraDiffusionStrength < 0.0f ||
        look.cameraDiffusionSpatialScale < 0.0f ||
        look.cameraDiffusionCoreIntensity < 0.0f ||
        look.cameraDiffusionHaloIntensity < 0.0f ||
        look.cameraDiffusionBloomIntensity < 0.0f ||
        look.printDiffusionStrength < 0.0f ||
        look.printDiffusionSpatialScale < 0.0f ||
        look.printDiffusionCoreIntensity < 0.0f ||
        look.printDiffusionHaloIntensity < 0.0f ||
        look.printDiffusionBloomIntensity < 0.0f) {
        return reject("spektrafilm_native: amount must be >= 0");
    }
    if (look.halationFirstSigmaUmR <= 0.0f ||
        look.halationFirstSigmaUmG <= 0.0f ||
        look.halationFirstSigmaUmB <= 0.0f) {
        return reject("spektrafilm_native: halation sigma must be > 0");
    }
    if (look.halationBoostRange < 0.0f || look.halationBoostRange > 1.0f) {
        return reject("spektrafilm_native: boost range must be 0..1");
    }
    if (look.grainModel < 0 || look.grainModel > 2) {
        return reject("spektrafilm_native: bad grain model (0 preview, 1 production, 2 synthesis)");
    }
    if (look.filmFormat < 0 || look.filmFormat > 7) {
        return reject("spektrafilm_native: bad film format");
    }
    if (!std::isfinite(look.grainAmount) || look.grainAmount < 0.0f ||
        !std::isfinite(look.grainSaturation) ||
        !std::isfinite(look.grainParticleAreaUm2) ||
        look.grainParticleAreaUm2 <= 0.0f ||
        !std::isfinite(look.grainParticleScaleR) ||
        look.grainParticleScaleR <= 0.0f ||
        !std::isfinite(look.grainParticleScaleG) ||
        look.grainParticleScaleG <= 0.0f ||
        !std::isfinite(look.grainParticleScaleB) ||
        look.grainParticleScaleB <= 0.0f ||
        !std::isfinite(look.grainParticleScaleLayer0) ||
        !std::isfinite(look.grainParticleScaleLayer1) ||
        !std::isfinite(look.grainParticleScaleLayer2) ||
        !std::isfinite(look.grainDensityMinR) ||
        !std::isfinite(look.grainDensityMinG) ||
        !std::isfinite(look.grainDensityMinB) ||
        !std::isfinite(look.grainUniformityR) ||
        !std::isfinite(look.grainUniformityG) ||
        !std::isfinite(look.grainUniformityB) ||
        !std::isfinite(look.grainFinalBlurUm) ||
        !std::isfinite(look.grainBlurDyeCloudsUm) ||
        !std::isfinite(look.grainMicroStructureScale) ||
        !std::isfinite(look.grainMicroStructureSigmaNm)) {
        return reject("spektrafilm_native: bad grain value");
    }
    if (look.grainSubLayerCount < 1) {
        return reject("spektrafilm_native: sub-layer count must be >= 1");
    }
    if (look.cameraDiffusionFamily < 0 || look.cameraDiffusionFamily > 3) {
        return reject("spektrafilm_native: bad diffusion family");
    }
    if (look.printDiffusionFamily < 0 || look.printDiffusionFamily > 3) {
        return reject("spektrafilm_native: bad diffusion family");
    }
    if (look.cameraDiffusionCoreSize <= 0.0f ||
        look.cameraDiffusionHaloSize <= 0.0f ||
        look.cameraDiffusionBloomSize <= 0.0f ||
        look.printDiffusionCoreSize <= 0.0f ||
        look.printDiffusionHaloSize <= 0.0f ||
        look.printDiffusionBloomSize <= 0.0f) {
        return reject("spektrafilm_native: diffusion size must be > 0");
    }
    if (!std::isfinite(look.grainSynthesisSize) ||
        !std::isfinite(look.grainSynthesisAmount) ||
        !std::isfinite(look.grainSynthesisSharpness) ||
        !std::isfinite(look.grainSynthesisQuality) ||
        !std::isfinite(look.grainSynthesisMeanRadiusUm) ||
        !std::isfinite(look.grainSynthesisRadiusStdDevRatio) ||
        !std::isfinite(look.grainSynthesisObservationSigmaUm) ||
        !std::isfinite(look.grainSynthesisCellSizeRatio) ||
        !std::isfinite(look.grainSynthesisMaxRadiusQuantile) ||
        !std::isfinite(look.grainSynthesisCoverageEpsilon) ||
        !std::isfinite(look.grainSynthesisRadiusScaleR) ||
        !std::isfinite(look.grainSynthesisRadiusScaleG) ||
        !std::isfinite(look.grainSynthesisRadiusScaleB) ||
        !std::isfinite(look.grainSynthesisLayerScale0) ||
        !std::isfinite(look.grainSynthesisLayerScale1) ||
        !std::isfinite(look.grainSynthesisLayerScale2)) {
        return reject("spektrafilm_native: bad synthesis value");
    }
    if (look.grainSynthesisSamples < 1 || look.grainSynthesisSamples > 1024) {
        return reject("spektrafilm_native: bad synthesis samples");
    }
    if (look.grainSynthesisMaxGrainsPerCell < 1 ||
        look.grainSynthesisMaxGrainsPerCell > 128) {
        return reject("spektrafilm_native: bad synthesis max grains");
    }
    if (look.outputRole < 0 || look.outputRole > 2) {
        return reject("spektrafilm_native: bad output role");
    }
    if (look.autoExposureMethod < 0 || look.autoExposureMethod > 1) {
        return reject("spektrafilm_native: bad auto-exposure method");
    }
    if (look.hdrTransfer < 0 || look.hdrTransfer > 1) {
        return reject("spektrafilm_native: bad HDR transfer");
    }
    if (look.hdrToneMapping < 0 || look.hdrToneMapping > 1) {
        return reject("spektrafilm_native: bad HDR tone mapping");
    }
    if (!std::isfinite(look.hdrReferenceWhiteNits) ||
        !std::isfinite(look.hdrPeakNits) ||
        !std::isfinite(look.hdrExposureEv) ||
        look.hdrReferenceWhiteNits < 1.0f ||
        look.hdrPeakNits <= look.hdrReferenceWhiteNits) {
        return reject("spektrafilm_native: bad HDR nits (need 1<=ref<peak)");
    }
    return true;
}

// Tile overlap estimator (upstream estimateVulkanTileOverlap 1190-1247):
// +256 per active spatial effect, +64 for production/synthesis grain.
// RCM output disables the scanner term. ProcessNegative skips film-path
// effects (grain/halation/camera diffusion/DIR) per upstream gating.
uint32_t estimateTileOverlapForLook(const FilmLook& look) noexcept {
    const bool processNegative = look.process == 2;
    const bool rcm = look.outputRole == 2;
    uint32_t overlap = 0;
    if (!processNegative && look.cameraDiffusionEnabled &&
        look.cameraDiffusionStrength > 0.0f &&
        look.cameraDiffusionSpatialScale > 0.0f) {
        overlap += 256u;
    }
    if (!processNegative && look.halationEnabled &&
        ((look.scatterAmount > 0.0f && look.scatterScale > 0.0f) ||
         (look.halationAmount > 0.0f && look.halationScale > 0.0f &&
          (look.halationStrengthR > 0.0f || look.halationStrengthG > 0.0f ||
           look.halationStrengthB > 0.0f)))) {
        overlap += 256u;
    }
    if (!processNegative && look.dirCouplersAmount > 0.0f &&
        (look.dirCouplersDiffusionUm > 0.0f ||
         (look.dirCouplersDiffusionTailUm > 0.0f &&
          look.dirCouplersDiffusionTailWeight > 0.0f))) {
        overlap += 256u;
    }
    if (!processNegative && look.grainEnabled && look.grainModel != 0) {
        overlap += 64u;
    }
    const bool printLikeFinal = look.process == 0 || look.process == 2;
    if (printLikeFinal && look.printDiffusionEnabled &&
        look.printDiffusionStrength > 0.0f &&
        look.printDiffusionSpatialScale > 0.0f) {
        overlap += 256u;
    }
    if (!rcm && look.scannerEnabled &&
        (look.glarePercent > 0.0f || look.scannerMtf50LpMm > 0.0f ||
         (look.scannerUnsharpRadiusUm > 0.0f &&
          look.scannerUnsharpAmount > 0.0f))) {
        overlap += 256u;
    }
    return overlap;
}

// Tile-arena overlap (create-time): like estimateTileOverlapForLook but from
// baked allocation gates only (continuous strengths can vary per record
// without exceeding it). DIR has no cheap gate for memory engines, so the
// caller must rebuild when dirCouplersAmount crosses 0 (stills scratchMatch
// does); the term is then exact. Print/scanner terms ignore baked
// process/role (superset: avoids recreate when toggling those per record).
uint32_t tileArenaOverlapForBaked(const FilmLook& baked) noexcept {
    uint32_t overlap = 0u;
    if (baked.halationEnabled) {
        overlap += 256u;
    }
    if (baked.cameraDiffusionEnabled) {
        overlap += 256u;
    }
    if (baked.dirCouplersAmount > 0.0f) {
        overlap += 256u;
    }
    if (baked.grainEnabled && baked.grainModel != 0) {
        overlap += 64u;
    }
    if (baked.printDiffusionEnabled) {
        overlap += 256u;
    }
    if (baked.scannerEnabled) {
        overlap += 256u;
    }
    return overlap;
}

// Upstream alignedReducedDimension (reference 1249-1264): reduced extent of
// [tileOrigin, tileOrigin+localSize) at 1/scale over a fullSize domain.
uint32_t alignedReducedDimension(uint32_t localSize, uint32_t tileOrigin,
                                 uint32_t fullSize, uint32_t scale) noexcept {
    if (localSize == 0u || fullSize == 0u) {
        return 0u;
    }
    scale = std::max(scale, 1u);
    const uint64_t start = static_cast<uint64_t>(tileOrigin) / scale;
    const uint64_t localEnd = std::min<uint64_t>(
        static_cast<uint64_t>(tileOrigin) + localSize, fullSize);
    const uint64_t end = (localEnd + scale - 1u) / scale;
    return static_cast<uint32_t>(
        std::max<uint64_t>(end > start ? end - start : 0u, 1u));
}

// Camera-diffusion CPU solver. Ports upstream's family table + component
// expansion (SpektraVulkanRenderer.cpp diffusionShape/diffusionWeights/
// haloChannelWeights/appendDiffusionGroupComponents/clusterDiffusion-
// Components/makeDiffusionComponents) with the default backend tuning
// (group size 2, downsample auto, cluster sigma 0.10): user params ->
// GPU-ready component list + info struct for SpektraDiffusion.comp
// bindings 27/28. Returns empty components when the path is off.
namespace diffusion {

constexpr uint32_t kMaxComponents = 32u;

struct Component {
    float sigmaPx = 0.0f;
    float weightR = 0.0f;
    float weightG = 0.0f;
    float weightB = 0.0f;
};

struct Info {
    uint32_t componentCount = 0;
    float scatterFraction = 0.0f;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
};
static_assert(sizeof(Info) == 16u, "diffusion info layout");
static_assert(sizeof(Component) == 16u, "diffusion component layout");

struct Group {
    float lambdaUm = 0.0f;
    float spread = 1.0f;
    uint32_t count = 1u;
    float alpha = 3.0f;
};

struct FamilyShape {
    Group core{};
    Group halo{};
    Group bloom{};
    float weightCore = 0.0f;
    float weightHalo = 0.0f;
    float weightBloom = 0.0f;
    float warmthBase = 0.0f;
    float totalGain = 1.0f;
};

FamilyShape diffusionShape(int32_t family) {
    // {lambdaUm, spread, count, alpha} x core/halo/bloom, then
    // weightCore/Halo/Bloom, warmthBase, totalGain.
    switch (family) {
        case 0:  // Glimmerglass
            return {{10.0f, 1.5f, 2u, 3.0f},
                     {50.0f, 2.0f, 3u, 3.0f},
                     {260.0f, 2.5f, 4u, 3.2f},
                     0.60f, 0.30f, 0.10f, 0.0f, 0.65f};
        case 2:  // ProMist
            return {{14.0f, 1.5f, 2u, 3.0f},
                     {150.0f, 2.0f, 3u, 3.0f},
                     {650.0f, 2.5f, 4u, 2.9f},
                     0.28f, 0.42f, 0.30f, 0.40f, 1.05f};
        case 3:  // CineBloom
            return {{20.0f, 1.5f, 2u, 3.0f},
                     {200.0f, 2.0f, 3u, 3.0f},
                     {1000.0f, 2.5f, 4u, 2.5f},
                     0.22f, 0.30f, 0.48f, 0.85f, 1.00f};
        case 1:  // BlackProMist (default)
        default:
            return {{16.0f, 1.5f, 2u, 3.0f},
                     {95.0f, 2.0f, 3u, 3.0f},
                     {380.0f, 2.5f, 4u, 3.5f},
                     0.40f, 0.47f, 0.13f, 0.65f, 0.75f};
    }
}

float diffusionScatterFraction(float strength, float familyGain) {
    if (strength <= 0.0f) {
        return 0.0f;
    }
    constexpr float kBreaks[5] = {0.125f, 0.25f, 0.5f, 1.0f, 2.0f};
    constexpr float kFractions[5] = {0.10f, 0.20f, 0.35f, 0.55f, 0.75f};
    const float logStrength = std::log2(std::max(strength, 1.0e-6f));
    if (logStrength <= std::log2(kBreaks[0])) {
        return std::clamp(kFractions[0] * familyGain, 0.0f, 0.99f);
    }
    if (logStrength >= std::log2(kBreaks[4])) {
        return std::clamp(kFractions[4] * familyGain, 0.0f, 0.99f);
    }
    for (size_t i = 0; i + 1u < 5u; ++i) {
        const float x0 = std::log2(kBreaks[i]);
        const float x1 = std::log2(kBreaks[i + 1u]);
        if (logStrength >= x0 && logStrength <= x1) {
            const float t =
                std::clamp((logStrength - x0) / std::max(x1 - x0, 1.0e-6f), 0.0f, 1.0f);
            return std::clamp((kFractions[i] + (kFractions[i + 1u] - kFractions[i]) * t) *
                                  familyGain,
                              0.0f, 0.99f);
        }
    }
    return 0.0f;
}

std::vector<float> diffusionLambdas(const Group& group) {
    std::vector<float> lambdas(group.count, group.lambdaUm);
    if (group.count <= 1u || group.spread <= 1.0f) {
        return lambdas;
    }
    const float logLo = std::log(group.lambdaUm / group.spread);
    const float logHi = std::log(group.lambdaUm * group.spread);
    for (uint32_t i = 0; i < group.count; ++i) {
        const float t = group.count == 1u
                            ? 0.0f
                            : static_cast<float>(i) / static_cast<float>(group.count - 1u);
        lambdas[i] = std::exp(logLo + (logHi - logLo) * t);
    }
    return lambdas;
}

std::vector<float> diffusionWeights(const Group& group, bool bloom) {
    const std::vector<float> lambdas = diffusionLambdas(group);
    std::vector<float> weights(lambdas.size(), 1.0f);
    if (bloom) {
        for (size_t i = 0; i < weights.size(); ++i) {
            weights[i] = std::pow(std::max(lambdas[i], 1.0e-6f), 2.0f - group.alpha);
        }
    }
    float sum = 0.0f;
    for (float weight : weights) {
        sum += weight;
    }
    for (float& weight : weights) {
        weight /= std::max(sum, 1.0e-6f);
    }
    return weights;
}

struct ChannelWeights {
    std::vector<float> r;
    std::vector<float> g;
    std::vector<float> b;
};

ChannelWeights haloChannelWeights(const std::vector<float>& weights, float warmth) {
    constexpr float kWarmthAxis[3] = {1.30f, 0.15f, -1.45f};
    ChannelWeights out{weights, weights, weights};
    const size_t count = weights.size();
    if (count < 2u) {
        return out;
    }
    warmth = std::clamp(warmth, -1.5f, 1.5f);
    std::vector<float> gradient(count, 0.0f);
    float totalWeight = 0.0f;
    float weightedGradient = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        gradient[i] = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(count - 1u);
        totalWeight += weights[i];
        weightedGradient += weights[i] * gradient[i];
    }
    const float gradientMean = weightedGradient / std::max(totalWeight, 1.0e-6f);
    for (float& value : gradient) {
        value -= gradientMean;
    }
    std::vector<float>* channels[3] = {&out.r, &out.g, &out.b};
    for (size_t channel = 0; channel < 3u; ++channel) {
        float sum = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            (*channels[channel])[i] =
                std::max(weights[i] * (1.0f + warmth * kWarmthAxis[channel] * gradient[i]), 0.0f);
            sum += (*channels[channel])[i];
        }
        for (size_t i = 0; i < count; ++i) {
            (*channels[channel])[i] *= totalWeight / std::max(sum, 1.0e-6f);
        }
    }
    return out;
}

void appendGroupComponents(std::vector<Component>& components, const Group& group,
                           const std::vector<float>& weights,
                           const std::array<float, 3>& channelScale,
                           float groupWeight, float spatialScale,
                           float pixelSizeUm) {
    constexpr float kExpGaussianFit[3][2] = {
        {0.1633f, 0.5360f}, {0.6496f, 1.5236f}, {0.1870f, 2.7684f}};
    const std::vector<float> lambdas = diffusionLambdas(group);
    for (size_t i = 0; i < lambdas.size(); ++i) {
        for (const auto& fit : kExpGaussianFit) {
            const float sigmaPx =
                std::max(lambdas[i] * fit[1] * spatialScale /
                             std::max(pixelSizeUm, 1.0e-6f),
                         1.0e-6f);
            const float weight = groupWeight * weights[i] * fit[0];
            Component c{sigmaPx, weight * channelScale[0],
                        weight * channelScale[1], weight * channelScale[2]};
            if (c.weightR == 0.0f && c.weightG == 0.0f && c.weightB == 0.0f) {
                continue;
            }
            components.push_back(c);
        }
    }
}

void clusterComponents(std::vector<Component>& components, float sigmaRatio) {
    if (components.size() < 2u || sigmaRatio <= 0.0f) {
        return;
    }
    std::sort(components.begin(), components.end(),
              [](const Component& a, const Component& b) {
                  return a.sigmaPx < b.sigmaPx;
              });
    std::vector<Component> clustered;
    clustered.reserve(components.size());
    for (const Component& component : components) {
        if (clustered.empty()) {
            clustered.push_back(component);
            continue;
        }
        Component& last = clustered.back();
        const float denom =
            std::max(std::max(last.sigmaPx, component.sigmaPx), 1.0e-6f);
        if (std::abs(component.sigmaPx - last.sigmaPx) / denom <= sigmaRatio) {
            const float lastWeight =
                std::abs(last.weightR) + std::abs(last.weightG) + std::abs(last.weightB);
            const float componentWeight = std::abs(component.weightR) +
                                          std::abs(component.weightG) +
                                          std::abs(component.weightB);
            const float totalWeight = lastWeight + componentWeight;
            if (totalWeight > 1.0e-8f) {
                last.sigmaPx = (last.sigmaPx * lastWeight +
                                component.sigmaPx * componentWeight) /
                               totalWeight;
            }
            last.weightR += component.weightR;
            last.weightG += component.weightG;
            last.weightB += component.weightB;
        } else {
            clustered.push_back(component);
        }
    }
    components.swap(clustered);
}

struct Solve {
    Info info{};
    std::vector<Component> components;
};

struct Settings {
    int32_t family = 1;
    float strength = 0.5f;
    float spatialScale = 1.0f;
    float haloWarmth = 0.0f;
    float coreIntensity = 1.0f;
    float coreSize = 1.0f;
    float haloIntensity = 1.0f;
    float haloSize = 1.0f;
    float bloomIntensity = 1.0f;
    float bloomSize = 1.0f;
};

Solve solveSettings(const Settings& settings, float pixelSizeUm) {
    Solve solve;
    if (settings.strength <= 0.0f || settings.spatialScale <= 0.0f) {
        return solve;
    }
    FamilyShape shape = diffusionShape(settings.family);
    const float coreIntensity = std::max(settings.coreIntensity, 0.0f);
    const float haloIntensity = std::max(settings.haloIntensity, 0.0f);
    const float bloomIntensity = std::max(settings.bloomIntensity, 0.0f);
    float wc = shape.weightCore * coreIntensity;
    float wh = shape.weightHalo * haloIntensity;
    float wb = shape.weightBloom * bloomIntensity;
    const float total = wc + wh + wb;
    if (total > 0.0f) {
        wc /= total;
        wh /= total;
        wb /= total;
    } else {
        return solve;
    }
    shape.core.lambdaUm *= std::max(settings.coreSize, 1.0e-6f);
    shape.halo.lambdaUm *= std::max(settings.haloSize, 1.0e-6f);
    shape.bloom.lambdaUm *= std::max(settings.bloomSize, 1.0e-6f);

    solve.info.scatterFraction =
        diffusionScatterFraction(settings.strength, shape.totalGain);
    if (solve.info.scatterFraction <= 0.0f) {
        solve.info.componentCount = 0u;
        return solve;
    }
    const float spatialScale = settings.spatialScale;
    appendGroupComponents(solve.components, shape.core,
                          diffusionWeights(shape.core, false), {1.0f, 1.0f, 1.0f},
                          wc, spatialScale, pixelSizeUm);
    const std::vector<float> haloWeights = diffusionWeights(shape.halo, false);
    const ChannelWeights haloPerChannel = haloChannelWeights(
        haloWeights, shape.warmthBase + settings.haloWarmth);
    const std::vector<float> haloLambdas = diffusionLambdas(shape.halo);
    constexpr float kExpGaussianFit[3][2] = {
        {0.1633f, 0.5360f}, {0.6496f, 1.5236f}, {0.1870f, 2.7684f}};
    for (size_t i = 0; i < haloLambdas.size(); ++i) {
        for (const auto& fit : kExpGaussianFit) {
            const float sigmaPx =
                std::max(haloLambdas[i] * fit[1] * spatialScale /
                             std::max(pixelSizeUm, 1.0e-6f),
                         1.0e-6f);
            Component c{sigmaPx, wh * haloPerChannel.r[i] * fit[0],
                        wh * haloPerChannel.g[i] * fit[0],
                        wh * haloPerChannel.b[i] * fit[0]};
            if (c.weightR == 0.0f && c.weightG == 0.0f && c.weightB == 0.0f) {
                continue;
            }
            solve.components.push_back(c);
        }
    }
    appendGroupComponents(solve.components, shape.bloom,
                          diffusionWeights(shape.bloom, true), {1.0f, 1.0f, 1.0f},
                          wb, spatialScale, pixelSizeUm);
    clusterComponents(solve.components, 0.10f);
    if (solve.components.size() > kMaxComponents) {
        solve.components.resize(kMaxComponents);
    }
    solve.info.componentCount =
        static_cast<uint32_t>(solve.components.size());
    return solve;
}

Solve solveCamera(const FilmLook& look, float pixelSizeUm) {
    if (!look.cameraDiffusionEnabled) {
        return Solve{};
    }
    Settings settings;
    settings.family = look.cameraDiffusionFamily;
    settings.strength = look.cameraDiffusionStrength;
    settings.spatialScale = look.cameraDiffusionSpatialScale;
    settings.haloWarmth = look.cameraDiffusionHaloWarmth;
    settings.coreIntensity = look.cameraDiffusionCoreIntensity;
    settings.coreSize = look.cameraDiffusionCoreSize;
    settings.haloIntensity = look.cameraDiffusionHaloIntensity;
    settings.haloSize = look.cameraDiffusionHaloSize;
    settings.bloomIntensity = look.cameraDiffusionBloomIntensity;
    settings.bloomSize = look.cameraDiffusionBloomSize;
    return solveSettings(settings, pixelSizeUm);
}

Solve solvePrint(const FilmLook& look, float pixelSizeUm) {
    if (!look.printDiffusionEnabled) {
        return Solve{};
    }
    Settings settings;
    settings.family = look.printDiffusionFamily;
    settings.strength = look.printDiffusionStrength;
    settings.spatialScale = look.printDiffusionSpatialScale;
    settings.haloWarmth = look.printDiffusionHaloWarmth;
    settings.coreIntensity = look.printDiffusionCoreIntensity;
    settings.coreSize = look.printDiffusionCoreSize;
    settings.haloIntensity = look.printDiffusionHaloIntensity;
    settings.haloSize = look.printDiffusionHaloSize;
    settings.bloomIntensity = look.printDiffusionBloomIntensity;
    settings.bloomSize = look.printDiffusionBloomSize;
    return solveSettings(settings, pixelSizeUm);
}

uint32_t downsampleScaleForSigma(float sigmaPx) {
    if (sigmaPx >= 48.0f) {
        return 8u;
    }
    if (sigmaPx >= 24.0f) {
        return 4u;
    }
    if (sigmaPx >= 12.0f) {
        return 2u;
    }
    return 1u;
}

}  // namespace diffusion

}  // namespace

struct SpektraFilm::Impl {
    VulkanContext context{};
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    uint32_t slotCount = 0;
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
    FilmLook bakedLook{};
    bool conditionalScratch = false;
    bool diagnosticTransfers = false;
    // Tiled memory-saving arena (stills): tile working buffers replace
    // full-frame effect scratch. Sized from maxTile + arena overlap.
    bool tileMemorySaving = false;
    uint32_t maxTileW = 512u;
    uint32_t maxTileH = 256u;
    uint32_t tileArenaOv = 0;
    uint32_t tileArenaW = 0;  // working pixels (maxTile + 2*overlap)
    uint32_t tileArenaH = 0;

    tables::StaticTables tables{};
    const spektrafilm::ProfileCurveSet* filmCurves = nullptr;
    const spektrafilm::ProfileCurveSet* paperCurves = nullptr;
    // Baked spectral state. record() requires the look to carry these exact
    // values (pass back bakedCameraFilters()). Change via updateCameraFilters().
    CameraFilters bakedCamera{};
    std::vector<float> hanatosSpectraData;  // retained for rebuilds
    int32_t bakedMethod = 2;
    // ProcessNegative paper-table cache (filtration + timing are live but bake
    // into paper Hanatos pairs; rebuilt via updateProcessNegativeTables while
    // GPU-idle). record() warns via validation only when tables are stale —
    // the shader reads whatever is bound, so callers must rebuild on change.
    struct ProcessNegativeCache {
        FilmLook look{};
        bool valid = false;
    } bakedProcessNegative;

    VkDescriptorSetLayout coreLayout = VK_NULL_HANDLE;  // 31x STORAGE_BUFFER
    VkPipelineLayout corePipelineLayout = VK_NULL_HANDLE;
    VkPipeline exposurePipeline = VK_NULL_HANDLE;
    VkPipeline developPipeline = VK_NULL_HANDLE;
    VkPipeline printScanPipeline = VK_NULL_HANDLE;
    VkPipeline grainPipeline = VK_NULL_HANDLE;
    VkPipeline dirPipeline = VK_NULL_HANDLE;
    VkPipeline halationPipeline = VK_NULL_HANDLE;
    VkPipeline diffusionPipeline = VK_NULL_HANDLE;
    VkPipeline scannerPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout ioLayout = VK_NULL_HANDLE;  // buffer + image
    VkPipelineLayout ioPipelineLayout = VK_NULL_HANDLE;
    VkPipeline inputPipeline = VK_NULL_HANDLE;
    VkPipeline outputPipeline = VK_NULL_HANDLE;
    VkPipeline outputHalfPipeline = VK_NULL_HANDLE;
    VkPipeline milestonePipeline = VK_NULL_HANDLE;
    // Glow-ratio export (UltraHDR tap): 2 buffers (pre/post scatter linear)
    // + 1 storage image. Built only when glowRatioSpirv is provided.
    VkDescriptorSetLayout glowRatioLayout = VK_NULL_HANDLE;
    VkPipelineLayout glowRatioPipelineLayout = VK_NULL_HANDLE;
    VkPipeline glowRatioPipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    struct StaticBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    // Dummy storage for layout bindings the shaders never read. Desktop
    // Vulkan tolerates sparse sets; MoltenVK argument buffers need every
    // declared binding valid or dispatches silently no-op.
    VkBuffer dummyBuffer = VK_NULL_HANDLE;
    // Indexed by StaticId; memory tracked in ownedMemory.
    std::vector<StaticBuffer> statics;
    enum StaticId : size_t {
        kPackedFilmExposure,
        kFilmDensityCurves,
        kInputToRefXyz,
        kInputToSrgb,
        kDecodeLuts,
        kTransferKinds,
        kMallett,
        kHanatosResponse,
        kPackedPaperExposure,
        kPaperDensityCurves,
        kFilmSpectral,
        kFilmBase,
        kPaperSensitivity,
        kPaperSpectral,
        kPaperBase,
        kScanProducts,
        kThKg3,
        kCustomFilters,
        kNeutralFilters,
        kCmfs,
        kFilmScanToOutput,
        kPaperScanToOutput,
        kEncodeAndGamut,
        kAcademy,
        kPaperHanatos,
        kPreflashHanatos,
        kDensityCurveLayers,
        kDensityCurveLayerMaxima,
        kStaticCount
    };

    struct Slot {
        // Full-frame pixel buffers (see field docs below). With
        // tiledMemorySaving, only source/destination/filteredEnlarger stay
        // full-frame; the rest are dummy-bound and never dispatched.
        VkBuffer source = VK_NULL_HANDLE;       // float px, DEVICE_LOCAL
        VkBuffer filmRaw = VK_NULL_HANDLE;      // float px
        VkBuffer filmDensity = VK_NULL_HANDLE;  // float px
        VkBuffer grainDensity = VK_NULL_HANDLE;  // float px (preview out = upstream A)
        // Production-grain scratch (upstream grainDensityB/Micro/Layer):
        // ~136 B/px total. Preview arenas stay small; full-res stills carry
        // ~1.7GB transient (accepted per full-res-everywhere directive).
        VkBuffer grainDensityB = VK_NULL_HANDLE;  // float px (production out)
        VkBuffer grainMicroA = VK_NULL_HANDLE;    // float px
        VkBuffer grainMicroB = VK_NULL_HANDLE;    // float px
        VkBuffer grainLayerA = VK_NULL_HANDLE;    // 9x float px
        // grainLayerB: legacy full-frame second layer buffer, no longer
        // allocated (sliced ops 14/15 use sharedS0). Field retained so
        // destroy() stays in sync; tile arenas keep their own t.grainLayerB.
        VkBuffer grainLayerB = VK_NULL_HANDLE;    // unused full-frame
        VkBuffer dirCorrectionA = VK_NULL_HANDLE;  // float px (DIR scratch)
        VkBuffer dirCorrectionB = VK_NULL_HANDLE;  // float px (DIR scratch)
        VkBuffer dirCorrectionC = VK_NULL_HANDLE;  // float px (DIR scratch)
        VkBuffer dirDensity = VK_NULL_HANDLE;      // float px (DIR output)
        VkBuffer halationRawA = VK_NULL_HANDLE;  // float px (halation scratch)
        VkBuffer halationRawB = VK_NULL_HANDLE;  // float px (halation scratch)
        VkBuffer halationRawC = VK_NULL_HANDLE;  // float px (halation scratch)
        VkBuffer halationRawD = VK_NULL_HANDLE;  // float px (halation scratch)
        VkBuffer halationLogRaw = VK_NULL_HANDLE;  // float px (halation output)
        VkBuffer halationBoostedRaw = VK_NULL_HANDLE;  // float px (boost out)
        VkBuffer halationBoostChunks = VK_NULL_HANDLE;  // float4 chunks
        VkBuffer halationBoostInfo = VK_NULL_HANDLE;  // 4 floats (max,x0,a,k)
        VkBuffer diffusionTemp = VK_NULL_HANDLE;  // 2x float px (grouped blur)
        VkBuffer diffusionAccum = VK_NULL_HANDLE;  // float px (accumulator)
        VkBuffer diffusionDownSource = VK_NULL_HANDLE;  // downsampled source
        VkBuffer diffusionDownTemp = VK_NULL_HANDLE;  // 2x downsampled temp
        VkBuffer diffusionDownBlur = VK_NULL_HANDLE;  // 2x downsampled blur
        VkBuffer cameraDiffusionRaw = VK_NULL_HANDLE;  // float px (diff output)
        VkBuffer diffusionInfo = VK_NULL_HANDLE;  // 16B, host-visible
        VkBuffer diffusionComponents = VK_NULL_HANDLE;  // 32x16B, host-visible
        void* mappedDiffusionInfo = nullptr;
        void* mappedDiffusionComponents = nullptr;
        VkBuffer printDiffusionRaw = VK_NULL_HANDLE;  // float px (print dif)
        VkBuffer printDiffusionInfo = VK_NULL_HANDLE;  // 16B, host-visible
        VkBuffer printDiffusionComponents = VK_NULL_HANDLE;  // 32x16B, host
        void* mappedPrintDiffusionInfo = nullptr;
        void* mappedPrintDiffusionComponents = nullptr;
        VkBuffer scanA = VK_NULL_HANDLE;  // float px (scanner scratch)
        VkBuffer scanB = VK_NULL_HANDLE;  // float px (scanner scratch)
        VkBuffer scanC = VK_NULL_HANDLE;  // float px (scanner scratch)
        VkBuffer glareA = VK_NULL_HANDLE;  // float px (glare scratch)
        VkBuffer glareB = VK_NULL_HANDLE;  // float px (glare scratch)
        // Shared spatial scratch (P1 aliasing): halation scatter/bounce,
        // DIR correction, and scanner post run sequentially with full
        // SHADER_WRITE->SHADER_READ barriers between stages, so they reuse
        // the same 3 full-res buffers instead of 4+3+5 privates. Mapping:
        // halation A->S0, B/D->S1 (D reuses B after resolve, B dead), C->S2;
        // DIR A->S0, B->S1, C->S2; scanner scanA->S0, scanB->S1, scanC and
        // glare temps ->S2 (glare dead after op3, scanC starts op4).
        // Diffusion keeps its own Temp(2x)/Accum/Down chain (grouped-blur
        // needs a single 2x Temp binding; splitting it would change shader
        // addressing). Legacy halationRawA-D/dirCorrection*/scan*/glare*
        // fields above stay null when shared is active (destroy skips nulls).
        VkBuffer sharedS0 = VK_NULL_HANDLE;  // float px (shared spatial 0)
        VkBuffer sharedS1 = VK_NULL_HANDLE;  // float px (shared spatial 1)
        VkBuffer sharedS2 = VK_NULL_HANDLE;  // float px (shared spatial 2)
        VkBuffer dirFloats = VK_NULL_HANDLE;       // 18 floats, host-visible
        VkBuffer dirCurves = VK_NULL_HANDLE;  // exposureCount*3 floats, host
        float* mappedDirFloats = nullptr;
        float* mappedDirCurves = nullptr;
        VkBuffer destination = VK_NULL_HANDLE;  // float px
        VkBuffer filteredEnlarger = VK_NULL_HANDLE;
        VkBuffer frameFloats = VK_NULL_HANDLE;  // host-visible
        VkBuffer frameInts = VK_NULL_HANDLE;    // host-visible
        float* mappedFloats = nullptr;
        uint32_t* mappedInts = nullptr;
        VkDescriptorSet exposureSet = VK_NULL_HANDLE;
        VkDescriptorSet developSet = VK_NULL_HANDLE;
        VkDescriptorSet finalSet = VK_NULL_HANDLE;
        VkDescriptorSet grainSet = VK_NULL_HANDLE;
        VkDescriptorSet grainProdSets[4] = {};
        VkDescriptorSet dirSets[7] = {};
        VkDescriptorSet halationSets[13] = {};
        VkDescriptorSet diffusionSets[3] = {};
        VkDescriptorSet printSets[2] = {};
        VkDescriptorSet scanSets[9] = {};
        VkDescriptorSet inputSet = VK_NULL_HANDLE;
        VkDescriptorSet outputSet = VK_NULL_HANDLE;
        // Dedicated glow-factor export set (2-buffer + image layout): the tap
        // image (binding 2) is bound once per record at record start, before
        // any dispatch; only the source buffers (bindings 0/1) are rebound
        // mid-record, matching the proven buffer-rebind pattern. Never shares
        // outputSet, so the final output path is untouched with or without a tap.
        VkDescriptorSet glowRatioSet = VK_NULL_HANDLE;
        // Tile-memory arena (tiledMemorySaving only): working-size pixel
        // buffers + a full private core-set bundle bound to them. Shared
        // host-visible/small buffers (frameFloats/Ints, diffusion info,
        // dirFloats/Curves, filteredEnlarger, full source) are aliased from
        // the slot. No boost buffers: tiled + boost is rejected (record-only
        // cannot do the GPU-readback milestone).
        struct TileBufs {
            VkBuffer filmRaw = VK_NULL_HANDLE;
            VkBuffer filmDensity = VK_NULL_HANDLE;
            VkBuffer grainDensity = VK_NULL_HANDLE;
            VkBuffer grainDensityB = VK_NULL_HANDLE;
            VkBuffer grainMicroA = VK_NULL_HANDLE;
            VkBuffer grainMicroB = VK_NULL_HANDLE;
            VkBuffer grainLayerA = VK_NULL_HANDLE;
            VkBuffer grainLayerB = VK_NULL_HANDLE;
            VkBuffer dirCorrectionA = VK_NULL_HANDLE;
            VkBuffer dirCorrectionB = VK_NULL_HANDLE;
            VkBuffer dirCorrectionC = VK_NULL_HANDLE;
            VkBuffer dirDensity = VK_NULL_HANDLE;
            VkBuffer halationRawA = VK_NULL_HANDLE;
            VkBuffer halationRawB = VK_NULL_HANDLE;
            VkBuffer halationRawC = VK_NULL_HANDLE;
            VkBuffer halationRawD = VK_NULL_HANDLE;
            VkBuffer halationLogRaw = VK_NULL_HANDLE;
            VkBuffer diffusionTemp = VK_NULL_HANDLE;
            VkBuffer diffusionAccum = VK_NULL_HANDLE;
            VkBuffer diffusionDownSource = VK_NULL_HANDLE;
            VkBuffer diffusionDownTemp = VK_NULL_HANDLE;
            VkBuffer diffusionDownBlur = VK_NULL_HANDLE;
            VkBuffer cameraDiffusionRaw = VK_NULL_HANDLE;
            VkBuffer printDiffusionRaw = VK_NULL_HANDLE;
            VkBuffer scanA = VK_NULL_HANDLE;
            VkBuffer scanB = VK_NULL_HANDLE;
            VkBuffer scanC = VK_NULL_HANDLE;
            VkBuffer glareA = VK_NULL_HANDLE;
            VkBuffer glareB = VK_NULL_HANDLE;
            VkBuffer dest = VK_NULL_HANDLE;
            // Boost milestone (tiled + boost only): working-size boosted raw,
            // FULL-frame chunk maxima (16B stride, tiny), and host-visible
            // 16B readback the ReduceMax writes directly.
            VkBuffer boostedRaw = VK_NULL_HANDLE;
            VkBuffer chunks = VK_NULL_HANDLE;
            VkBuffer boostReadback = VK_NULL_HANDLE;
            void* mappedBoostReadback = nullptr;
        } tile;
        VkDescriptorSet tileExposureSet = VK_NULL_HANDLE;
        VkDescriptorSet tileDevelopSet = VK_NULL_HANDLE;
        VkDescriptorSet tileFinalSet = VK_NULL_HANDLE;
        VkDescriptorSet tileGrainSet = VK_NULL_HANDLE;
        VkDescriptorSet tileGrainProdSets[4] = {};
        VkDescriptorSet tileDirSets[7] = {};
        VkDescriptorSet tileScanSets[9] = {};
        VkDescriptorSet tileHalationSets[13] = {};
        VkDescriptorSet tileDiffusionSets[3] = {};
        VkDescriptorSet tilePrintSets[2] = {};
        VkDescriptorSet tileBoostMaxSet = VK_NULL_HANDLE;
        VkDescriptorSet tileBoostReduceSet = VK_NULL_HANDLE;
    };
    std::vector<Slot> slots;
    std::vector<VkDeviceMemory> ownedMemory;

    explicit Impl(const SpektraFilmCreateInfo& ci)
        : context(ci.context),
          queue(ci.queue),
          queueFamily(ci.queueFamilyIndex),
          slotCount(ci.maxFramesInFlight),
          maxWidth(ci.maxWidth),
          maxHeight(ci.maxHeight),
          bakedLook(ci.look),
          conditionalScratch(ci.conditionalEffectScratch),
          diagnosticTransfers(ci.diagnosticTransfers),
          tileMemorySaving(ci.tiledMemorySaving),
          maxTileW(std::max(ci.maxTileWidth, 64u)),
          maxTileH(std::max(ci.maxTileHeight, 64u)),
          tileArenaOv(tileArenaOverlapForBaked(ci.look)),
          tileArenaW(std::min(ci.maxWidth, maxTileW + 2u * tileArenaOverlapForBaked(ci.look))),
          tileArenaH(std::min(ci.maxHeight, maxTileH + 2u * tileArenaOverlapForBaked(ci.look))) {
        const char* reason = nullptr;
        if (!SpektraFilm::validateCreateInfo(ci, &reason)) {
            throw std::invalid_argument(reason ? reason : "invalid create info");
        }
        try {
            filmCurves = spektrafilm::filmProfileCurves(ci.look.film);
            paperCurves = spektrafilm::paperProfileCurves(ci.look.paper);
            if (!filmCurves || !filmCurves->densityCurveLayers ||
                !filmCurves->densityCurveLayerMaxima) {
                throw std::invalid_argument(
                    "spektrafilm: film profile lacks grain layer tables");
            }
            bakedCamera.uvEnabled = ci.look.cameraUvFilterEnabled;
            bakedCamera.uvCutNm = ci.look.cameraUvCutNm;
            bakedCamera.irEnabled = ci.look.cameraIrFilterEnabled;
            bakedCamera.irCutNm = ci.look.cameraIrCutNm;
            bakedMethod = ci.look.rgbToRawMethod;
            if (!tables::buildStaticTables(
                    ci.look.film, ci.look.paper, ci.look.rgbToRawMethod,
                    bakedCamera, ci.hanatosSpectra,
                    ci.hanatosSpectraFloats, ci.gamutCompression,
                    ci.gamutCompressionFloats, tables, &reason)) {
                throw std::runtime_error(reason ? reason : "table build failed");
            }
            // Retain spectra for debounced rebuilds (size validated above).
            {
                const spektrafilm::HanatosSpectraLutInfo& hanatosInfo =
                    spektrafilm::hanatosSpectraLutInfo();
                const size_t spectraCount =
                    static_cast<size_t>(hanatosInfo.width) * hanatosInfo.height *
                    hanatosInfo.wavelengthCount;
                hanatosSpectraData.assign(ci.hanatosSpectra,
                                          ci.hanatosSpectra + spectraCount);
            }
            checkDeviceLimits();
            uploadStatics();
            createLayoutsAndPipelines(ci);
            createSlots();
            writeStaticDescriptorSets();
        } catch (...) {
            destroy();
            throw;
        }
    }

    ~Impl() { destroy(); }

    void checkDeviceLimits() {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(context.physicalDevice, &props);
        if (props.limits.maxPushConstantsSize < kCorePushBytes) {
            fail("maxPushConstantsSize < 120");
        }
        const VkDeviceSize biggest =
            static_cast<VkDeviceSize>(spektrafilm::kSpektraColorSpaceCount) *
            spektrafilm::kSpektraColorTransferLutSize * sizeof(float);
        if (biggest > props.limits.maxStorageBufferRange) {
            fail("encode LUT exceeds maxStorageBufferRange");
        }
        VkFormatProperties fmt{};
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice,
                                            VK_FORMAT_R8G8B8A8_UNORM, &fmt);
        if ((fmt.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
            fail("RGBA8 storage image unsupported");
        }
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice,
                                            VK_FORMAT_R16G16B16A16_SFLOAT, &fmt);
        const auto need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((fmt.optimalTilingFeatures & need) != need) {
            fail("RGBA16F storage image unsupported");
        }
    }

    VkBuffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags memFlags, void** mapped,
                        VkDeviceMemory* memoryOut = nullptr) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = std::max<VkDeviceSize>(size, 4);
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        checkVk(vkCreateBuffer(context.device, &bi, context.allocator, &buffer),
                "create buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(context.device, buffer, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            findMemoryType(context.physicalDevice, req.memoryTypeBits, memFlags);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        checkVk(vkAllocateMemory(context.device, &ai, context.allocator, &memory),
                "alloc buffer memory");
        checkVk(vkBindBufferMemory(context.device, buffer, memory, 0),
                "bind buffer memory");
        ownedMemory.push_back(memory);
        if (memoryOut) {
            *memoryOut = memory;
        }
        if (mapped) {
            checkVk(vkMapMemory(context.device, memory, 0, req.size, 0, mapped),
                    "map buffer memory");
        }
        return buffer;
    }

    // One-shot init upload: host-visible + immediate memcpy (same pattern as
    // TonemapEngine's LUT buffers). No staging, no submit; record() stays
    // submit-free and the init queue is currently unused.
    void addStatic(StaticId id, const void* data, VkDeviceSize bytes) {
        StaticBuffer entry{};
        void* mapped = nullptr;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        entry.buffer = makeBuffer(
            bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &mapped, &memory);
        entry.memory = memory;
        entry.memory = memory;
        std::memcpy(mapped, data, static_cast<size_t>(bytes));
        vkUnmapMemory(context.device, memory);
        statics[id] = entry;
    }

    // Re-uploads one static buffer with same-size data (rebuild paths).
    // Buffers stay host-visible, so no submit is needed; the next record()'s
    // host barrier covers visibility. Call only while the GPU is idle on
    // prior spektra work using this engine.
    void uploadToStatic(StaticId id, const std::vector<float>& data) {
        const StaticBuffer& entry = statics[id];
        void* mapped = nullptr;
        checkVk(vkMapMemory(context.device, entry.memory, 0, VK_WHOLE_SIZE, 0,
                            &mapped),
                "map static for re-upload");
        std::memcpy(mapped, data.data(), data.size() * sizeof(float));
        vkUnmapMemory(context.device, entry.memory);
    }

    void uploadStatics() {
        using spektrafilm::kSpektraColorSpaceCount;
        using spektrafilm::kSpektraColorTransferLutSize;
        statics.assign(kStaticCount, StaticBuffer{});
        const uint32_t wave = tables.wavelengthCount;
        const auto fBytes = [](size_t n) {
            return static_cast<VkDeviceSize>(n * sizeof(float));
        };
        addStatic(kPackedFilmExposure, tables.packedFilmCurveExposure.data(),
                  fBytes(tables.packedFilmCurveExposure.size()));
        addStatic(kFilmDensityCurves, filmCurves->densityCurves,
                  fBytes(static_cast<size_t>(tables.exposureCount) * 3u));
        addStatic(kInputToRefXyz, filmCurves->inputToReferenceXyz,
                  fBytes(static_cast<size_t>(kSpektraColorSpaceCount) * 9u));
        addStatic(kInputToSrgb, filmCurves->inputToSrgb,
                  fBytes(static_cast<size_t>(kSpektraColorSpaceCount) * 9u));
        addStatic(kDecodeLuts, spektrafilm::colorDecodeLuts(),
                  fBytes(static_cast<size_t>(kSpektraColorSpaceCount) *
                         kSpektraColorTransferLutSize));
        addStatic(kTransferKinds, spektrafilm::colorTransferKinds(),
                  static_cast<VkDeviceSize>(kSpektraColorSpaceCount * sizeof(uint32_t)));
        addStatic(kMallett, tables.mallettRawMatrix.data(), fBytes(9));
        addStatic(kHanatosResponse, tables.hanatosRawResponse.data(),
                  fBytes(tables.hanatosRawResponse.size()));
        addStatic(kPackedPaperExposure, tables.packedPaperCurveExposure.data(),
                  fBytes(tables.packedPaperCurveExposure.size()));
        addStatic(kPaperDensityCurves, paperCurves->densityCurves,
                  fBytes(static_cast<size_t>(tables.paperExposureCount) * 3u));
        addStatic(kFilmSpectral, tables.packedFilmSpectralDensity.data(),
                  fBytes(tables.packedFilmSpectralDensity.size()));
        addStatic(kFilmBase, filmCurves->baseDensity, fBytes(wave));
        addStatic(kPaperSensitivity, tables.paperSensitivityLinear.data(),
                  fBytes(tables.paperSensitivityLinear.size()));
        addStatic(kPaperSpectral, tables.packedPaperSpectralDensity.data(),
                  fBytes(tables.packedPaperSpectralDensity.size()));
        addStatic(kPaperBase, paperCurves->baseDensity, fBytes(wave));
        addStatic(kScanProducts, tables.scanProducts.data(),
                  fBytes(tables.scanProducts.size()));
        addStatic(kThKg3, spektrafilm::thKg3Illuminant(), fBytes(wave));
        addStatic(kCustomFilters, spektrafilm::customEnlargerFilters(),
                  fBytes(static_cast<size_t>(wave) * 3u));
        addStatic(kNeutralFilters, spektrafilm::neutralPrintFilters(),
                  fBytes(static_cast<size_t>(spektrafilm::kSpektraPaperCount) *
                         spektrafilm::kSpektraFilmCount * 3u));
        addStatic(kCmfs, spektrafilm::standardObserverCmfs(),
                  fBytes(static_cast<size_t>(wave) * 3u));
        addStatic(kFilmScanToOutput, filmCurves->scanToOutputRgb,
                  fBytes(static_cast<size_t>(kSpektraColorSpaceCount) * 9u));
        addStatic(kPaperScanToOutput, paperCurves->scanToOutputRgb,
                  fBytes(static_cast<size_t>(kSpektraColorSpaceCount) * 9u));
        addStatic(kEncodeAndGamut, tables.colorEncodeAndGamut.data(),
                  fBytes(tables.colorEncodeAndGamut.size()));
        addStatic(kAcademy, spektrafilm::academyPrinterDensityData(),
                  fBytes(static_cast<size_t>(wave) * 3u +
                         static_cast<size_t>(spektrafilm::kSpektraPaperCount) *
                             spektrafilm::kSpektraFilmCount * 3u));
        addStatic(kPaperHanatos, tables.paperHanatosResponse.data(),
                  fBytes(tables.paperHanatosResponse.size()));
        addStatic(kPreflashHanatos, tables.preflashPaperHanatosResponse.data(),
                  fBytes(tables.preflashPaperHanatosResponse.size()));
        addStatic(kDensityCurveLayers, filmCurves->densityCurveLayers,
                  fBytes(static_cast<size_t>(tables.exposureCount) * 9u));
        addStatic(kDensityCurveLayerMaxima, filmCurves->densityCurveLayerMaxima,
                  fBytes(9u));
        // Dummy buffer backing never-read core bindings (see member note).
        StaticBuffer dummy{};
        dummy.buffer = makeBuffer(
            16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
        dummyBuffer = dummy.buffer;
    }

    VkShaderModule makeModule(const uint32_t* words, size_t bytes, const char* what) {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = bytes;
        mi.pCode = words;
        VkShaderModule module = VK_NULL_HANDLE;
        checkVk(vkCreateShaderModule(context.device, &mi, context.allocator, &module),
                what);
        return module;
    }

    VkPipeline makeComputePipeline(VkPipelineLayout layout, VkShaderModule module) {
        VkComputePipelineCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = module;
        pi.stage.pName = "main";
        pi.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        checkVk(vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1, &pi,
                                         context.allocator, &pipeline),
                "create compute pipeline");
        return pipeline;
    }

    void createLayoutsAndPipelines(const SpektraFilmCreateInfo& ci) {
        // Core layout: bindings 0..30, all STORAGE_BUFFER (upstream shares one
        // 31-binding layout across exposure/develop/printscan).
        std::vector<VkDescriptorSetLayoutBinding> coreBindings;
        for (uint32_t b = 0; b <= 30; ++b) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = b;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            coreBindings.push_back(binding);
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = static_cast<uint32_t>(coreBindings.size());
        li.pBindings = coreBindings.data();
        checkVk(vkCreateDescriptorSetLayout(context.device, &li, context.allocator,
                                            &coreLayout),
                "create core layout");
        VkPushConstantRange corePush{};
        corePush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        corePush.size = kCorePushBytes;
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &coreLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &corePush;
        checkVk(vkCreatePipelineLayout(context.device, &pli, context.allocator,
                                       &corePipelineLayout),
                "create core pipeline layout");

        VkShaderModule exposure = makeModule(ci.exposureSpirv, ci.exposureSpirvBytes,
                                             "exposure shader module");
        VkShaderModule develop =
            makeModule(ci.developSpirv, ci.developSpirvBytes, "develop shader module");
        VkShaderModule printScan = makeModule(ci.printScanSpirv, ci.printScanSpirvBytes,
                                              "printscan shader module");
        VkShaderModule grain =
            makeModule(ci.grainSpirv, ci.grainSpirvBytes, "grain shader module");
        VkShaderModule dir =
            makeModule(ci.dirSpirv, ci.dirSpirvBytes, "dir shader module");
        VkShaderModule halation = makeModule(ci.halationSpirv, ci.halationSpirvBytes,
                                             "halation shader module");
        VkShaderModule diffusion = makeModule(ci.diffusionSpirv, ci.diffusionSpirvBytes,
                                              "diffusion shader module");
        VkShaderModule scanner = makeModule(ci.scannerSpirv, ci.scannerSpirvBytes,
                                            "scanner shader module");
        exposurePipeline = makeComputePipeline(corePipelineLayout, exposure);
        developPipeline = makeComputePipeline(corePipelineLayout, develop);
        printScanPipeline = makeComputePipeline(corePipelineLayout, printScan);
        grainPipeline = makeComputePipeline(corePipelineLayout, grain);
        dirPipeline = makeComputePipeline(corePipelineLayout, dir);
        halationPipeline = makeComputePipeline(corePipelineLayout, halation);
        diffusionPipeline = makeComputePipeline(corePipelineLayout, diffusion);
        scannerPipeline = makeComputePipeline(corePipelineLayout, scanner);
        vkDestroyShaderModule(context.device, exposure, context.allocator);
        vkDestroyShaderModule(context.device, develop, context.allocator);
        vkDestroyShaderModule(context.device, printScan, context.allocator);
        vkDestroyShaderModule(context.device, grain, context.allocator);
        vkDestroyShaderModule(context.device, dir, context.allocator);
        vkDestroyShaderModule(context.device, halation, context.allocator);
        vkDestroyShaderModule(context.device, diffusion, context.allocator);
        vkDestroyShaderModule(context.device, scanner, context.allocator);

        // IO layout: binding 0 buffer + binding 1 storage image, 8B push.
        std::array<VkDescriptorSetLayoutBinding, 2> ioBindings{};
        ioBindings[0].binding = 0;
        ioBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ioBindings[0].descriptorCount = 1;
        ioBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        ioBindings[1].binding = 1;
        ioBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ioBindings[1].descriptorCount = 1;
        ioBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        li.bindingCount = 2;
        li.pBindings = ioBindings.data();
        checkVk(vkCreateDescriptorSetLayout(context.device, &li, context.allocator,
                                            &ioLayout),
                "create io layout");
        VkPushConstantRange ioPush{};
        ioPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        ioPush.size = kIoPushBytes;
        pli.pSetLayouts = &ioLayout;
        pli.pPushConstantRanges = &ioPush;
        checkVk(vkCreatePipelineLayout(context.device, &pli, context.allocator,
                                       &ioPipelineLayout),
                "create io pipeline layout");
        VkShaderModule input = makeModule(ci.inputSpirv, ci.inputSpirvBytes,
                                          "input shader module");
        VkShaderModule output = makeModule(ci.outputSpirv, ci.outputSpirvBytes,
                                           "output shader module");
        inputPipeline = makeComputePipeline(ioPipelineLayout, input);
        outputPipeline = makeComputePipeline(ioPipelineLayout, output);
        vkDestroyShaderModule(context.device, input, context.allocator);
        vkDestroyShaderModule(context.device, output, context.allocator);
        if (ci.hdrOutputSpirv && ci.hdrOutputSpirvBytes) {
            VkShaderModule outputHalf =
                makeModule(ci.hdrOutputSpirv, ci.hdrOutputSpirvBytes,
                           "half output shader module");
            outputHalfPipeline =
                makeComputePipeline(ioPipelineLayout, outputHalf);
            vkDestroyShaderModule(context.device, outputHalf,
                                  context.allocator);
        }
        if (ci.boostMilestoneSpirv && ci.boostMilestoneSpirvBytes) {
            VkShaderModule milestone =
                makeModule(ci.boostMilestoneSpirv, ci.boostMilestoneSpirvBytes,
                           "boost milestone shader module");
            milestonePipeline =
                makeComputePipeline(corePipelineLayout, milestone);
            vkDestroyShaderModule(context.device, milestone,
                                  context.allocator);
        }
        // Glow-ratio layout: bindings 0/1 storage buffers (pre/post scatter
        // linear) + binding 2 storage image, 8B push (width/height).
        std::array<VkDescriptorSetLayoutBinding, 3> glowBindings{};
        glowBindings[0].binding = 0;
        glowBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        glowBindings[0].descriptorCount = 1;
        glowBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        glowBindings[1].binding = 1;
        glowBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        glowBindings[1].descriptorCount = 1;
        glowBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        glowBindings[2].binding = 2;
        glowBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        glowBindings[2].descriptorCount = 1;
        glowBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        li.bindingCount = 3;
        li.pBindings = glowBindings.data();
        checkVk(vkCreateDescriptorSetLayout(context.device, &li, context.allocator,
                                            &glowRatioLayout),
                "create glow ratio layout");
        VkPushConstantRange glowPush{};
        glowPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        glowPush.size = 8u * sizeof(uint32_t);
        pli.pSetLayouts = &glowRatioLayout;
        pli.pPushConstantRanges = &glowPush;
        checkVk(vkCreatePipelineLayout(context.device, &pli, context.allocator,
                                       &glowRatioPipelineLayout),
                "create glow ratio pipeline layout");
        if (ci.glowRatioSpirv && ci.glowRatioSpirvBytes) {
            VkShaderModule glowRatio =
                makeModule(ci.glowRatioSpirv, ci.glowRatioSpirvBytes,
                           "glow ratio shader module");
            glowRatioPipeline =
                makeComputePipeline(glowRatioPipelineLayout, glowRatio);
            vkDestroyShaderModule(context.device, glowRatio,
                                  context.allocator);
        }

        std::array<VkDescriptorPoolSize, 2> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        // 42 core + 2 IO (input, output) + 2 glow-ratio per slot, plus 44
        // core for the tile-memory bundle. The IO and glow-ratio sets hold
        // one storage image each; buffer counts stay a loose over-estimate.
        const uint32_t setsPerSlot = tileMemorySaving ? 90u : 46u;
        poolSizes[0].descriptorCount = slotCount * (setsPerSlot * 31u + 2u);
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[1].descriptorCount = slotCount * 4u;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = slotCount * setsPerSlot;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes.data();
        checkVk(vkCreateDescriptorPool(context.device, &poolInfo, context.allocator,
                                       &descriptorPool),
                "create descriptor pool");
    }

    void createSlots() {
        const VkDeviceSize pixelBytes =
            static_cast<VkDeviceSize>(maxWidth) * maxHeight * 16u;
        const VkDeviceSize filteredBytes =
            static_cast<VkDeviceSize>(tables.wavelengthCount) * 8u * sizeof(float);
        const VkBufferUsageFlags pixelUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            (diagnosticTransfers ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0);
        // Conditional scratch (still engines): skip effect buffers whose
        // enable flags are off in the baked look. record() only runs an
        // effect when its buffers exist, and the caller recreates when the
        // gated flags change, so flags always match allocation.
        const bool wantPreviewGrain =
            !conditionalScratch || bakedLook.grainEnabled;
        // Production layers are gated on the baked model even for preview
        // (non-conditional) engines: the app bakes preview looks with all
        // enables forced on for instant toggling, but keeps the real grain
        // model, so Preview-model previews skip 9 layer buffers (~35% of the
        // preview arena). The caller must recreate on grainModel flips
        // (preview bakedChanged + still scratchMatch both cover it); without
        // a rebuild record() safely skips production grain when its buffers
        // are absent.
        const bool wantProdGrain =
            bakedLook.grainEnabled && bakedLook.grainModel != 0;
        const bool wantHalation =
            !conditionalScratch || bakedLook.halationEnabled;
        const bool wantCameraDiffusion =
            !conditionalScratch || bakedLook.cameraDiffusionEnabled;
        const bool wantPrintDiffusion =
            !conditionalScratch || bakedLook.printDiffusionEnabled;
        const bool wantScanner =
            !conditionalScratch || bakedLook.scannerEnabled;
        // DIR has no enable flag; gate on amount>0 for conditional (still)
        // engines. Preview (non-conditional) keeps it for instant slider
        // response; quarter-res cost is small. Stills rebuild when amount
        // crosses 0 via scratchMatch.
        const bool wantDir =
            !conditionalScratch || bakedLook.dirCouplersAmount > 0.0f;
        // Shared spatial scratch replaces halationRawA-D, dirCorrectionA-C,
        // and scanA-C/glareA-B (sequential lifetimes, barrier-separated).
        // It also backs the sliced production-grain blur exchange (3 comps
        // at a time; see kOpLayerBlurXSlice/YSlice), so prod grain joins the
        // sharer set. Diffusion keeps its own Temp(2x)/Accum/Down chain.
        const bool wantSharedSpatial =
            wantHalation || wantDir || wantScanner || wantProdGrain;
        // Log raw doubles as the develop input for camera diffusion when
        // halation itself is off (upstream allocates it in that case too).
        const bool wantLogRaw = !conditionalScratch ||
                                bakedLook.halationEnabled ||
                                bakedLook.cameraDiffusionEnabled;
        slots.resize(slotCount);
        for (auto& slot : slots) {
            slot.source = makeBuffer(pixelBytes, pixelUsage,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            // Full-frame effect scratch is skipped on tile-memory engines
            // (only source/destination/filteredEnlarger + host buffers stay
            // full-frame); the tile arena below replaces it.
            const bool wantFullFx = !tileMemorySaving;
            if (wantFullFx) {
                slot.filmRaw = makeBuffer(pixelBytes, pixelUsage,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.filmDensity = makeBuffer(pixelBytes, pixelUsage,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            }
            if (wantPreviewGrain && wantFullFx) {
                slot.grainDensity = makeBuffer(pixelBytes, pixelUsage,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            }
            if (wantProdGrain && wantFullFx) {
                slot.grainDensityB = makeBuffer(pixelBytes, pixelUsage,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.grainMicroA = makeBuffer(pixelBytes, pixelUsage,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.grainMicroB = makeBuffer(pixelBytes, pixelUsage,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                // Nine float layers per pixel (36 B), not nine vec4s (144 B).
                const VkDeviceSize layerBytes = pixelBytes * 9u / 4u;
                slot.grainLayerA = makeBuffer(layerBytes, pixelUsage,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                // grainLayerB intentionally never allocated: sliced blur ops
                // (14/15) exchange 3 components at a time through sharedS0
                // (16B/px vec4 store, 12B used). See record() grain loops.
            }
            if (wantDir && wantFullFx) {
                slot.dirDensity = makeBuffer(pixelBytes, pixelUsage,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            }
            // Shared spatial arena (replaces halationRawA-D +
            // dirCorrectionA-C + scanA-C/glareA-B). Legacy fields stay null
            // (destroy skips nulls; descriptors bind shared below).
            if (wantSharedSpatial && wantFullFx) {
                slot.sharedS0 = makeBuffer(pixelBytes, pixelUsage,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.sharedS1 = makeBuffer(pixelBytes, pixelUsage,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.sharedS2 = makeBuffer(pixelBytes, pixelUsage,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            }
            if (wantLogRaw && wantFullFx) {
                slot.halationLogRaw = makeBuffer(pixelBytes, pixelUsage,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            }
            if (wantHalation && wantFullFx) {
                slot.halationBoostedRaw = makeBuffer(pixelBytes, pixelUsage,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                // Boost max-reduction: one float4 per 256-px chunk, sized for the
                // full arena (upstream kHalationBoostMaxChunkPixels = 256).
                const VkDeviceSize boostChunks =
                    ((static_cast<VkDeviceSize>(maxWidth) * maxHeight + 255u) / 256u) *
                    4u * sizeof(float);
                slot.halationBoostChunks = makeBuffer(boostChunks, pixelUsage,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.halationBoostInfo = makeBuffer(4u * sizeof(float), pixelUsage,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            }
            // Diffusion Temp/Accum/Down chain is shared by camera and print
            // sequences (never concurrent). cameraDiffusionRaw doubles as the
            // print-diffusion raw (also never concurrent: camera pre-develop,
            // print post-printRaw); printDiffusionRaw stays null to save 1 buf.
            const bool wantAnyDiffusion = wantCameraDiffusion || wantPrintDiffusion;
            if (wantAnyDiffusion && wantFullFx) {
                slot.diffusionTemp = makeBuffer(pixelBytes * 2u, pixelUsage,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.diffusionAccum = makeBuffer(pixelBytes, pixelUsage,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                // Downsample chain sized for scale 2 (largest reduced size;
                // scale 4/8 dispatches address a prefix of these buffers).
                const VkDeviceSize downW = (maxWidth + 1u) / 2u;
                const VkDeviceSize downH = (maxHeight + 1u) / 2u;
                const VkDeviceSize downBytes = downW * downH * 16u;
                slot.diffusionDownSource = makeBuffer(downBytes, pixelUsage,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.diffusionDownTemp = makeBuffer(downBytes * 2u, pixelUsage,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.diffusionDownBlur = makeBuffer(downBytes * 2u, pixelUsage,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                slot.cameraDiffusionRaw = makeBuffer(pixelBytes, pixelUsage,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            }
            slot.diffusionInfo = makeBuffer(
                16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &slot.mappedDiffusionInfo);
            slot.diffusionComponents = makeBuffer(
                32u * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &slot.mappedDiffusionComponents);
            // printDiffusionRaw intentionally never allocated: print sequence
            // reuses cameraDiffusionRaw (see above). Field stays null.
            slot.printDiffusionInfo = makeBuffer(
                16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &slot.mappedPrintDiffusionInfo);
            slot.printDiffusionComponents = makeBuffer(
                32u * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &slot.mappedPrintDiffusionComponents);
            // Scanner scratch intentionally never allocated standalone:
            // scanner reuses the shared S0/S1/S2 arena (see above).
            slot.dirFloats = makeBuffer(
                18u * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                reinterpret_cast<void**>(&slot.mappedDirFloats));
            slot.dirCurves = makeBuffer(
                static_cast<VkDeviceSize>(tables.exposureCount) * 3u * sizeof(float),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                reinterpret_cast<void**>(&slot.mappedDirCurves));
            slot.destination = makeBuffer(pixelBytes, pixelUsage,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            slot.filteredEnlarger = makeBuffer(
                filteredBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            slot.frameFloats = makeBuffer(
                105u * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                reinterpret_cast<void**>(&slot.mappedFloats));
            slot.frameInts = makeBuffer(
                29u * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                reinterpret_cast<void**>(&slot.mappedInts));
            // Tile-memory arena (tiledMemorySaving only): working-size effect
            // scratch shared across tiles of a record (tiles run sequentially
            // in one command buffer). Gated by the same baked enable flags;
            // DIR is always reserved (no enable flag, toggleable per record).
            // No boost buffers: tiled + boost is rejected.
            if (tileMemorySaving) {
                const VkDeviceSize tileBytes =
                    static_cast<VkDeviceSize>(tileArenaW) * tileArenaH * 16u;
                const VkDeviceSize tileDownW = tileArenaW / 2u + 2u;
                const VkDeviceSize tileDownH = tileArenaH / 2u + 2u;
                const VkDeviceSize tileDownBytes = tileDownW * tileDownH * 16u;
                auto& t = slot.tile;
                // Halation, DIR, production-grain blur, and scanner post run
                // in that order. Their spatial intermediates never overlap
                // in lifetime, so bind the same buffers in those phases.
                const uint32_t spatialCount =
                    wantScanner ? 5u : wantHalation ? 4u :
                    wantDir ? 3u : wantProdGrain ? 1u : 0u;
                VkBuffer spatial[5] = {};
                for (uint32_t i = 0; i < spatialCount; ++i) {
                    spatial[i] = makeBuffer(tileBytes, pixelUsage,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                }
                t.filmRaw = makeBuffer(tileBytes, pixelUsage,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                t.filmDensity = makeBuffer(tileBytes, pixelUsage,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                if (wantPreviewGrain) {
                    t.grainDensity = makeBuffer(tileBytes, pixelUsage,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                }
                if (wantProdGrain) {
                    t.grainDensityB = makeBuffer(tileBytes, pixelUsage,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    t.grainMicroA = makeBuffer(tileBytes, pixelUsage,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    t.grainMicroB = makeBuffer(tileBytes, pixelUsage,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    const VkDeviceSize tileLayerBytes = tileBytes * 9u / 4u;
                    t.grainLayerA = makeBuffer(tileLayerBytes, pixelUsage,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    // Sliced blur exchanges three of nine layers at a time.
                    // The shader addresses only 3 floats per pixel in this
                    // buffer, so one vec4-sized tile allocation suffices.
                    t.grainLayerB = spatial[0];
                }
                if (wantDir) {
                    t.dirCorrectionA = spatial[0];
                    t.dirCorrectionB = spatial[1];
                    t.dirCorrectionC = spatial[2];
                    t.dirDensity = makeBuffer(tileBytes, pixelUsage,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                }
                if (wantHalation) {
                    t.halationRawA = spatial[0];
                    t.halationRawB = spatial[1];
                    t.halationRawC = spatial[2];
                    t.halationRawD = spatial[3];
                    t.boostedRaw = makeBuffer(tileBytes, pixelUsage,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    // Chunk maxima cover the FULL frame (16B stride for the
                    // op8 reader); the 16B readback is host-visible so the
                    // ReduceMax writes it directly (submit+wait, then map).
                    const VkDeviceSize chunkCount =
                        (static_cast<VkDeviceSize>(maxWidth) * maxHeight +
                         255u) /
                        256u;
                    t.chunks = makeBuffer(chunkCount * 16u, pixelUsage,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          nullptr);
                    t.boostReadback = makeBuffer(
                        4u * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &t.mappedBoostReadback);
                }
                if (wantLogRaw) {
                    t.halationLogRaw = makeBuffer(tileBytes, pixelUsage,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                }
                if (wantCameraDiffusion) {
                    t.diffusionTemp = makeBuffer(tileBytes * 2u, pixelUsage,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    t.diffusionAccum = makeBuffer(tileBytes, pixelUsage,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    t.diffusionDownSource = makeBuffer(tileDownBytes, pixelUsage,
                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    t.diffusionDownTemp = makeBuffer(tileDownBytes * 2u, pixelUsage,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    t.diffusionDownBlur = makeBuffer(tileDownBytes * 2u, pixelUsage,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                    t.cameraDiffusionRaw = makeBuffer(tileBytes, pixelUsage,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                }
                if (wantPrintDiffusion) {
                    t.printDiffusionRaw = makeBuffer(tileBytes, pixelUsage,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
                }
                if (wantScanner) {
                    t.scanA = spatial[0];
                    t.scanB = spatial[1];
                    t.scanC = spatial[2];
                    t.glareA = spatial[3];
                    t.glareB = spatial[4];
                }
                t.dest = makeBuffer(tileBytes, pixelUsage,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr);
            }
        }
        std::vector<VkDescriptorSetLayout> layouts;
        for (uint32_t s = 0; s < slotCount; ++s) {
            for (uint32_t i = 0; i < 46u; ++i) {
                layouts.push_back(i < 42u ? coreLayout : (i < 44u ? ioLayout : glowRatioLayout));
            }
            if (tileMemorySaving) {
                // Tile-memory bundle mirrors the 42 core sets (no IO sets)
                // plus 2 boost-milestone sets (chunk max + reduce).
                for (uint32_t i = 0; i < 44u; ++i) {
                    layouts.push_back(coreLayout);
                }
            }
        }
        std::vector<VkDescriptorSet> sets(layouts.size());
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptorPool;
        ai.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        ai.pSetLayouts = layouts.data();
        checkVk(vkAllocateDescriptorSets(context.device, &ai, sets.data()),
                "alloc descriptor sets");
        for (uint32_t s = 0; s < slotCount; ++s) {
            const uint32_t base = s * (tileMemorySaving ? 90u : 46u);
            slots[s].exposureSet = sets[base];
            slots[s].developSet = sets[base + 1u];
            slots[s].finalSet = sets[base + 2u];
            slots[s].grainSet = sets[base + 3u];
            for (int d = 0; d < 7; ++d) {
                slots[s].dirSets[d] = sets[base + 4u + d];
            }
            for (int d = 0; d < 9; ++d) {
                slots[s].scanSets[d] = sets[base + 11u + d];
            }
            for (int d = 0; d < 13; ++d) {
                slots[s].halationSets[d] = sets[base + 20u + d];
            }
            for (int d = 0; d < 3; ++d) {
                slots[s].diffusionSets[d] = sets[base + 33u + d];
            }
            for (int d = 0; d < 2; ++d) {
                slots[s].printSets[d] = sets[base + 36u + d];
            }
            for (int d = 0; d < 4; ++d) {
                slots[s].grainProdSets[d] = sets[base + 38u + d];
            }
            slots[s].inputSet = sets[base + 42u];
            slots[s].outputSet = sets[base + 43u];
            slots[s].glowRatioSet = sets[base + 44u];
            if (tileMemorySaving) {
                const uint32_t tbase = base + 46u;
                slots[s].tileExposureSet = sets[tbase];
                slots[s].tileDevelopSet = sets[tbase + 1u];
                slots[s].tileFinalSet = sets[tbase + 2u];
                slots[s].tileGrainSet = sets[tbase + 3u];
                for (int d = 0; d < 7; ++d) {
                    slots[s].tileDirSets[d] = sets[tbase + 4u + d];
                }
                for (int d = 0; d < 9; ++d) {
                    slots[s].tileScanSets[d] = sets[tbase + 11u + d];
                }
                for (int d = 0; d < 13; ++d) {
                    slots[s].tileHalationSets[d] = sets[tbase + 20u + d];
                }
                for (int d = 0; d < 3; ++d) {
                    slots[s].tileDiffusionSets[d] = sets[tbase + 33u + d];
                }
                for (int d = 0; d < 2; ++d) {
                    slots[s].tilePrintSets[d] = sets[tbase + 36u + d];
                }
                for (int d = 0; d < 4; ++d) {
                    slots[s].tileGrainProdSets[d] = sets[tbase + 38u + d];
                }
                slots[s].tileBoostMaxSet = sets[tbase + 42u];
                slots[s].tileBoostReduceSet = sets[tbase + 43u];
            }
        }
    }

    void writeBufferSet(VkDescriptorSet set,
                        const std::vector<std::pair<uint32_t, VkBuffer>>& bindings) {
        std::vector<VkDescriptorBufferInfo> infos(bindings.size());
        std::vector<VkWriteDescriptorSet> writes(bindings.size());
        for (size_t i = 0; i < bindings.size(); ++i) {
            infos[i].buffer = bindings[i].second;
            infos[i].offset = 0;
            infos[i].range = VK_WHOLE_SIZE;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = bindings[i].first;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(context.device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    void writeStaticDescriptorSets() {
        const VkBuffer curve2 = statics[kPackedFilmExposure].buffer;
        const VkBuffer curve3 = statics[kFilmDensityCurves].buffer;
        // Conditionally-allocated scratch (still engines) leaves null
        // handles; bind the dummy there (never read: record() only runs an
        // effect when its buffers exist).
        const auto orDummy = [&](VkBuffer b) -> VkBuffer {
            return b ? b : dummyBuffer;
        };
        // Fully populate every core-layout binding (0..30) in each set.
        std::vector<std::pair<uint32_t, VkBuffer>> exposureExtra;
        for (uint32_t b = 10; b <= 30; ++b) {
            exposureExtra.push_back({b, dummyBuffer});
        }
        std::vector<std::pair<uint32_t, VkBuffer>> developExtra;
        for (uint32_t b = 4; b <= 30; ++b) {
            developExtra.push_back({b, dummyBuffer});
        }
        for (auto& slot : slots) {
            std::vector<std::pair<uint32_t, VkBuffer>> exposureBindings = {
                {0, slot.source},
                {1, orDummy(slot.filmRaw)},
                {2, curve2},
                {3, curve3},
                {4, statics[kInputToSrgb].buffer},
                {5, statics[kDecodeLuts].buffer},
                {6, statics[kTransferKinds].buffer},
                {7, statics[kMallett].buffer},
                {8, statics[kInputToRefXyz].buffer},
                {9, statics[kHanatosResponse].buffer}};
            exposureBindings.insert(exposureBindings.end(), exposureExtra.begin(),
                                    exposureExtra.end());
            writeBufferSet(slot.exposureSet, exposureBindings);
            std::vector<std::pair<uint32_t, VkBuffer>> developBindings = {
                {0, orDummy(slot.filmRaw)},
                {1, orDummy(slot.filmDensity)},
                {2, curve2},
                {3, curve3}};
            developBindings.insert(developBindings.end(), developExtra.begin(),
                                   developExtra.end());
            writeBufferSet(slot.developSet, developBindings);
            // Grain set: 0 in, 1 out, 8 density curves live; the rest is
            // unread by kOpPreview (dummy-backed for Metal argument buffers).
            std::vector<std::pair<uint32_t, VkBuffer>> grainBindings = {
                {0, orDummy(slot.filmDensity)},
                {1, orDummy(slot.grainDensity)},
                {8, curve3},
                {27, slot.frameFloats},
                {28, slot.frameInts}};
            for (uint32_t b : {2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u}) {
                grainBindings.push_back({b, dummyBuffer});
            }
            for (uint32_t b = 11; b <= 26; ++b) {
                grainBindings.push_back({b, dummyBuffer});
            }
            grainBindings.push_back({29, dummyBuffer});
            grainBindings.push_back({30, dummyBuffer});
            writeBufferSet(slot.grainSet, grainBindings);
            // Production grain sets mirror upstream's 4 production wirings
            // (0 source, 1 dest, 2/3 aux ping-pong, 4/5 micro scratch, 6/7
            // layer scratch, 8 density curves, 9/10 layer tables, 27/28
            // frame arrays). Written for the no-DIR source (filmDensity);
            // record() rebinds binding 0 of sets 0/3 when DIR runs.
            auto prodGrainSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
                std::vector<std::pair<uint32_t, VkBuffer>> out(live);
                bool used[31] = {};
                for (const auto& p : live) {
                    used[p.first] = true;
                }
                for (uint32_t b = 0; b <= 30; ++b) {
                    if (!used[b] && b != 27 && b != 28) {
                        out.emplace_back(b, dummyBuffer);
                    }
                }
                out.emplace_back(27, slot.frameFloats);
                out.emplace_back(28, slot.frameInts);
                return out;
            };
            const VkBuffer layerTables = statics[kDensityCurveLayers].buffer;
            const VkBuffer layerMaxima = statics[kDensityCurveLayerMaxima].buffer;
            auto prodGrainCommon = [&](std::vector<std::pair<uint32_t, VkBuffer>> out) {
                out.emplace_back(4, orDummy(slot.grainMicroA));
                out.emplace_back(5, orDummy(slot.grainMicroB));
                out.emplace_back(6, orDummy(slot.grainLayerA));
                // Binding 7 is the sliced-blur scratch (3 comps, stride 3);
                // legacy 9x LayerB no longer exists full-frame.
                out.emplace_back(7, orDummy(slot.sharedS0));
                out.emplace_back(8, curve3);
                out.emplace_back(9, layerTables);
                out.emplace_back(10, layerMaxima);
                return out;
            };
            writeBufferSet(slot.grainProdSets[0], prodGrainCommon(prodGrainSet({
                {0, orDummy(slot.filmDensity)}, {1, orDummy(slot.grainDensity)},
                {2, orDummy(slot.grainDensityB)}, {3, orDummy(slot.grainDensityB)}})));
            writeBufferSet(slot.grainProdSets[1], prodGrainCommon(prodGrainSet({
                {0, orDummy(slot.grainDensity)}, {1, orDummy(slot.grainDensityB)},
                {2, orDummy(slot.grainDensityB)}, {3, orDummy(slot.grainDensity)}})));
            writeBufferSet(slot.grainProdSets[2], prodGrainCommon(prodGrainSet({
                {0, orDummy(slot.grainDensityB)}, {1, orDummy(slot.grainDensity)},
                {2, orDummy(slot.grainDensityB)}, {3, orDummy(slot.grainDensity)}})));
            writeBufferSet(slot.grainProdSets[3], prodGrainCommon(prodGrainSet({
                {0, orDummy(slot.filmDensity)}, {1, orDummy(slot.grainDensity)},
                {2, orDummy(slot.grainDensityB)}, {3, orDummy(slot.grainDensityB)}})));
            // DIR sets mirror upstream's 7 op wirings (bindings 0-3 ping-pong
            // between density/correction scratch, 4 log exposure, 5 corrected
            // curves, 27 dir floats). Set3 binding 0 and set6 binding 2 vary
            // per record (blur/tail path) and are rebound in record().
            auto dirSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
                std::vector<std::pair<uint32_t, VkBuffer>> out(live);
                bool used[31] = {};
                for (const auto& p : live) {
                    used[p.first] = true;
                }
                for (uint32_t b = 0; b <= 30; ++b) {
                    if (!used[b]) {
                        out.emplace_back(b, dummyBuffer);
                    }
                }
                return out;
            };
            // DIR sets: correction scratch A/B/C map to shared S0/S1/S2
            // (sequential with halation/scanner, barrier-separated).
            writeBufferSet(slot.dirSets[0], dirSet({
                {0, orDummy(slot.filmDensity)}, {1, orDummy(slot.sharedS0)}, {2, orDummy(slot.sharedS0)},
                {3, orDummy(slot.sharedS0)}, {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
            writeBufferSet(slot.dirSets[1], dirSet({
                {0, orDummy(slot.sharedS0)}, {1, orDummy(slot.sharedS1)},
                {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
            writeBufferSet(slot.dirSets[2], dirSet({
                {0, orDummy(slot.sharedS1)}, {1, orDummy(slot.sharedS2)},
                {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
            writeBufferSet(slot.dirSets[3], dirSet({
                {0, orDummy(slot.sharedS0)}, {1, orDummy(slot.sharedS1)},
                {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
            writeBufferSet(slot.dirSets[4], dirSet({
                {0, orDummy(slot.sharedS0)}, {1, orDummy(slot.sharedS2)},
                {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
            writeBufferSet(slot.dirSets[5], dirSet({
                {0, orDummy(slot.sharedS2)}, {1, orDummy(slot.sharedS1)},
                {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
            writeBufferSet(slot.dirSets[6], dirSet({
                {0, orDummy(slot.filmRaw)}, {1, orDummy(slot.sharedS0)}, {2, orDummy(slot.sharedS0)},
                {3, orDummy(slot.dirDensity)}, {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
            // Halation sets mirror upstream's 13 op wirings (bindings 0-3
            // ping-pong between source/scratch, 27 frame floats). Written
            // for the no-boost source (filmRaw); record() rebinds binding 0
            // of sets 0/2/3/5 (scatter source) and 6/7/9 (bounce source)
            // when boost/scatter run. Boost reduction sets 10/11/12 are
            // fully static.
            auto halationSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
                std::vector<std::pair<uint32_t, VkBuffer>> out;
                bool used[31] = {};
                for (const auto& p : live) {
                    // Bindings 0..3 are image ping-pong (possibly absent
                    // under conditional scratch); the rest are always live.
                    out.emplace_back(p.first,
                                     p.first <= 3 ? orDummy(p.second)
                                                  : p.second);
                    used[p.first] = true;
                }
                for (uint32_t b = 0; b <= 30; ++b) {
                    if (!used[b] && b != 27) {
                        out.emplace_back(b, dummyBuffer);
                    }
                }
                out.emplace_back(27, slot.frameFloats);
                return out;
            };
            // Halation sets: RawA->S0, RawB/D->S1 (D reuses B after resolve),
            // RawC->S2. D==B alias is safe: B dead after ScatterResolve.
            writeBufferSet(slot.halationSets[0], halationSet({
                {0, slot.filmRaw}, {1, slot.sharedS0},
                {2, slot.sharedS0}, {3, slot.sharedS0}}));
            writeBufferSet(slot.halationSets[1], halationSet({
                {0, slot.sharedS0}, {1, slot.sharedS1},
                {2, slot.sharedS0}, {3, slot.sharedS1}}));
            writeBufferSet(slot.halationSets[2], halationSet({
                {0, slot.filmRaw}, {1, slot.sharedS2},
                {2, slot.sharedS0}, {3, slot.sharedS2}}));
            writeBufferSet(slot.halationSets[3], halationSet({
                {0, slot.filmRaw}, {1, slot.sharedS0},
                {2, slot.sharedS0}, {3, slot.sharedS0}}));
            writeBufferSet(slot.halationSets[4], halationSet({
                {0, slot.sharedS0}, {1, slot.sharedS2},
                {2, slot.sharedS0}, {3, slot.sharedS2}}));
            writeBufferSet(slot.halationSets[5], halationSet({
                {0, slot.filmRaw}, {1, slot.sharedS1},
                {2, slot.sharedS2}, {3, slot.sharedS1}}));
            writeBufferSet(slot.halationSets[6], halationSet({
                {0, slot.filmRaw}, {1, slot.sharedS2},
                {2, slot.sharedS0}, {3, slot.sharedS2}}));
            writeBufferSet(slot.halationSets[7], halationSet({
                {0, slot.filmRaw}, {1, slot.sharedS0},
                {2, slot.sharedS0}, {3, slot.sharedS0}}));
            writeBufferSet(slot.halationSets[8], halationSet({
                {0, slot.sharedS0}, {1, slot.sharedS2},
                {2, slot.sharedS0}, {3, slot.sharedS2}}));
            writeBufferSet(slot.halationSets[9], halationSet({
                {0, slot.filmRaw}, {1, slot.sharedS2},
                {2, slot.sharedS0}, {3, slot.halationLogRaw}}));
            writeBufferSet(slot.halationSets[10], halationSet({
                {0, slot.filmRaw}, {1, slot.halationBoostChunks},
                {2, slot.halationBoostInfo}, {3, slot.halationBoostedRaw}}));
            writeBufferSet(slot.halationSets[11], halationSet({
                {0, slot.halationBoostChunks}, {1, slot.halationBoostInfo},
                {2, slot.halationBoostInfo}, {3, slot.halationBoostedRaw}}));
            writeBufferSet(slot.halationSets[12], halationSet({
                {0, slot.filmRaw}, {1, slot.halationBoostedRaw},
                {2, slot.halationBoostInfo}, {3, slot.halationBoostedRaw}}));
            // Diffusion sets mirror upstream's camera wirings (0 source,
            // 1 temp, 2 accum, 3 dest, 4-6 downsample chain, 27 info,
            // 28 components). Set0 binding 0 is the boost-dependent source
            // and is rebound in record(); set1 converts the diffused raw
            // to log when halation is off (upstream diffusion set 1).
            auto diffusionSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
                std::vector<std::pair<uint32_t, VkBuffer>> out;
                bool used[31] = {};
                for (const auto& p : live) {
                    // Bindings 0..6 are image/downsample scratch (possibly
                    // absent); 27/28 host uploads are always live.
                    out.emplace_back(p.first,
                                     p.first <= 6 ? orDummy(p.second)
                                                  : p.second);
                    used[p.first] = true;
                }
                for (uint32_t b = 0; b <= 30; ++b) {
                    if (!used[b] && b != 27 && b != 28) {
                        out.emplace_back(b, dummyBuffer);
                    }
                }
                out.emplace_back(27, slot.diffusionInfo);
                out.emplace_back(28, slot.diffusionComponents);
                return out;
            };
            writeBufferSet(slot.diffusionSets[0], diffusionSet({
                {0, slot.filmRaw}, {1, slot.diffusionTemp},
                {2, slot.diffusionAccum}, {3, slot.cameraDiffusionRaw},
                {4, slot.diffusionDownSource}, {5, slot.diffusionDownTemp},
                {6, slot.diffusionDownBlur}}));
            writeBufferSet(slot.diffusionSets[1], diffusionSet({
                {0, slot.cameraDiffusionRaw}, {1, slot.diffusionTemp},
                {2, slot.diffusionAccum}, {3, slot.halationLogRaw},
                {4, slot.diffusionDownSource}, {5, slot.diffusionDownTemp},
                {6, slot.diffusionDownBlur}}));
            // Print sequence set: source AND dest share cameraDiffusionRaw
            // (never concurrent with camera use; see createSlots).
            {
                std::vector<std::pair<uint32_t, VkBuffer>> bindings = {
                    {0, orDummy(slot.cameraDiffusionRaw)}, {1, orDummy(slot.diffusionTemp)},
                    {2, orDummy(slot.diffusionAccum)}, {3, orDummy(slot.cameraDiffusionRaw)},
                    {4, orDummy(slot.diffusionDownSource)}, {5, orDummy(slot.diffusionDownTemp)},
                    {6, orDummy(slot.diffusionDownBlur)},
                    {27, slot.printDiffusionInfo},
                    {28, slot.printDiffusionComponents}};
                bool used[31] = {};
                for (const auto& p : bindings) {
                    used[p.first] = true;
                }
                for (uint32_t b = 0; b <= 30; ++b) {
                    if (!used[b]) {
                        bindings.emplace_back(b, dummyBuffer);
                    }
                }
                writeBufferSet(slot.diffusionSets[2], bindings);
            }
            // Scanner sets mirror upstream's 9 op wirings (0 source, 1 temp,
            // 2 unsharp, 3 dest, 6 transfer kinds, 25 encode LUTs, 27/28 frame
            // arrays). Set4/6/7/8 source-ish bindings vary per record
            // (glare/blur path) and are rebound in record().
            auto scanSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
                std::vector<std::pair<uint32_t, VkBuffer>> out;
                bool used[31] = {};
                for (const auto& p : live) {
                    // Bindings 0..3 are scanner scratch (possibly absent);
                    // the rest are statics/frame arrays.
                    out.emplace_back(p.first,
                                     p.first <= 3 ? orDummy(p.second)
                                                  : p.second);
                    used[p.first] = true;
                }
                for (uint32_t b = 0; b <= 30; ++b) {
                    if (!used[b]) {
                        out.emplace_back(b, dummyBuffer);
                    }
                }
                return out;
            };
            // Scanner sets: scanA->S0, scanB/glareB->S1, scanC/glareA->S2.
            // Glare temps (S1/S2) are dead after op3; scanC starts op4.
            const VkBuffer transferKinds = statics[kTransferKinds].buffer;
            // kEncodeAndGamut holds the output encode LUTs (binding 25).
            const VkBuffer encodeLuts = statics[kEncodeAndGamut].buffer;
            writeBufferSet(slot.scanSets[0], scanSet({
                {0, slot.sharedS0}, {1, slot.sharedS2}, {2, slot.sharedS0}, {3, slot.sharedS2},
                {6, transferKinds}, {25, encodeLuts},
                {27, slot.frameFloats}, {28, slot.frameInts}}));
            writeBufferSet(slot.scanSets[1], scanSet({
                {0, slot.sharedS2}, {1, slot.sharedS1}, {2, slot.sharedS2}, {3, slot.sharedS1},
                {6, transferKinds}, {25, encodeLuts},
                {27, slot.frameFloats}, {28, slot.frameInts}}));
            writeBufferSet(slot.scanSets[2], scanSet({
                {0, slot.sharedS1}, {1, slot.sharedS2}, {2, slot.sharedS1}, {3, slot.sharedS2},
                {6, transferKinds}, {25, encodeLuts},
                {27, slot.frameFloats}, {28, slot.frameInts}}));
            writeBufferSet(slot.scanSets[3], scanSet({
                {0, slot.sharedS0}, {1, slot.sharedS2}, {2, slot.sharedS0}, {3, slot.sharedS1},
                {6, transferKinds}, {25, encodeLuts},
                {27, slot.frameFloats}, {28, slot.frameInts}}));
            writeBufferSet(slot.scanSets[4], scanSet({
                {0, slot.sharedS0}, {1, slot.sharedS2}, {2, slot.sharedS0}, {3, slot.sharedS2},
                {6, transferKinds}, {25, encodeLuts},
                {27, slot.frameFloats}, {28, slot.frameInts}}));
            writeBufferSet(slot.scanSets[5], scanSet({
                {0, slot.sharedS2}, {1, slot.sharedS0}, {2, slot.sharedS2}, {3, slot.sharedS0},
                {6, transferKinds}, {25, encodeLuts},
                {27, slot.frameFloats}, {28, slot.frameInts}}));
            writeBufferSet(slot.scanSets[6], scanSet({
                {0, slot.sharedS0}, {1, slot.sharedS2}, {2, slot.sharedS0}, {3, slot.sharedS2},
                {6, transferKinds}, {25, encodeLuts},
                {27, slot.frameFloats}, {28, slot.frameInts}}));
            writeBufferSet(slot.scanSets[7], scanSet({
                {0, slot.sharedS2}, {1, slot.sharedS1}, {2, slot.sharedS2}, {3, slot.sharedS1},
                {6, transferKinds}, {25, encodeLuts},
                {27, slot.frameFloats}, {28, slot.frameInts}}));
            writeBufferSet(slot.scanSets[8], scanSet({
                {0, slot.sharedS0}, {1, slot.sharedS2}, {2, slot.sharedS1}, {3, slot.destination},
                {6, transferKinds}, {25, encodeLuts},
                {27, slot.frameFloats}, {28, slot.frameInts}}));
            // PrintScan set layout shared by finalSet (op3 Full path) and
            // printSets (op1/op2 split path): binding 0 density in,
            // binding 1 destination. printSets[0] renders PrintRaw into
            // printDiffusionRaw; printSets[1] renders the final from the
            // diffused raw. Binding 0 of finalSet/printSets[0] and binding
            // 1 of finalSet/printSets[1] vary per record (density routing /
            // scanner defer) and are rebound in record().
            auto printScanBindings = [&](VkBuffer in, VkBuffer out) {
                return std::vector<std::pair<uint32_t, VkBuffer>>{
                    {0, in},
                    {1, out},
                    {2, curve2},
                    {3, curve3},
                    {4, dummyBuffer},
                    {5, statics[kDecodeLuts].buffer},
                    {6, statics[kTransferKinds].buffer},
                    {7, statics[kMallett].buffer},
                    {8, statics[kInputToRefXyz].buffer},
                    {9, statics[kHanatosResponse].buffer},
                    {10, statics[kPackedPaperExposure].buffer},
                    {11, statics[kPaperDensityCurves].buffer},
                    {12, statics[kFilmSpectral].buffer},
                    {13, statics[kFilmBase].buffer},
                    {14, slot.filteredEnlarger},
                    {15, statics[kThKg3].buffer},
                    {16, statics[kCustomFilters].buffer},
                    {17, statics[kNeutralFilters].buffer},
                    {18, statics[kPaperSpectral].buffer},
                    {19, statics[kPaperBase].buffer},
                    {20, statics[kScanProducts].buffer},
                    {21, statics[kPaperSensitivity].buffer},
                    {22, statics[kCmfs].buffer},
                    {23, statics[kFilmScanToOutput].buffer},
                    {24, statics[kPaperScanToOutput].buffer},
                    {25, statics[kEncodeAndGamut].buffer},
                    {26, statics[kAcademy].buffer},
                    {27, slot.frameFloats},
                    {28, slot.frameInts},
                    {29, statics[kPaperHanatos].buffer},
                    {30, statics[kPreflashHanatos].buffer}};
            };
            writeBufferSet(slot.finalSet,
                           printScanBindings(orDummy(slot.filmDensity),
                                             slot.destination));
            writeBufferSet(slot.printSets[0],
                           printScanBindings(orDummy(slot.filmDensity),
                                             orDummy(slot.cameraDiffusionRaw)));
            writeBufferSet(slot.printSets[1],
                           printScanBindings(orDummy(slot.cameraDiffusionRaw),
                                             slot.destination));
            // inputSet/outputSet image bindings are per-record (views vary).
            writeBufferSet(slot.inputSet, {{0, slot.source}});
            writeBufferSet(slot.outputSet, {{0, slot.destination}});
            // Export-set sources are rebound per record when glow is requested;
            // park them on dummy so the set is never uninitialized if used.
            writeBufferSet(slot.glowRatioSet, {{0, dummyBuffer}, {1, dummyBuffer}});
            // Tile-memory bundle (tiledMemorySaving only): same 42-set
            // binding contract as above, but pixel buffers come from the
            // working-size tile arena. Shared host-visible/small buffers
            // (frame arrays, diffusion info, dir tables, enlarger response)
            // alias the slot; exposure binding 0 stays the FULL-frame source
            // (read with _pad2 fullFrameSource). No boost sets content: tiled
            // + boost is rejected, so sets 10-12 stay dummy-bound.
            if (tileMemorySaving) {
                writeTileSets(slot);
            }
        }
    }

    void writeTileSets(Slot& slot) {
        const VkBuffer curve2 = statics[kPackedFilmExposure].buffer;
        const VkBuffer curve3 = statics[kFilmDensityCurves].buffer;
        const auto orDummy = [&](VkBuffer b) -> VkBuffer {
            return b ? b : dummyBuffer;
        };
        std::vector<std::pair<uint32_t, VkBuffer>> exposureExtra;
        for (uint32_t b = 10; b <= 30; ++b) {
            exposureExtra.push_back({b, dummyBuffer});
        }
        std::vector<std::pair<uint32_t, VkBuffer>> developExtra;
        for (uint32_t b = 4; b <= 30; ++b) {
            developExtra.push_back({b, dummyBuffer});
        }
        auto& t = slot.tile;
        std::vector<std::pair<uint32_t, VkBuffer>> exposureBindings = {
            {0, slot.source},
            {1, orDummy(t.filmRaw)},
            {2, curve2},
            {3, curve3},
            {4, statics[kInputToSrgb].buffer},
            {5, statics[kDecodeLuts].buffer},
            {6, statics[kTransferKinds].buffer},
            {7, statics[kMallett].buffer},
            {8, statics[kInputToRefXyz].buffer},
            {9, statics[kHanatosResponse].buffer}};
        exposureBindings.insert(exposureBindings.end(), exposureExtra.begin(),
                                exposureExtra.end());
        writeBufferSet(slot.tileExposureSet, exposureBindings);
        std::vector<std::pair<uint32_t, VkBuffer>> developBindings = {
            {0, orDummy(t.filmRaw)},
            {1, orDummy(t.filmDensity)},
            {2, curve2},
            {3, curve3}};
        developBindings.insert(developBindings.end(), developExtra.begin(),
                               developExtra.end());
        writeBufferSet(slot.tileDevelopSet, developBindings);
        std::vector<std::pair<uint32_t, VkBuffer>> grainBindings = {
            {0, orDummy(t.filmDensity)},
            {1, orDummy(t.grainDensity)},
            {8, curve3},
            {27, slot.frameFloats},
            {28, slot.frameInts}};
        for (uint32_t b : {2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u}) {
            grainBindings.push_back({b, dummyBuffer});
        }
        for (uint32_t b = 11; b <= 26; ++b) {
            grainBindings.push_back({b, dummyBuffer});
        }
        grainBindings.push_back({29, dummyBuffer});
        grainBindings.push_back({30, dummyBuffer});
        writeBufferSet(slot.tileGrainSet, grainBindings);
        auto prodGrainSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
            std::vector<std::pair<uint32_t, VkBuffer>> out(live);
            bool used[31] = {};
            for (const auto& p : live) {
                used[p.first] = true;
            }
            for (uint32_t b = 0; b <= 30; ++b) {
                if (!used[b] && b != 27 && b != 28) {
                    out.emplace_back(b, dummyBuffer);
                }
            }
            out.emplace_back(27, slot.frameFloats);
            out.emplace_back(28, slot.frameInts);
            return out;
        };
        const VkBuffer layerTables = statics[kDensityCurveLayers].buffer;
        const VkBuffer layerMaxima = statics[kDensityCurveLayerMaxima].buffer;
        auto prodGrainCommon = [&](std::vector<std::pair<uint32_t, VkBuffer>> out) {
            out.emplace_back(4, orDummy(t.grainMicroA));
            out.emplace_back(5, orDummy(t.grainMicroB));
            out.emplace_back(6, orDummy(t.grainLayerA));
            out.emplace_back(7, orDummy(t.grainLayerB));
            out.emplace_back(8, curve3);
            out.emplace_back(9, layerTables);
            out.emplace_back(10, layerMaxima);
            return out;
        };
        writeBufferSet(slot.tileGrainProdSets[0], prodGrainCommon(prodGrainSet({
            {0, orDummy(t.filmDensity)}, {1, orDummy(t.grainDensity)},
            {2, orDummy(t.grainDensityB)}, {3, orDummy(t.grainDensityB)}})));
        writeBufferSet(slot.tileGrainProdSets[1], prodGrainCommon(prodGrainSet({
            {0, orDummy(t.grainDensity)}, {1, orDummy(t.grainDensityB)},
            {2, orDummy(t.grainDensityB)}, {3, orDummy(t.grainDensity)}})));
        writeBufferSet(slot.tileGrainProdSets[2], prodGrainCommon(prodGrainSet({
            {0, orDummy(t.grainDensityB)}, {1, orDummy(t.grainDensity)},
            {2, orDummy(t.grainDensityB)}, {3, orDummy(t.grainDensity)}})));
        writeBufferSet(slot.tileGrainProdSets[3], prodGrainCommon(prodGrainSet({
            {0, orDummy(t.filmDensity)}, {1, orDummy(t.grainDensity)},
            {2, orDummy(t.grainDensityB)}, {3, orDummy(t.grainDensityB)}})));
        auto dirSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
            std::vector<std::pair<uint32_t, VkBuffer>> out(live);
            bool used[31] = {};
            for (const auto& p : live) {
                used[p.first] = true;
            }
            for (uint32_t b = 0; b <= 30; ++b) {
                if (!used[b]) {
                    out.emplace_back(b, dummyBuffer);
                }
            }
            return out;
        };
        writeBufferSet(slot.tileDirSets[0], dirSet({
            {0, orDummy(t.filmDensity)}, {1, orDummy(t.dirCorrectionA)}, {2, orDummy(t.dirCorrectionA)},
            {3, orDummy(t.dirCorrectionA)}, {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
        writeBufferSet(slot.tileDirSets[1], dirSet({
            {0, orDummy(t.dirCorrectionA)}, {1, orDummy(t.dirCorrectionB)},
            {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
        writeBufferSet(slot.tileDirSets[2], dirSet({
            {0, orDummy(t.dirCorrectionB)}, {1, orDummy(t.dirCorrectionC)},
            {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
        writeBufferSet(slot.tileDirSets[3], dirSet({
            {0, orDummy(t.dirCorrectionA)}, {1, orDummy(t.dirCorrectionB)},
            {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
        writeBufferSet(slot.tileDirSets[4], dirSet({
            {0, orDummy(t.dirCorrectionA)}, {1, orDummy(t.dirCorrectionC)},
            {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
        writeBufferSet(slot.tileDirSets[5], dirSet({
            {0, orDummy(t.dirCorrectionC)}, {1, orDummy(t.dirCorrectionB)},
            {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
        writeBufferSet(slot.tileDirSets[6], dirSet({
            {0, orDummy(t.filmRaw)}, {1, orDummy(t.dirCorrectionA)}, {2, orDummy(t.dirCorrectionA)},
            {3, orDummy(t.dirDensity)}, {4, curve2}, {5, slot.dirCurves}, {27, slot.dirFloats}}));
        auto halationSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
            std::vector<std::pair<uint32_t, VkBuffer>> out;
            bool used[31] = {};
            for (const auto& p : live) {
                out.emplace_back(p.first,
                                 p.first <= 3 ? orDummy(p.second)
                                              : p.second);
                used[p.first] = true;
            }
            for (uint32_t b = 0; b <= 30; ++b) {
                if (!used[b] && b != 27) {
                    out.emplace_back(b, dummyBuffer);
                }
            }
            out.emplace_back(27, slot.frameFloats);
            return out;
        };
        writeBufferSet(slot.tileHalationSets[0], halationSet({
            {0, t.filmRaw}, {1, t.halationRawA},
            {2, t.halationRawA}, {3, t.halationRawA}}));
        writeBufferSet(slot.tileHalationSets[1], halationSet({
            {0, t.halationRawA}, {1, t.halationRawB},
            {2, t.halationRawA}, {3, t.halationRawB}}));
        writeBufferSet(slot.tileHalationSets[2], halationSet({
            {0, t.filmRaw}, {1, t.halationRawC},
            {2, t.halationRawA}, {3, t.halationRawC}}));
        writeBufferSet(slot.tileHalationSets[3], halationSet({
            {0, t.filmRaw}, {1, t.halationRawA},
            {2, t.halationRawA}, {3, t.halationRawA}}));
        writeBufferSet(slot.tileHalationSets[4], halationSet({
            {0, t.halationRawA}, {1, t.halationRawC},
            {2, t.halationRawA}, {3, t.halationRawC}}));
        writeBufferSet(slot.tileHalationSets[5], halationSet({
            {0, t.filmRaw}, {1, t.halationRawB},
            {2, t.halationRawC}, {3, t.halationRawD}}));
        writeBufferSet(slot.tileHalationSets[6], halationSet({
            {0, t.filmRaw}, {1, t.halationRawC},
            {2, t.halationRawA}, {3, t.halationRawC}}));
        writeBufferSet(slot.tileHalationSets[7], halationSet({
            {0, t.filmRaw}, {1, t.halationRawA},
            {2, t.halationRawA}, {3, t.halationRawA}}));
        writeBufferSet(slot.tileHalationSets[8], halationSet({
            {0, t.halationRawA}, {1, t.halationRawC},
            {2, t.halationRawA}, {3, t.halationRawC}}));
        writeBufferSet(slot.tileHalationSets[9], halationSet({
            {0, t.filmRaw}, {1, t.halationRawC},
            {2, t.halationRawA}, {3, t.halationLogRaw}}));
        for (int s = 10; s < 12; ++s) {
            writeBufferSet(slot.tileHalationSets[s], halationSet({
                {0, VK_NULL_HANDLE}, {1, VK_NULL_HANDLE},
                {2, VK_NULL_HANDLE}, {3, VK_NULL_HANDLE}}));
        }
        // BoostApply (op9): filmRaw -> boostedRaw via the milestone info in
        // the host-visible readback (phase 2 uploads it before dispatch).
        writeBufferSet(slot.tileHalationSets[12], halationSet({
            {0, t.filmRaw}, {1, t.boostedRaw},
            {2, t.boostReadback}, {3, t.boostedRaw}}));
        // Milestone chunk-max (adapter shader): tile filmRaw -> full chunks.
        {
            std::vector<std::pair<uint32_t, VkBuffer>> bindings = {
                {0, orDummy(t.filmRaw)}, {1, orDummy(t.chunks)}};
            for (uint32_t b = 2; b <= 30; ++b) {
                bindings.emplace_back(b, dummyBuffer);
            }
            writeBufferSet(slot.tileBoostMaxSet, bindings);
        }
        // Milestone reduce (Halation op8): full chunks -> 16B readback.
        {
            std::vector<std::pair<uint32_t, VkBuffer>> bindings = {
                {0, orDummy(t.chunks)}, {1, orDummy(t.boostReadback)},
                {2, orDummy(t.boostReadback)}, {3, dummyBuffer}};
            for (uint32_t b = 4; b <= 30; ++b) {
                if (b != 27) {
                    bindings.emplace_back(b, dummyBuffer);
                }
            }
            bindings.emplace_back(27, slot.frameFloats);
            writeBufferSet(slot.tileBoostReduceSet, bindings);
        }
        auto diffusionSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
            std::vector<std::pair<uint32_t, VkBuffer>> out;
            bool used[31] = {};
            for (const auto& p : live) {
                out.emplace_back(p.first,
                                 p.first <= 6 ? orDummy(p.second)
                                              : p.second);
                used[p.first] = true;
            }
            for (uint32_t b = 0; b <= 30; ++b) {
                if (!used[b] && b != 27 && b != 28) {
                    out.emplace_back(b, dummyBuffer);
                }
            }
            out.emplace_back(27, slot.diffusionInfo);
            out.emplace_back(28, slot.diffusionComponents);
            return out;
        };
        writeBufferSet(slot.tileDiffusionSets[0], diffusionSet({
            {0, t.filmRaw}, {1, t.diffusionTemp},
            {2, t.diffusionAccum}, {3, t.cameraDiffusionRaw},
            {4, t.diffusionDownSource}, {5, t.diffusionDownTemp},
            {6, t.diffusionDownBlur}}));
        writeBufferSet(slot.tileDiffusionSets[1], diffusionSet({
            {0, t.cameraDiffusionRaw}, {1, t.diffusionTemp},
            {2, t.diffusionAccum}, {3, t.halationLogRaw},
            {4, t.diffusionDownSource}, {5, t.diffusionDownTemp},
            {6, t.diffusionDownBlur}}));
        {
            std::vector<std::pair<uint32_t, VkBuffer>> bindings = {
                {0, orDummy(t.printDiffusionRaw)}, {1, orDummy(t.diffusionTemp)},
                {2, orDummy(t.diffusionAccum)}, {3, orDummy(t.printDiffusionRaw)},
                {4, orDummy(t.diffusionDownSource)}, {5, orDummy(t.diffusionDownTemp)},
                {6, orDummy(t.diffusionDownBlur)},
                {27, slot.printDiffusionInfo},
                {28, slot.printDiffusionComponents}};
            bool used[31] = {};
            for (const auto& p : bindings) {
                used[p.first] = true;
            }
            for (uint32_t b = 0; b <= 30; ++b) {
                if (!used[b]) {
                    bindings.emplace_back(b, dummyBuffer);
                }
            }
            writeBufferSet(slot.tileDiffusionSets[2], bindings);
        }
        auto scanSet = [&](std::initializer_list<std::pair<uint32_t, VkBuffer>> live) {
            std::vector<std::pair<uint32_t, VkBuffer>> out;
            bool used[31] = {};
            for (const auto& p : live) {
                out.emplace_back(p.first,
                                 p.first <= 3 ? orDummy(p.second)
                                              : p.second);
                used[p.first] = true;
            }
            for (uint32_t b = 0; b <= 30; ++b) {
                if (!used[b]) {
                    out.emplace_back(b, dummyBuffer);
                }
            }
            return out;
        };
        const VkBuffer transferKinds = statics[kTransferKinds].buffer;
        const VkBuffer encodeLuts = statics[kEncodeAndGamut].buffer;
        writeBufferSet(slot.tileScanSets[0], scanSet({
            {0, t.scanA}, {1, t.glareA}, {2, t.scanA}, {3, t.glareA},
            {6, transferKinds}, {25, encodeLuts},
            {27, slot.frameFloats}, {28, slot.frameInts}}));
        writeBufferSet(slot.tileScanSets[1], scanSet({
            {0, t.glareA}, {1, t.glareB}, {2, t.glareA}, {3, t.glareB},
            {6, transferKinds}, {25, encodeLuts},
            {27, slot.frameFloats}, {28, slot.frameInts}}));
        writeBufferSet(slot.tileScanSets[2], scanSet({
            {0, t.glareB}, {1, t.glareA}, {2, t.glareB}, {3, t.glareA},
            {6, transferKinds}, {25, encodeLuts},
            {27, slot.frameFloats}, {28, slot.frameInts}}));
        writeBufferSet(slot.tileScanSets[3], scanSet({
            {0, t.scanA}, {1, t.glareA}, {2, t.scanA}, {3, t.scanB},
            {6, transferKinds}, {25, encodeLuts},
            {27, slot.frameFloats}, {28, slot.frameInts}}));
        writeBufferSet(slot.tileScanSets[4], scanSet({
            {0, t.scanA}, {1, t.scanC}, {2, t.scanA}, {3, t.scanC},
            {6, transferKinds}, {25, encodeLuts},
            {27, slot.frameFloats}, {28, slot.frameInts}}));
        writeBufferSet(slot.tileScanSets[5], scanSet({
            {0, t.scanC}, {1, t.scanA}, {2, t.scanC}, {3, t.scanA},
            {6, transferKinds}, {25, encodeLuts},
            {27, slot.frameFloats}, {28, slot.frameInts}}));
        writeBufferSet(slot.tileScanSets[6], scanSet({
            {0, t.scanA}, {1, t.scanC}, {2, t.scanA}, {3, t.scanC},
            {6, transferKinds}, {25, encodeLuts},
            {27, slot.frameFloats}, {28, slot.frameInts}}));
        writeBufferSet(slot.tileScanSets[7], scanSet({
            {0, t.scanC}, {1, t.scanB}, {2, t.scanC}, {3, t.scanB},
            {6, transferKinds}, {25, encodeLuts},
            {27, slot.frameFloats}, {28, slot.frameInts}}));
        writeBufferSet(slot.tileScanSets[8], scanSet({
            {0, t.scanA}, {1, t.scanC}, {2, t.scanB}, {3, t.dest},
            {6, transferKinds}, {25, encodeLuts},
            {27, slot.frameFloats}, {28, slot.frameInts}}));
        auto printScanBindings = [&](VkBuffer in, VkBuffer out) {
            return std::vector<std::pair<uint32_t, VkBuffer>>{
                {0, in},
                {1, out},
                {2, curve2},
                {3, curve3},
                {4, dummyBuffer},
                {5, statics[kDecodeLuts].buffer},
                {6, statics[kTransferKinds].buffer},
                {7, statics[kMallett].buffer},
                {8, statics[kInputToRefXyz].buffer},
                {9, statics[kHanatosResponse].buffer},
                {10, statics[kPackedPaperExposure].buffer},
                {11, statics[kPaperDensityCurves].buffer},
                {12, statics[kFilmSpectral].buffer},
                {13, statics[kFilmBase].buffer},
                {14, slot.filteredEnlarger},
                {15, statics[kThKg3].buffer},
                {16, statics[kCustomFilters].buffer},
                {17, statics[kNeutralFilters].buffer},
                {18, statics[kPaperSpectral].buffer},
                {19, statics[kPaperBase].buffer},
                {20, statics[kScanProducts].buffer},
                {21, statics[kPaperSensitivity].buffer},
                {22, statics[kCmfs].buffer},
                {23, statics[kFilmScanToOutput].buffer},
                {24, statics[kPaperScanToOutput].buffer},
                {25, statics[kEncodeAndGamut].buffer},
                {26, statics[kAcademy].buffer},
                {27, slot.frameFloats},
                {28, slot.frameInts},
                {29, statics[kPaperHanatos].buffer},
                {30, statics[kPreflashHanatos].buffer}};
        };
        writeBufferSet(slot.tileFinalSet,
                       printScanBindings(orDummy(t.filmDensity),
                                         orDummy(t.dest)));
        writeBufferSet(slot.tilePrintSets[0],
                       printScanBindings(orDummy(t.filmDensity),
                                         orDummy(t.printDiffusionRaw)));
        writeBufferSet(slot.tilePrintSets[1],
                       printScanBindings(orDummy(t.printDiffusionRaw),
                                         orDummy(t.dest)));
    }

    void destroy() noexcept {
        if (!context.device) {
            return;
        }
        // No device-wide wait: all film work is fence-scoped by the caller
        // (record-only contract — no film work may be in flight), matching
        // TonemapEngine/FCC teardown. A device drain here stalls preview and
        // stills sharing the device for seconds (GBs of full-res scratch).
        std::unordered_set<VkBuffer> destroyedBuffers;
        for (auto& slot : slots) {
            for (VkBuffer b : {slot.source, slot.filmRaw, slot.filmDensity,
                               slot.grainDensity, slot.grainDensityB,
                               slot.grainMicroA, slot.grainMicroB,
                               slot.grainLayerA, slot.grainLayerB, slot.dirCorrectionA,
                               slot.dirCorrectionB, slot.dirCorrectionC,
                               slot.dirDensity, slot.halationRawA,
                               slot.halationRawB, slot.halationRawC,
                               slot.halationRawD, slot.halationLogRaw,
                               slot.halationBoostedRaw, slot.halationBoostChunks,
                               slot.halationBoostInfo, slot.diffusionTemp,
                               slot.diffusionAccum, slot.diffusionDownSource,
                               slot.diffusionDownTemp, slot.diffusionDownBlur,
                               slot.cameraDiffusionRaw, slot.diffusionInfo,
                                slot.diffusionComponents, slot.printDiffusionRaw,
                                slot.printDiffusionInfo,
                                slot.printDiffusionComponents, slot.dirFloats,
                                slot.dirCurves, slot.scanA, slot.scanB,
                                slot.scanC, slot.glareA, slot.glareB,
                                slot.sharedS0, slot.sharedS1, slot.sharedS2,
                                slot.destination, slot.filteredEnlarger,
                                slot.frameFloats, slot.frameInts,
                                slot.tile.filmRaw, slot.tile.filmDensity,
                                slot.tile.grainDensity, slot.tile.grainDensityB,
                                slot.tile.grainMicroA, slot.tile.grainMicroB,
                                slot.tile.grainLayerA, slot.tile.grainLayerB,
                                slot.tile.dirCorrectionA, slot.tile.dirCorrectionB,
                                slot.tile.dirCorrectionC, slot.tile.dirDensity,
                                slot.tile.halationRawA, slot.tile.halationRawB,
                                slot.tile.halationRawC, slot.tile.halationRawD,
                                slot.tile.halationLogRaw, slot.tile.boostedRaw,
                                slot.tile.chunks, slot.tile.boostReadback,
                                slot.tile.diffusionTemp,
                                slot.tile.diffusionAccum, slot.tile.diffusionDownSource,
                                slot.tile.diffusionDownTemp, slot.tile.diffusionDownBlur,
                                slot.tile.cameraDiffusionRaw, slot.tile.printDiffusionRaw,
                                slot.tile.scanA, slot.tile.scanB, slot.tile.scanC,
                                slot.tile.glareA, slot.tile.glareB,
                                slot.tile.dest}) {
                if (b && destroyedBuffers.insert(b).second) {
                    vkDestroyBuffer(context.device, b, context.allocator);
                }
            }
        }
        for (const auto& entry : statics) {
            if (entry.buffer) {
                vkDestroyBuffer(context.device, entry.buffer, context.allocator);
            }
        }
        if (dummyBuffer) {
            vkDestroyBuffer(context.device, dummyBuffer, context.allocator);
            dummyBuffer = VK_NULL_HANDLE;
        }
        // All buffer memory (static, slot, staging) is tracked in ownedMemory.
        for (VkDeviceMemory m : ownedMemory) {
            if (m) {
                vkFreeMemory(context.device, m, context.allocator);
            }
        }
        for (VkPipeline p :
             {exposurePipeline, developPipeline, printScanPipeline, grainPipeline,
              dirPipeline, halationPipeline, diffusionPipeline, scannerPipeline,
              inputPipeline, outputPipeline, outputHalfPipeline,
              milestonePipeline, glowRatioPipeline}) {
            if (p) {
                vkDestroyPipeline(context.device, p, context.allocator);
            }
        }
        for (VkDescriptorSetLayout l : {coreLayout, ioLayout, glowRatioLayout}) {
            if (l) {
                vkDestroyDescriptorSetLayout(context.device, l, context.allocator);
            }
        }
        for (VkPipelineLayout l : {corePipelineLayout, ioPipelineLayout, glowRatioPipelineLayout}) {
            if (l) {
                vkDestroyPipelineLayout(context.device, l, context.allocator);
            }
        }
        if (descriptorPool) {
            vkDestroyDescriptorPool(context.device, descriptorPool,
                                    context.allocator);
        }
        slots.clear();
        context.device = VK_NULL_HANDLE;
    }

    void recordTiledMemory(const SpektraFilmRecordInfo& ri, Slot& slot,
                           uint32_t w, uint32_t h, float pixelSizeUm,
                           uint32_t adaptFlags);
    void recordBoostMilestone(const SpektraFilmRecordInfo& ri);
};

SpektraFilmBoostMilestone SpektraFilm::computeBoostInfo(
    float globalMaxRaw, float protectEv, float range,
    float boostEv) noexcept {
    SpektraFilmBoostMilestone out{};
    const float maxRaw = std::max(globalMaxRaw, 0.0f);
    const float rawX0 =
        std::clamp(0.184f * std::exp2(protectEv), 0.0f, maxRaw);
    const float boostRange = std::clamp(range, 0.0f, 1.0f);
    const float a = std::pow(28.0f, 1.0f - boostRange);
    const float x0 = maxRaw > 0.0f ? rawX0 / maxRaw : 1.0f;
    const float denom =
        std::exp(a * (1.0f - x0)) - a * (1.0f - x0) - 1.0f;
    const float k =
        (maxRaw > 0.0f && rawX0 < maxRaw && denom > 1.0e-10f)
            ? (std::exp2(std::max(boostEv, 0.0f)) - 1.0f) / denom
            : 0.0f;
    out.values[0] = maxRaw;
    out.values[1] = rawX0;
    out.values[2] = a;
    out.values[3] = k;
    return out;
}

void SpektraFilm::recordBoostMilestone(const SpektraFilmRecordInfo& ri) {
    Impl& impl = *impl_;
    const char* reason = nullptr;
    if (!validateRecordInfo(ri, impl.slotCount, impl.maxWidth, impl.maxHeight,
                            &reason)) {
        throw std::invalid_argument(reason ? reason : "invalid record info");
    }
    if (!impl.tileMemorySaving) {
        throw std::invalid_argument(
            "spektrafilm: boost milestone needs a tile-memory engine");
    }
    if (ri.tilingMode != GpuRenderTilingMode::Tiled) {
        throw std::invalid_argument(
            "spektrafilm: boost milestone needs Tiled records");
    }
    if (!validLookForRecord(ri.look, impl.bakedLook, impl.bakedCamera,
                            &reason)) {
        throw std::invalid_argument(reason ? reason : "invalid look");
    }
    if (!ri.look.halationEnabled || !(ri.look.halationBoostEv > 0.0f)) {
        throw std::invalid_argument(
            "spektrafilm: boost milestone needs halation boost enabled");
    }
    if (!impl.milestonePipeline) {
        throw std::invalid_argument(
            "spektrafilm: boost milestone needs boostMilestoneSpirv at create");
    }
    impl.recordBoostMilestone(ri);
}

SpektraFilmBoostMilestone SpektraFilm::readBoostMilestone(
    uint32_t frameSlot) const {
    const Impl& impl = *impl_;
    if (frameSlot >= impl.slotCount) {
        throw std::out_of_range("spektrafilm: frameSlot out of range");
    }
    const void* mapped = impl.slots[frameSlot].tile.mappedBoostReadback;
    if (!mapped) {
        throw std::logic_error(
            "spektrafilm: no boost readback (tile-memory + halation engine?)");
    }
    SpektraFilmBoostMilestone out{};
    std::memcpy(out.values, mapped, sizeof(out.values));
    return out;
}

// Boost milestone phase 1 (tile-memory + boost only): input copy,
// linear-raw exposure per tile, per-tile atomic chunk maxima over the full
// chunk buffer, then BoostReduceMax straight into the host-visible readback.
// The caller submits, waits, maps via readBoostMilestone(), and feeds phase 2.
void SpektraFilm::Impl::recordBoostMilestone(const SpektraFilmRecordInfo& ri) {
    Slot& slot = slots[ri.frameSlot];
    const uint32_t w = ri.input.width;
    const uint32_t h = ri.input.height;
    VkCommandBuffer cmd = ri.commandBuffer;
    VkDevice device = context.device;
    auto& t = slot.tile;
    const uint32_t adaptFlags = tables::colorAdaptationFlags(ri.look);

    float frameFloats[105];
    uint32_t frameInts[29];
    const float enlarger =
        std::clamp(ri.look.enlargerScale, 1.0f, 32.0f);
    const float longEdgeMm = tables::filmFormatLongEdgeMm(ri.look.filmFormat);
    const float pixelSizeUm =
        longEdgeMm * 1000.0f / static_cast<float>(std::max(w, h)) / enlarger;
    (void)pixelSizeUm;
    tables::fillFrameArrays(*filmCurves, tables, ri.look, bakedLook.film,
                            bakedLook.paper, pixelSizeUm, longEdgeMm,
                            ri.timeSec, frameFloats, frameInts);
    frameInts[1] = static_cast<uint32_t>(ri.look.outputColorSpace);
    frameInts[2] = static_cast<uint32_t>(std::clamp(ri.look.outputRole, 0, 2));
    std::memcpy(slot.mappedFloats, frameFloats, sizeof(frameFloats));
    std::memcpy(slot.mappedInts, frameInts, sizeof(frameInts));
    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &hostBarrier, 0, nullptr, 0, nullptr);

    CorePush push{};
    push.filmExposureEv = ri.look.filmExposureEv;
    std::copy(ri.adaptiveTone.begin(), ri.adaptiveTone.end(), push.adaptiveTone);
    push.filmGamma = tables::effectiveFilmGamma(ri.look.filmPushPullMode,
                                                ri.look.filmGamma,
                                                ri.look.filmPushPullStops);
    push.filmPushPullMode = ri.look.filmPushPullMode;
    push.filmPushPullStops = ri.look.filmPushPullStops;
    push.exposureCount = tables.exposureCount;
    push.inputColorSpace = ri.look.inputColorSpace;
    push.rgbToRawMethod = ri.look.rgbToRawMethod;
    push.colorDecodeMin = spektrafilm::colorDecodeLutMin();
    push.colorDecodeMax = spektrafilm::colorDecodeLutMax();
    const auto& hanatos = spektrafilm::hanatosSpectraLutInfo();
    push.hanatosWidth = hanatos.width;
    push.hanatosHeight = hanatos.height;
    push.fullWidth = w;
    push.fullHeight = h;
    const uint32_t tileW = std::min(std::max(ri.tileWidth, 64u), w);
    const uint32_t tileH = std::min(std::max(ri.tileHeight, 64u), h);

    const auto computeBarrier = [&]() {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
    };
    uint32_t ioPush[16] = {w, h, 0u, 0u};
    std::memcpy(&ioPush[4], &ri.sensorToLinearSrgb[0], 3 * sizeof(float));
    std::memcpy(&ioPush[8], &ri.sensorToLinearSrgb[3], 3 * sizeof(float));
    std::memcpy(&ioPush[12], &ri.sensorToLinearSrgb[6], 3 * sizeof(float));
    VkDescriptorImageInfo inputImage{};
    inputImage.imageView = ri.input.view;
    inputImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet imageWrite{};
    imageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    imageWrite.dstBinding = 1;
    imageWrite.descriptorCount = 1;
    imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    imageWrite.dstSet = slot.inputSet;
    imageWrite.pImageInfo = &inputImage;
    vkUpdateDescriptorSets(device, 1, &imageWrite, 0, nullptr);
    VkMemoryBarrier inputReady{};
    inputReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    inputReady.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    inputReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &inputReady, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inputPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            ioPipelineLayout, 0, 1, &slot.inputSet, 0,
                            nullptr);
    vkCmdPushConstants(cmd, ioPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(ioPush), ioPush);
    vkCmdDispatch(cmd, (w + 31u) / 32u, (h + 7u) / 8u, 1);
    computeBarrier();

    // Zero the full chunk buffer, then per tile: linear exposure into tile
    // filmRaw + atomic chunk maxima with global chunk mapping.
    VkMemoryBarrier fillBarrier{};
    fillBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fillBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT;
    const VkDeviceSize chunkBytes =
        ((static_cast<VkDeviceSize>(w) * h + 255u) / 256u) * 16u;
    vkCmdFillBuffer(cmd, t.chunks, 0, chunkBytes, 0u);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &fillBarrier, 0, nullptr, 0, nullptr);
    for (uint32_t cy = 0; cy < h; cy += tileH) {
        for (uint32_t cx = 0; cx < w; cx += tileW) {
            const uint32_t ww = std::min(tileW, w - cx);
            const uint32_t wh = std::min(tileH, h - cy);
            // Linear-raw exposure straight into the tile buffer.
            push.op = 1u;
            push.pad1 = adaptFlags;
            push.pad2 = 1u;
            push.width = ww;
            push.height = wh;
            push.tileOriginX = cx;
            push.tileOriginY = cy;
            push.activeOriginX = 0u;
            push.activeOriginY = 0u;
            push.activeWidth = ww;
            push.activeHeight = wh;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              exposurePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    corePipelineLayout, 0, 1,
                                    &slot.tileExposureSet, 0, nullptr);
            vkCmdPushConstants(cmd, corePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd, (ww + 31u) / 32u, (wh + 7u) / 8u, 1);
            computeBarrier();
            // Atomic partial maxima (tile-local pixels, global chunks).
            push.op = 0u;
            push.pad1 = 0u;
            push.pad2 = 0u;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              milestonePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    corePipelineLayout, 0, 1,
                                    &slot.tileBoostMaxSet, 0, nullptr);
            vkCmdPushConstants(cmd, corePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd,
                          (static_cast<uint32_t>(
                               (static_cast<uint64_t>(ww) * wh + 255u) / 256u) +
                           0u),
                          1u, 1u);
            computeBarrier();
        }
    }
    // Reduce over the FULL chunk buffer into the host-visible readback.
    const uint32_t chunkCount =
        static_cast<uint32_t>((static_cast<uint64_t>(w) * h + 255u) / 256u);
    push.op = 8u;
    push.pad1 = chunkCount;
    push.pad2 = 0u;
    push.width = w;
    push.height = h;
    push.tileOriginX = 0u;
    push.tileOriginY = 0u;
    push.activeOriginX = 0u;
    push.activeOriginY = 0u;
    push.activeWidth = w;
    push.activeHeight = h;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, halationPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            corePipelineLayout, 0, 1,
                            &slot.tileBoostReduceSet, 0, nullptr);
    vkCmdPushConstants(cmd, corePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, 1u, 1u, 1u);
    computeBarrier();
}

// Tile-memory record (tiledMemorySaving engines, Tiled mode): the chain runs
// per image-tile into working-size tile scratch (working = center inflated by
// the overlap estimator, clamped), centers accumulate to the full-frame
// destination. All passes cover the full working rect; halo error from
// edge-clamped sampling stays inside the overlap (overlap = summed radii),
// so copied centers are exact. First pass reads the full-frame source via
// _pad2 (no staging copy); processNegative stages working rows (no exposure
// on that path). Downsample passes use alignedReducedDimension geometry like
// upstream. Tiled + boost/synthesis are rejected in validateRecordInfo.
void SpektraFilm::Impl::recordTiledMemory(const SpektraFilmRecordInfo& ri,
                                          Slot& slot, uint32_t w, uint32_t h,
                                          float pixelSizeUm,
                                          uint32_t adaptFlags) {
    VkCommandBuffer cmd = ri.commandBuffer;
    VkDevice device = context.device;
    auto& t = slot.tile;
    const bool processNegative = ri.look.process == 2;
    const bool rcmOutput = ri.look.outputRole == 2;
    const uint32_t ov = estimateTileOverlapForLook(ri.look);
    if (ov > tileArenaOv) {
        throw std::invalid_argument(
            "spektrafilm: tile overlap exceeds arena; recreate with effects "
            "matching the baked look");
    }
    const uint32_t tileW = std::min(std::max(ri.tileWidth, 64u), w);
    const uint32_t tileH = std::min(std::max(ri.tileHeight, 64u), h);
    if (tileW > maxTileW || tileH > maxTileH) {
        throw std::invalid_argument(
            "spektrafilm: tile exceeds maxTile; recreate with larger maxTile");
    }
    const auto needTile = [&](bool want, VkBuffer have, const char* what) {
        if (want && !have) {
            throw std::invalid_argument(
                std::string("spektrafilm: tile scratch missing for ") + what +
                "; recreate with the effect enabled");
        }
    };
    const bool halationFeature =
        !processNegative && ri.look.halationEnabled && t.halationRawA &&
        t.halationLogRaw;
    needTile(!processNegative && ri.look.halationEnabled,
             t.halationRawA, "halation");
    needTile(!processNegative &&
                 (ri.look.halationEnabled || ri.look.cameraDiffusionEnabled),
             t.halationLogRaw, "halation/lograw");
    const bool halationScatter =
        halationFeature && ri.look.scatterAmount > 0.0f &&
        ri.look.scatterScale > 0.0f;
    const bool halationBounce =
        halationFeature && ri.look.halationAmount > 0.0f &&
        ri.look.halationScale > 0.0f &&
        (ri.look.halationStrengthR > 0.0f ||
         ri.look.halationStrengthG > 0.0f ||
         ri.look.halationStrengthB > 0.0f);
    const bool halationBoost =
        halationFeature && ri.look.halationBoostEv > 0.0f;
    if (halationBoost && !ri.hasBoostInfo) {
        throw std::invalid_argument(
            "spektrafilm: tiled + boost needs hasBoostInfo from "
            "recordBoostMilestone (submit + wait first)");
    }
    needTile(halationBoost, t.boostedRaw, "halation boost");
    const bool halationPass =
        halationBoost || halationScatter || halationBounce;
    if (halationBoost) {
        std::memcpy(t.mappedBoostReadback, ri.boostInfo,
                    sizeof(ri.boostInfo));
    }
    const diffusion::Solve camSolve =
        diffusion::solveCamera(ri.look, pixelSizeUm);
    const bool cameraDiffusionPath =
        !processNegative &&
        camSolve.info.componentCount > 0u &&
        !camSolve.components.empty() && t.diffusionTemp &&
        t.cameraDiffusionRaw;
    needTile(!processNegative && ri.look.cameraDiffusionEnabled,
             t.cameraDiffusionRaw, "camera diffusion");
    if (cameraDiffusionPath) {
        std::memcpy(slot.mappedDiffusionInfo, &camSolve.info,
                    sizeof(diffusion::Info));
        std::memcpy(slot.mappedDiffusionComponents,
                    camSolve.components.data(),
                    camSolve.components.size() * sizeof(diffusion::Component));
    }
    const diffusion::Solve printSolve =
        diffusion::solvePrint(ri.look, pixelSizeUm);
    const bool printDiffusionPath =
        (ri.look.process == 0 || ri.look.process == 2) &&
        printSolve.info.componentCount > 0u &&
        !printSolve.components.empty() && t.printDiffusionRaw;
    needTile(ri.look.printDiffusionEnabled, t.printDiffusionRaw,
             "print diffusion");
    if (printDiffusionPath) {
        std::memcpy(slot.mappedPrintDiffusionInfo, &printSolve.info,
                    sizeof(diffusion::Info));
        std::memcpy(slot.mappedPrintDiffusionComponents,
                    printSolve.components.data(),
                    printSolve.components.size() * sizeof(diffusion::Component));
    }
    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &hostBarrier, 0, nullptr, 0, nullptr);

    const bool dirPath = !processNegative && ri.look.dirCouplersAmount > 0.0f;
    needTile(dirPath, t.dirDensity, "DIR");
    const bool dirBlurPath =
        dirPath && ri.look.dirCouplersDiffusionUm > 0.0f;
    const bool dirTailPath =
        dirBlurPath && ri.look.dirCouplersDiffusionTailUm > 0.0f &&
        ri.look.dirCouplersDiffusionTailWeight > 0.0f;
    VkBuffer tileDirDensitySrc = t.filmDensity;
    if (dirPath) {
        fillDirFloats(*filmCurves, ri.look, pixelSizeUm,
                      slot.mappedDirFloats);
        fillDirCorrectedCurves(*filmCurves, slot.mappedDirFloats,
                               slot.mappedDirCurves);
        tileDirDensitySrc = t.dirDensity;
    }
    const bool previewGrain = !processNegative && ri.look.grainEnabled &&
                              ri.look.grainModel == 0 && t.grainDensity;
    const bool productionGrain = !processNegative && ri.look.grainEnabled &&
                                 ri.look.grainModel == 1 && t.grainLayerA &&
                                 t.grainDensityB;
    needTile(!processNegative && ri.look.grainEnabled,
             ri.look.grainModel == 0 ? t.grainDensity : t.grainDensityB,
             "grain");
    const bool printLikeFinal = ri.look.process == 0 || ri.look.process == 2;
    const bool scanGlarePath = !rcmOutput && ri.look.scannerEnabled &&
                               printLikeFinal && ri.look.glarePercent > 0.0f;
    const bool scanBlurPath = !rcmOutput && ri.look.scannerEnabled &&
                              ri.look.scannerMtf50LpMm > 0.0f;
    const bool scanUnsharpPath = !rcmOutput && ri.look.scannerEnabled &&
                                 ri.look.scannerUnsharpRadiusUm > 0.0f &&
                                 ri.look.scannerUnsharpAmount > 0.0f;
    const bool scannerPath = t.scanA &&
                             (scanGlarePath || scanBlurPath ||
                              scanUnsharpPath);
    needTile(ri.look.scannerEnabled, t.scanA, "scanner");

    CorePush push{};
    push.filmExposureEv = ri.look.filmExposureEv;
    std::copy(ri.adaptiveTone.begin(), ri.adaptiveTone.end(), push.adaptiveTone);
    push.filmGamma = tables::effectiveFilmGamma(ri.look.filmPushPullMode,
                                                ri.look.filmGamma,
                                                ri.look.filmPushPullStops);
    push.filmPushPullMode = ri.look.filmPushPullMode;
    push.filmPushPullStops = ri.look.filmPushPullStops;
    push.exposureCount = tables.exposureCount;
    push.inputColorSpace = ri.look.inputColorSpace;
    push.rgbToRawMethod = ri.look.rgbToRawMethod;
    push.colorDecodeMin = spektrafilm::colorDecodeLutMin();
    push.colorDecodeMax = spektrafilm::colorDecodeLutMax();
    const auto& hanatos = spektrafilm::hanatosSpectraLutInfo();
    push.hanatosWidth = hanatos.width;
    push.hanatosHeight = hanatos.height;
    push.fullWidth = w;
    push.fullHeight = h;

    const auto computeBarrier = [&]() {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
    };
    const auto rebindBuffer = [&](VkDescriptorSet set, uint32_t binding,
                                  VkBuffer buffer) {
        VkDescriptorBufferInfo info{};
        info.buffer = buffer;
        info.offset = 0;
        info.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    };
    // Working-rect dispatch (32x8 groups; print workgroup sweep is a
    // debug-only tuning and intentionally unsupported here — group size never
    // affects results).
    const auto tCore = [&](VkPipeline pipeline, VkDescriptorSet set,
                           uint32_t op, uint32_t pad1, uint32_t pad2,
                           uint32_t strideW, uint32_t strideH, uint32_t ox,
                           uint32_t oy, uint32_t ax, uint32_t ay, uint32_t aw,
                           uint32_t ah, uint32_t gz = 1) {
        push.op = op;
        push.pad1 = pad1;
        push.pad2 = pad2;
        push.width = strideW;
        push.height = strideH;
        push.tileOriginX = ox;
        push.tileOriginY = oy;
        push.activeOriginX = ax;
        push.activeOriginY = ay;
        push.activeWidth = aw;
        push.activeHeight = ah;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                corePipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, corePipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(cmd, (aw + 31u) / 32u, (ah + 7u) / 8u, gz);
        computeBarrier();
        if (ri.passBoundary) ri.passBoundary(ri.passBoundaryUserData);
    };
    const auto tAdapt = [&](VkPipeline pipeline, VkDescriptorSet set,
                            uint32_t op, uint32_t pad2, uint32_t strideW,
                            uint32_t strideH, uint32_t ox, uint32_t oy,
                            uint32_t ax, uint32_t ay, uint32_t aw,
                            uint32_t ah) {
        tCore(pipeline, set, op, adaptFlags, pad2, strideW, strideH, ox, oy,
              ax, ay, aw, ah, 1);
    };
    // Row-copy between full-stride and working-stride float4 buffers.
    const auto copyRows = [&](VkBuffer src, uint32_t srcStridePx, VkBuffer dst,
                              uint32_t dstStridePx, uint32_t sx, uint32_t sy,
                              uint32_t dx, uint32_t dy, uint32_t ww,
                              uint32_t wh) {
        std::vector<VkBufferCopy> regions;
        regions.reserve(wh);
        for (uint32_t r = 0; r < wh; ++r) {
            VkBufferCopy c{};
            c.srcOffset =
                (static_cast<VkDeviceSize>(sy + r) * srcStridePx + sx) * 16u;
            c.dstOffset =
                (static_cast<VkDeviceSize>(dy + r) * dstStridePx + dx) * 16u;
            c.size = static_cast<VkDeviceSize>(ww) * 16u;
            regions.push_back(c);
        }
        vkCmdCopyBuffer(cmd, src, dst, static_cast<uint32_t>(regions.size()),
                        regions.data());
    };
    const auto transferBarrier = [&](VkAccessFlags src, VkAccessFlags dst,
                                     VkPipelineStageFlags srcStage,
                                     VkPipelineStageFlags dstStage) {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = src;
        barrier.dstAccessMask = dst;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
    };
    // Diffusion sequence on working geometry (mirrors record()'s
    // runDiffusionSequence; downsample dims via alignedReducedDimension).
    const auto tDiffSeq = [&](VkDescriptorSet set,
                              const std::vector<diffusion::Component>&
                                  components,
                              uint32_t strideW, uint32_t strideH, uint32_t ox,
                              uint32_t oy) {
        const uint32_t n = static_cast<uint32_t>(components.size());
        tCore(diffusionPipeline, set, 0u, 0u, 0u, strideW, strideH, ox, oy, 0u,
              0u, strideW, strideH);
        for (uint32_t c = 0u; c < n;) {
            const uint32_t scale =
                diffusion::downsampleScaleForSigma(components[c].sigmaPx);
            uint32_t group = 1u;
            while (c + group < n && group < 2u &&
                   diffusion::downsampleScaleForSigma(
                       components[c + group].sigmaPx) == scale) {
                ++group;
            }
            if (scale <= 1u) {
                if (group <= 1u) {
                    tCore(diffusionPipeline, set, 1u, c, 0u, strideW, strideH,
                          ox, oy, 0u, 0u, strideW, strideH);
                    tCore(diffusionPipeline, set, 2u, c, 0u, strideW, strideH,
                          ox, oy, 0u, 0u, strideW, strideH);
                } else {
                    tCore(diffusionPipeline, set, 5u, c, group, strideW,
                          strideH, ox, oy, 0u, 0u, strideW, strideH);
                    tCore(diffusionPipeline, set, 6u, c, group, strideW,
                          strideH, ox, oy, 0u, 0u, strideW, strideH);
                }
                c += group;
                continue;
            }
            const uint32_t redW =
                alignedReducedDimension(strideW, ox, w, scale);
            const uint32_t redH =
                alignedReducedDimension(strideH, oy, h, scale);
            const uint32_t redX = ox / scale;
            const uint32_t redY = oy / scale;
            const uint32_t packed =
                (std::min(scale, 0xffffu) << 16u) | std::min(group, 0xffffu);
            // Downsample + blur dispatches address the reduced domain;
            // active rect tracks it (shader maps via tileOrigin/scale).
            push.op = 7u;
            push.pad1 = scale;
            push.pad2 = 0u;
            push.width = strideW;
            push.height = strideH;
            push.tileOriginX = ox;
            push.tileOriginY = oy;
            push.activeOriginX = redX;
            push.activeOriginY = redY;
            push.activeWidth = redW;
            push.activeHeight = redH;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              diffusionPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    corePipelineLayout, 0, 1, &set, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, corePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd, (redW + 31u) / 32u, (redH + 7u) / 8u, 1);
            computeBarrier();
            if (group <= 1u) {
                tCore(diffusionPipeline, set, 8u, c, packed, strideW, strideH,
                      ox, oy, redX, redY, redW, redH);
                tCore(diffusionPipeline, set, 9u, c, packed, strideW, strideH,
                      ox, oy, redX, redY, redW, redH);
                tCore(diffusionPipeline, set, 10u, c, packed, strideW, strideH,
                      ox, oy, 0u, 0u, strideW, strideH);
            } else {
                tCore(diffusionPipeline, set, 11u, c, packed, strideW, strideH,
                      ox, oy, redX, redY, redW, redH);
                tCore(diffusionPipeline, set, 12u, c, packed, strideW, strideH,
                      ox, oy, redX, redY, redW, redH);
                tCore(diffusionPipeline, set, 13u, c, packed, strideW, strideH,
                      ox, oy, 0u, 0u, strideW, strideH);
            }
            c += group;
        }
        tCore(diffusionPipeline, set, 3u, 0u, 0u, strideW, strideH, ox, oy, 0u,
              0u, strideW, strideH);
    };

    // Hoisted per-record rebinds (identical every tile).
    rebindBuffer(slot.tileDevelopSet, 0,
                 (halationPass || cameraDiffusionPath) ? t.halationLogRaw
                                                      : t.filmRaw);
    rebindBuffer(slot.tileDiffusionSets[0], 0,
                 halationBoost ? t.boostedRaw : t.filmRaw);
    if (halationScatter) {
        const VkBuffer src = cameraDiffusionPath ? t.cameraDiffusionRaw
                             : (halationBoost ? t.boostedRaw : t.filmRaw);
        for (uint32_t s : {0u, 2u, 3u, 5u}) {
            rebindBuffer(slot.tileHalationSets[s], 0, src);
        }
    }
    const VkBuffer halBounceSrc =
        halationScatter ? t.halationRawD
                        : (cameraDiffusionPath ? t.cameraDiffusionRaw
                              : (halationBoost ? t.boostedRaw : t.filmRaw));
    if (halationBounce) {
        rebindBuffer(slot.tileHalationSets[6], 0, halBounceSrc);
        rebindBuffer(slot.tileHalationSets[7], 0, halBounceSrc);
        rebindBuffer(slot.tileHalationSets[9], 0, halBounceSrc);
    } else if (halationPass) {
        rebindBuffer(slot.tileHalationSets[9], 0, halBounceSrc);
    }
    if (dirPath) {
        rebindBuffer(slot.tileDirSets[3], 0, t.dirCorrectionC);
        rebindBuffer(slot.tileDirSets[6], 2,
                     dirTailPath ? t.dirCorrectionB
                                 : (dirBlurPath ? t.dirCorrectionC
                                                : t.dirCorrectionA));
        rebindBuffer(slot.tileDirSets[6], 0,
                     (halationPass || cameraDiffusionPath) ? t.halationLogRaw
                                                           : t.filmRaw);
        rebindBuffer(slot.tileGrainSet, 0, t.dirDensity);
    } else {
        rebindBuffer(slot.tileGrainSet, 0, t.filmDensity);
    }
    {
        const VkBuffer densitySrc = previewGrain    ? t.grainDensity
                                    : productionGrain ? t.grainDensityB
                                                      : tileDirDensitySrc;
        if (processNegative) {
            rebindBuffer(slot.tileFinalSet, 0, t.filmRaw);
            rebindBuffer(slot.tilePrintSets[0], 0, t.filmRaw);
        } else {
            rebindBuffer(slot.tileFinalSet, 0, densitySrc);
            rebindBuffer(slot.tilePrintSets[0], 0, densitySrc);
        }
    }
    if (productionGrain) {
        rebindBuffer(slot.tileGrainProdSets[0], 0, tileDirDensitySrc);
        rebindBuffer(slot.tileGrainProdSets[3], 0, tileDirDensitySrc);
    }
    // Frame constants once (shared filteredEnlarger + frame arrays).
    // Idempotent: chunked submits repeat it harmlessly (1x1 dispatch).
    tCore(printScanPipeline, slot.tileFinalSet, 3u, 0u, 0u, w, h, 0u, 0u, 0u,
          0u, 1u, 1u);
    // NOTE: op3 ignores active geometry (1x1); stride fields are irrelevant.
    const bool glowExport = ri.glowGainOutput.view != VK_NULL_HANDLE;
    if (glowExport) {
        if (processNegative || (!halationPass && !cameraDiffusionPath) ||
            !glowRatioPipeline) {
            throw std::invalid_argument(
                "spektrafilm: tiled glow gain needs a linear scatter path and shader");
        }
        const VkBuffer linearSrc = halationScatter ? t.halationRawD :
            cameraDiffusionPath ? t.cameraDiffusionRaw :
            halationBoost ? t.boostedRaw : t.filmRaw;
        writeBufferSet(slot.glowRatioSet,
                       {{0, t.filmRaw}, {1, linearSrc}});
    }

    // Tile grid in row-major order; chunked submits cover a sub-range.
    struct TileWork {
        uint32_t cx, cy, cw, ch;
    };
    std::vector<TileWork> grid;
    for (uint32_t cy = 0; cy < h; cy += tileH) {
        for (uint32_t cx = 0; cx < w; cx += tileW) {
            grid.push_back({cx, cy, std::min(tileW, w - cx),
                            std::min(tileH, h - cy)});
        }
    }
    const uint64_t first =
        std::min<uint64_t>(ri.tileFirst, grid.size());
    const uint64_t last =
        std::min<uint64_t>(first + ri.tileCount, grid.size());
    for (uint64_t gi = first; gi < last; ++gi) {
        const uint32_t cx = grid[(size_t)gi].cx;
        const uint32_t cy = grid[(size_t)gi].cy;
        const uint32_t cw = grid[(size_t)gi].cw;
        const uint32_t ch = grid[(size_t)gi].ch;
        {
            const uint32_t wx0 = cx > ov ? cx - ov : 0u;
            const uint32_t wy0 = cy > ov ? cy - ov : 0u;
            const uint32_t wx1 = std::min(cx + cw + ov, w);
            const uint32_t wy1 = std::min(cy + ch + ov, h);
            const uint32_t ww = wx1 - wx0;
            const uint32_t wh = wy1 - wy0;
            const uint32_t ax = cx - wx0;
            const uint32_t ay = cy - wy0;
            if (!processNegative) {
                tAdapt(exposurePipeline, slot.tileExposureSet,
                       (halationPass || cameraDiffusionPath) ? 1u : 0u, 1u, ww,
                       wh, wx0, wy0, 0u, 0u, ww, wh);
                // BoostApply from the phase-1 milestone (filmRaw, global max
                // baked into the readback): feeds diffusion/scatter below.
                if (halationBoost) {
                    tCore(halationPipeline, slot.tileHalationSets[12], 9u, 0u,
                          0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                }
                if (cameraDiffusionPath) {
                    tDiffSeq(slot.tileDiffusionSets[0], camSolve.components,
                             ww, wh, wx0, wy0);
                    if (!halationPass) {
                        tCore(diffusionPipeline, slot.tileDiffusionSets[1], 4u,
                              0u, 0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    }
                }
                if (halationScatter) {
                    tCore(halationPipeline, slot.tileHalationSets[0], 1u, 0u,
                          0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    tCore(halationPipeline, slot.tileHalationSets[1], 2u, 0u,
                          0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    tCore(halationPipeline, slot.tileHalationSets[2], 0u, 0u,
                          0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    for (uint32_t c = 0; c < 3; ++c) {
                        tCore(halationPipeline, slot.tileHalationSets[3], 1u,
                              1u, c, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                        tCore(halationPipeline, slot.tileHalationSets[4], 3u,
                              1u, c, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    }
                    tCore(halationPipeline, slot.tileHalationSets[5], 4u, 0u,
                          0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                }
                if (halationBounce) {
                    tCore(halationPipeline, slot.tileHalationSets[6], 0u, 0u,
                          0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    for (uint32_t b = 0; b < 3; ++b) {
                        tCore(halationPipeline, slot.tileHalationSets[7], 1u,
                              2u, b, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                        tCore(halationPipeline, slot.tileHalationSets[8], 3u,
                              2u, b, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    }
                    tCore(halationPipeline, slot.tileHalationSets[9], 5u, 0u,
                          0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                } else if (halationPass) {
                    tCore(halationPipeline, slot.tileHalationSets[9], 6u, 0u,
                          0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                }
                if (glowExport) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      glowRatioPipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            glowRatioPipelineLayout, 0, 1,
                                            &slot.glowRatioSet, 0, nullptr);
                    const uint32_t ratioPush[8] =
                        {ww, wh, ax, ay, cx, cy, cw, ch};
                    vkCmdPushConstants(cmd, glowRatioPipelineLayout,
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(ratioPush), ratioPush);
                    vkCmdDispatch(cmd, (cw + 31u) / 32u, (ch + 7u) / 8u, 1u);
                    VkMemoryBarrier readDone{};
                    readDone.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    readDone.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    readDone.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &readDone, 0, nullptr, 0, nullptr);
                }
                tAdapt(developPipeline, slot.tileDevelopSet, 0u, 0u, ww, wh,
                       wx0, wy0, 0u, 0u, ww, wh);
                if (dirPath) {
                    tCore(dirPipeline, slot.tileDirSets[0], 0u, 0u, 0u, ww, wh,
                          wx0, wy0, 0u, 0u, ww, wh);
                    if (dirBlurPath) {
                        tCore(dirPipeline, slot.tileDirSets[1], 1u, 0u, 0u, ww,
                              wh, wx0, wy0, 0u, 0u, ww, wh);
                        tCore(dirPipeline, slot.tileDirSets[2], 2u, 0u, 0u, ww,
                              wh, wx0, wy0, 0u, 0u, ww, wh);
                    }
                    if (dirTailPath) {
                        tCore(dirPipeline, slot.tileDirSets[3], 3u, 0u, 0u, ww,
                              wh, wx0, wy0, 0u, 0u, ww, wh);
                        for (uint32_t c = 0; c < 3; ++c) {
                            tCore(dirPipeline, slot.tileDirSets[4], 4u, c, 0u,
                                  ww, wh, wx0, wy0, 0u, 0u, ww, wh, 1);
                            tCore(dirPipeline, slot.tileDirSets[5], 5u, c, 0u,
                                  ww, wh, wx0, wy0, 0u, 0u, ww, wh, 1);
                        }
                    }
                    tCore(dirPipeline, slot.tileDirSets[6], 6u, 0u, 0u, ww, wh,
                          wx0, wy0, 0u, 0u, ww, wh);
                }
                if (previewGrain) {
                    tCore(grainPipeline, slot.tileGrainSet, 0u, 0u, 0u, ww, wh,
                          wx0, wy0, 0u, 0u, ww, wh, 1);
                }
                if (productionGrain) {
                    tCore(grainPipeline, slot.tileGrainProdSets[0], 1u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh, 9);
                    for (uint32_t layerBase = 0u; layerBase < 9u; layerBase += 3u) {
                        tCore(grainPipeline, slot.tileGrainProdSets[0], 14u,
                              layerBase, 0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh, 3);
                        tCore(grainPipeline, slot.tileGrainProdSets[0], 15u,
                              layerBase, 0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh, 3);
                    }
                    tCore(grainPipeline, slot.tileGrainProdSets[0], 4u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    tCore(grainPipeline, slot.tileGrainProdSets[0], 5u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    tCore(grainPipeline, slot.tileGrainProdSets[0], 6u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    tCore(grainPipeline, slot.tileGrainProdSets[0], 7u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    tCore(grainPipeline, slot.tileGrainProdSets[1], 8u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    tCore(grainPipeline, slot.tileGrainProdSets[2], 9u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    tCore(grainPipeline, slot.tileGrainProdSets[3], 10u, 0u,
                          0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                }
            } else {
                // ProcessNegative staging: working rows of the full source
                // into tile filmRaw (free on this path: no exposure).
                transferBarrier(VK_ACCESS_SHADER_WRITE_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
                copyRows(slot.source, w, t.filmRaw, ww, wx0, wy0, 0u, 0u, ww,
                         wh);
                transferBarrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            }
            if (printDiffusionPath) {
                rebindBuffer(slot.tilePrintSets[1], 1,
                             scannerPath ? t.scanA : t.dest);
                tCore(printScanPipeline, slot.tilePrintSets[0],
                      processNegative ? 4u : 1u, 0u, 0u, ww, wh, wx0, wy0, 0u,
                      0u, ww, wh);
                tDiffSeq(slot.tileDiffusionSets[2], printSolve.components, ww,
                         wh, wx0, wy0);
                tCore(printScanPipeline, slot.tilePrintSets[1], 2u,
                      scannerPath ? 1u : 0u, 0u, ww, wh, wx0, wy0, 0u, 0u, ww,
                      wh);
            } else {
                rebindBuffer(slot.tileFinalSet, 1,
                             scannerPath ? t.scanA : t.dest);
                tCore(printScanPipeline, slot.tileFinalSet,
                      processNegative ? 4u : 0u, scannerPath ? 1u : 0u, 0u, ww,
                      wh, wx0, wy0, 0u, 0u, ww, wh);
            }
            if (scannerPath) {
                const VkBuffer postGlare = scanGlarePath ? t.scanB : t.scanA;
                const VkBuffer postBlur = scanBlurPath ? t.scanA : postGlare;
                const VkBuffer unsharpBuf = scanBlurPath ? t.scanB : t.scanA;
                if (scanGlarePath) {
                    tCore(scannerPipeline, slot.tileScanSets[0], 0u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    if (ri.look.glareBlur > 0.0f) {
                        tCore(scannerPipeline, slot.tileScanSets[1], 1u, 0u,
                              0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                        tCore(scannerPipeline, slot.tileScanSets[2], 2u, 0u,
                              0u, ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    }
                    tCore(scannerPipeline, slot.tileScanSets[3], 3u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                }
                if (scanBlurPath) {
                    rebindBuffer(slot.tileScanSets[4], 0, postGlare);
                    tCore(scannerPipeline, slot.tileScanSets[4], 4u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    tCore(scannerPipeline, slot.tileScanSets[5], 5u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                }
                if (scanUnsharpPath) {
                    rebindBuffer(slot.tileScanSets[6], 0, postBlur);
                    tCore(scannerPipeline, slot.tileScanSets[6], 6u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                    rebindBuffer(slot.tileScanSets[7], 1, unsharpBuf);
                    tCore(scannerPipeline, slot.tileScanSets[7], 7u, 0u, 0u,
                          ww, wh, wx0, wy0, 0u, 0u, ww, wh);
                }
                rebindBuffer(slot.tileScanSets[8], 0, postBlur);
                rebindBuffer(slot.tileScanSets[8], 2,
                             scanUnsharpPath ? unsharpBuf : postBlur);
                tCore(scannerPipeline, slot.tileScanSets[8], 8u, 0u, 0u, ww,
                      wh, wx0, wy0, 0u, 0u, ww, wh);
            }
            // Accumulate the exact center into the full-frame destination.
            transferBarrier(VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyRows(t.dest, ww, slot.destination, w, ax, ay, cx, cy, cw, ch);
            transferBarrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
    }
    // Output dispatch (full-frame, mirrors record() tail). Skipped for
    // non-final chunks (the destination accumulates across submits; only
    // the last chunk needs to pack the image).
    if (!ri.tileFinalize) {
        return;
    }
    {
        uint32_t ioPush[16] = {w, h, 0u, 0u};
        std::memcpy(&ioPush[4], &ri.sensorToLinearSrgb[0], 3 * sizeof(float));
        std::memcpy(&ioPush[8], &ri.sensorToLinearSrgb[3], 3 * sizeof(float));
        std::memcpy(&ioPush[12], &ri.sensorToLinearSrgb[6], 3 * sizeof(float));
        const char* ditherText = std::getenv("SPEKTRA_FILM_OUTPUT_DITHER");
        ioPush[2] = (ditherText && (ditherText[0] == '1' || ditherText[0] == 't' ||
                                    ditherText[0] == 'y'))
                        ? 1u
                        : 0u;
        // Per-record output image binding (tile path shares slot.outputSet).
        VkDescriptorImageInfo outputImage{};
        outputImage.imageView = ri.output.view;
        outputImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet imageWrite{};
        imageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        imageWrite.dstBinding = 1;
        imageWrite.descriptorCount = 1;
        imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        imageWrite.dstSet = slot.outputSet;
        imageWrite.pImageInfo = &outputImage;
        vkUpdateDescriptorSets(device, 1, &imageWrite, 0, nullptr);
        VkMemoryBarrier outputReady{};
        outputReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        outputReady.srcAccessMask = 0;
        outputReady.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &outputReady, 0, nullptr, 0, nullptr);
        VkPipeline outPipeline = outputPipeline;
        if (ri.output.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
            if (!outputHalfPipeline) {
                throw std::invalid_argument(
                    "spektrafilm: RGBA16F output needs hdrOutputSpirv at create");
            }
            outPipeline = outputHalfPipeline;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, outPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                ioPipelineLayout, 0, 1, &slot.outputSet, 0,
                                nullptr);
        vkCmdPushConstants(cmd, ioPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(ioPush), ioPush);
        vkCmdDispatch(cmd, (w + 31u) / 32u, (h + 7u) / 8u, 1);
    }
}

bool SpektraFilm::validateCreateInfo(const SpektraFilmCreateInfo& ci,
                                     const char** reason) noexcept {
    const auto reject = [&](const char* message) {
        if (reason) {
            *reason = message;
        }
        return false;
    };
    if (!ci.context.physicalDevice || !ci.context.device || !ci.queue) {
        return reject("spektrafilm: Vulkan context/queue incomplete");
    }
    if (ci.maxFramesInFlight == 0 || ci.maxFramesInFlight > 8) {
        return reject("spektrafilm: maxFramesInFlight out of range");
    }
    if (ci.maxWidth == 0 || ci.maxHeight == 0 || ci.maxWidth > 8192 ||
        ci.maxHeight > 8192) {
        return reject("spektrafilm: max dimensions invalid");
    }
    if (!validSpirv(ci.inputSpirv, ci.inputSpirvBytes) ||
        !validSpirv(ci.exposureSpirv, ci.exposureSpirvBytes) ||
        !validSpirv(ci.developSpirv, ci.developSpirvBytes) ||
        !validSpirv(ci.printScanSpirv, ci.printScanSpirvBytes) ||
        !validSpirv(ci.outputSpirv, ci.outputSpirvBytes) ||
        !validSpirv(ci.grainSpirv, ci.grainSpirvBytes) ||
        !validSpirv(ci.dirSpirv, ci.dirSpirvBytes) ||
        !validSpirv(ci.halationSpirv, ci.halationSpirvBytes) ||
        !validSpirv(ci.diffusionSpirv, ci.diffusionSpirvBytes) ||
        !validSpirv(ci.scannerSpirv, ci.scannerSpirvBytes)) {
        return reject("spektrafilm: SPIR-V invalid");
    }
    if (!ci.hanatosSpectra || ci.hanatosSpectraFloats == 0 || !ci.gamutCompression ||
        ci.gamutCompressionFloats < 26u * 18u) {
        return reject("spektrafilm: data assets missing");
    }
    if (!spektrafilm::filmProfileCurves(ci.look.film)) {
        return reject("spektrafilm: unknown film index");
    }
    if (!spektrafilm::paperProfileCurves(ci.look.paper)) {
        return reject("spektrafilm_native: unknown paper index");
    }
    if (ci.look.rgbToRawMethod < 0 || ci.look.rgbToRawMethod > 2) {
        return reject("spektrafilm_native: unknown spectral method");
    }
    if (ci.tiledMemorySaving) {
        if (ci.maxTileWidth < 64u || ci.maxTileWidth > 8192u ||
            ci.maxTileHeight < 64u || ci.maxTileHeight > 8192u) {
            return reject("spektrafilm: maxTile dimensions out of range (64..8192)");
        }
    }
    if ((ci.hdrOutputSpirv || ci.hdrOutputSpirvBytes) &&
        !validSpirv(ci.hdrOutputSpirv, ci.hdrOutputSpirvBytes)) {
        return reject("spektrafilm: HDR output SPIR-V invalid");
    }
    if ((ci.boostMilestoneSpirv || ci.boostMilestoneSpirvBytes) &&
        !validSpirv(ci.boostMilestoneSpirv, ci.boostMilestoneSpirvBytes)) {
        return reject("spektrafilm: boost milestone SPIR-V invalid");
    }
    if ((ci.glowRatioSpirv || ci.glowRatioSpirvBytes) &&
        !validSpirv(ci.glowRatioSpirv, ci.glowRatioSpirvBytes)) {
        return reject("spektrafilm: glow ratio SPIR-V invalid");
    }
    return true;
}

bool SpektraFilm::validateRecordInfo(const SpektraFilmRecordInfo& ri,
                                     uint32_t maxFlights, uint32_t maxWidth,
                                     uint32_t maxHeight,
                                     const char** reason) noexcept {
    const auto reject = [&](const char* message) {
        if (reason) {
            *reason = message;
        }
        return false;
    };
    if (!std::all_of(ri.adaptiveTone.begin(), ri.adaptiveTone.end(), [](float v) { return std::isfinite(v); }) ||
        ri.adaptiveTone[0] < 0.0f || ri.adaptiveTone[0] > 3.0f || !(ri.adaptiveTone[3] > 0.0f)) {
        return reject("spektrafilm: invalid adaptive tone curve");
    }
    if (!ri.commandBuffer) {
        return reject("spektrafilm: command buffer null");
    }
    if (!ri.input.view || !ri.output.view) {
        return reject("spektrafilm: image view null");
    }
    if (ri.input.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
        return reject("spektrafilm: input must be RGBA16F");
    }
    if (ri.output.format != VK_FORMAT_R8G8B8A8_UNORM &&
        ri.output.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
        return reject("spektrafilm: output must be RGBA8 or RGBA16F");
    }
    if (ri.input.layout != VK_IMAGE_LAYOUT_GENERAL ||
        ri.output.layout != VK_IMAGE_LAYOUT_GENERAL) {
        return reject("spektrafilm: images must be GENERAL");
    }
    if (ri.glowGainOutput.view != VK_NULL_HANDLE) {
        if (ri.glowGainOutput.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
            return reject("spektrafilm: glow gain output must be RGBA16F");
        }
        if (ri.glowGainOutput.layout != VK_IMAGE_LAYOUT_GENERAL) {
            return reject("spektrafilm: glow gain output must be GENERAL");
        }
        if (ri.glowGainOutput.width != ri.input.width || ri.glowGainOutput.height != ri.input.height) {
            return reject("spektrafilm: glow gain output dimensions must match input (full-res V1)");
        }
    }
    if (ri.input.width == 0 || ri.input.height == 0 ||
        ri.output.width != ri.input.width || ri.output.height != ri.input.height) {
        return reject("spektrafilm: dimensions invalid/mismatched");
    }
    if (ri.input.width > maxWidth || ri.input.height > maxHeight) {
        return reject("spektrafilm: frame exceeds arena");
    }
    if (ri.frameSlot >= maxFlights) {
        return reject("spektrafilm: frameSlot out of range");
    }
    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(ri.sensorToLinearSrgb[i])) {
            return reject("spektrafilm: sensor matrix not finite");
        }
    }
    if (static_cast<int32_t>(ri.tilingMode) < 0 ||
        static_cast<int32_t>(ri.tilingMode) > 1) {
        return reject("spektrafilm: bad tiling mode");
    }
    if (ri.tilingMode == GpuRenderTilingMode::Tiled) {
        if (ri.tileWidth < 64u || ri.tileWidth > 8192u || ri.tileHeight < 64u ||
            ri.tileHeight > 8192u) {
            return reject("spektrafilm: tile dimensions out of range (64..8192)");
        }
        if (ri.look.grainEnabled && ri.look.grainModel == 2) {
            return reject("spektrafilm: tiled synthesis needs full-frame path");
        }
        // Tiled + boost is allowed: tile-memory engines run the two-phase
        // milestone (recordBoostMilestone + hasBoostInfo); full-frame
        // compute tiling cannot (no GPU-readback split) and is rejected in
        // record() where the engine kind is known.
    }
    return true;
}

SpektraFilm::SpektraFilm(const SpektraFilmCreateInfo& createInfo)
    : impl_(std::make_unique<Impl>(createInfo)) {}

SpektraFilm::~SpektraFilm() = default;

uint32_t SpektraFilm::maxFramesInFlight() const noexcept {
    return impl_->slotCount;
}

void SpektraFilm::updateCameraFilters(const CameraFilters& filters) {
    Impl& impl = *impl_;
    const spektrafilm::HanatosSpectraLutInfo& hanatos =
        spektrafilm::hanatosSpectraLutInfo();
    const char* reason = nullptr;
    const tables::FilmSpectral spectral = tables::rebuildFilmSpectral(
        *impl.filmCurves, impl.bakedMethod, filters, impl.hanatosSpectraData,
        hanatos, &reason);
    if (impl.bakedMethod != 1 && spectral.hanatosPair.empty()) {
        throw std::runtime_error(reason ? reason : "film spectral rebuild failed");
    }
    impl.uploadToStatic(Impl::kMallett, std::vector<float>(
                                            spectral.mallett.begin(),
                                            spectral.mallett.end()));
    if (impl.bakedMethod != 1) {
        impl.uploadToStatic(Impl::kHanatosResponse, spectral.hanatosPair);
        impl.tables.hanatosRawResponse = spectral.hanatosPair;
    }
    impl.tables.mallettRawMatrix.assign(spectral.mallett.begin(),
                                        spectral.mallett.end());
    impl.bakedCamera = filters;
    impl.bakedLook.cameraUvFilterEnabled = filters.uvEnabled;
    impl.bakedLook.cameraUvCutNm = filters.uvCutNm;
    impl.bakedLook.cameraIrFilterEnabled = filters.irEnabled;
    impl.bakedLook.cameraIrCutNm = filters.irCutNm;
}

CameraFilters SpektraFilm::bakedCameraFilters() const noexcept {
    return impl_->bakedCamera;
}

bool SpektraFilm::willWriteGlowGain(const FilmLook& look, uint32_t w, uint32_t h,
                                     GpuRenderTilingMode tilingMode, bool tileMemorySaving) noexcept {
    // Full-frame and memory-saving tile records can both export the scatter
    // quotient. Full-frame compute tiling cannot, since its mid-chain tap is
    // not partitioned by tile.
    if (look.process == 2) return false;
    if (tileMemorySaving && tilingMode != GpuRenderTilingMode::Tiled) return false;
    if (tilingMode == GpuRenderTilingMode::Tiled && !tileMemorySaving) return false;
    if (w == 0 || h == 0) return false;
    // Halation sub-paths, mirroring record() (minus buffer allocation, which
    // the caller guarantees via the conditional-scratch rebuild contract).
    const bool feature = look.halationEnabled;
    const bool scatter = feature && look.scatterAmount > 0.0f && look.scatterScale > 0.0f;
    const bool bounce = feature && look.halationAmount > 0.0f && look.halationScale > 0.0f &&
                        (look.halationStrengthR > 0.0f || look.halationStrengthG > 0.0f ||
                         look.halationStrengthB > 0.0f);
    const bool boost = feature && look.halationBoostEv > 0.0f;
    const bool halationPass = scatter || bounce || boost;
    if (halationPass) return true;
    // Camera-diffusion solve needs the record pixel size.
    const float enlarger = std::clamp(look.enlargerScale, 1.0f, 32.0f);
    const float longEdgeMm = tables::filmFormatLongEdgeMm(look.filmFormat);
    const float pixelSizeUm = longEdgeMm * 1000.0f / static_cast<float>(std::max(w, h)) / enlarger;
    if (!std::isfinite(pixelSizeUm) || pixelSizeUm <= 0.0f) return false;
    try {
        const diffusion::Solve solve = diffusion::solveCamera(look, pixelSizeUm);
        return solve.info.componentCount > 0u && !solve.components.empty();
    } catch (...) {
        return false;
    }
}

void SpektraFilm::updateProcessNegativeTables(const FilmLook& look) {
    Impl& impl = *impl_;
    const spektrafilm::HanatosSpectraLutInfo& hanatos =
        spektrafilm::hanatosSpectraLutInfo();
    const tables::PaperWeights weights = tables::rebuildProcessNegativeWeights(
        *impl.filmCurves, *impl.paperCurves, impl.bakedLook.film,
        impl.bakedLook.paper, look.printTiming, look.filterC, look.filterMShift,
        look.filterYShift, look.preflashExposure, look.preflashMFilterShift,
        look.preflashYFilterShift, impl.hanatosSpectraData, hanatos);
    if (weights.paperHanatos.empty() || weights.preflashHanatos.empty()) {
        throw std::runtime_error("spektrafilm: process-negative rebuild failed");
    }
    impl.uploadToStatic(Impl::kPaperHanatos, weights.paperHanatos);
    impl.uploadToStatic(Impl::kPreflashHanatos, weights.preflashHanatos);
    impl.tables.paperHanatosResponse = weights.paperHanatos;
    impl.tables.preflashPaperHanatosResponse = weights.preflashHanatos;
    impl.bakedProcessNegative.look = look;
    impl.bakedProcessNegative.valid = true;
}

void SpektraFilm::record(const SpektraFilmRecordInfo& ri) {
    const char* reason = nullptr;
    if (!validateRecordInfo(ri, impl_->slotCount, impl_->maxWidth, impl_->maxHeight,
                            &reason)) {
        throw std::invalid_argument(reason ? reason : "invalid record info");
    }
    if (!validLookForRecord(ri.look, impl_->bakedLook,
                            impl_->bakedCamera, &reason)) {
        throw std::invalid_argument(reason ? reason : "invalid look");
    }
    Impl& impl = *impl_;
    Impl::Slot& slot = impl.slots[ri.frameSlot];
    const uint32_t w = ri.input.width;
    const uint32_t h = ri.input.height;
    const VkCommandBuffer cmd = ri.commandBuffer;
    const VkDevice device = impl.context.device;

    // Frame arrays (tiny host-visible upload).
    float frameFloats[105];
    uint32_t frameInts[29];
    const float enlarger =
        std::clamp(ri.look.enlargerScale, 1.0f, 32.0f);
    const float longEdgeMm = tables::filmFormatLongEdgeMm(ri.look.filmFormat);
    const float pixelSizeUm =
        longEdgeMm * 1000.0f / static_cast<float>(std::max(w, h)) / enlarger;
    tables::fillFrameArrays(*impl.filmCurves, impl.tables, ri.look,
                            impl.bakedLook.film, impl.bakedLook.paper,
                            pixelSizeUm, longEdgeMm, ri.timeSec, frameFloats,
                            frameInts);
    frameInts[1] = static_cast<uint32_t>(ri.look.outputColorSpace);
    frameInts[2] = static_cast<uint32_t>(std::clamp(ri.look.outputRole, 0, 2));
    const uint32_t adaptFlags = tables::colorAdaptationFlags(ri.look);
    const bool processNegative = ri.look.process == 2;
    const bool rcmOutput = ri.look.outputRole == 2;
    // Scanner post frame state (mirrors upstream renderCoreBootstrap fill).
    // Ints 13..15 gate the pass; white/black levels live at floats 24/25.
    frameInts[13] = ri.look.scannerEnabled ? 1u : 0u;
    frameInts[14] = ri.look.scannerWhiteCorrection ? 1u : 0u;
    frameInts[15] = ri.look.scannerBlackCorrection ? 1u : 0u;
    frameFloats[24] = ri.look.scannerWhiteLevel;
    frameFloats[25] = ri.look.scannerBlackLevel;
    frameFloats[40] = scannerSigmaUmFromMtf50(ri.look.scannerMtf50LpMm) /
                      std::max(pixelSizeUm, 1.0e-6f);
    frameFloats[41] = std::max(ri.look.scannerUnsharpRadiusUm, 0.0f) /
                      std::max(pixelSizeUm, 1.0e-6f);
    frameFloats[42] = ri.look.scannerUnsharpAmount;
    frameFloats[43] = ri.look.glarePercent;
    frameFloats[44] = ri.look.glareRoughness;
    frameFloats[45] = ri.look.glareBlur;
    {
        float glare[3];
        scanGlareRgb(*impl.paperCurves, ri.look.outputColorSpace, glare);
        frameFloats[46] = glare[0];
        frameFloats[47] = glare[1];
        frameFloats[48] = glare[2];
    }
    std::memcpy(slot.mappedFloats, frameFloats, sizeof(frameFloats));
    std::memcpy(slot.mappedInts, frameInts, sizeof(frameInts));
    // Host-visible writes need a HOST_WRITE availability barrier even though
    // the memory is COHERENT (coherent removes the flush, not the dependency).
    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &hostBarrier,
                         0, nullptr, 0, nullptr);

    CorePush push{};
    push.width = w;
    push.height = h;
    push.filmExposureEv = ri.look.filmExposureEv;
    std::copy(ri.adaptiveTone.begin(), ri.adaptiveTone.end(), push.adaptiveTone);
    push.filmGamma = tables::effectiveFilmGamma(ri.look.filmPushPullMode,
                                                ri.look.filmGamma,
                                                ri.look.filmPushPullStops);
    push.filmPushPullMode = ri.look.filmPushPullMode;
    push.filmPushPullStops = ri.look.filmPushPullStops;
    push.exposureCount = impl.tables.exposureCount;
    push.inputColorSpace = ri.look.inputColorSpace;
    push.rgbToRawMethod = ri.look.rgbToRawMethod;
    push.colorDecodeMin = spektrafilm::colorDecodeLutMin();
    push.colorDecodeMax = spektrafilm::colorDecodeLutMax();
    const auto& hanatos = spektrafilm::hanatosSpectraLutInfo();
    push.hanatosWidth = hanatos.width;
    push.hanatosHeight = hanatos.height;
    push.fullWidth = w;
    push.fullHeight = h;
    push.activeWidth = w;
    push.activeHeight = h;
    // Profiling gate (debug only): stop after N stages to attribute cost.
    // 1=input copy, 2=+exposure, 3=+develop, 4=+printscan, 5=+output.
    // Unset (or >5) = full chain. Bench timing only; output is partial.
    int profileUpto = 6;
    if (const char* profileText = std::getenv("SPEKTRA_FILM_PROFILE_UPTO")) {
        profileUpto = std::atoi(profileText);
    }
    const uint32_t groupsX = (w + 31u) / 32u;
    const uint32_t groupsY = (h + 7u) / 8u;
    // Workgroup sweep (debug only): "WxH" must match the compiled shaders'
    // local_size (validation parity guards mismatches). Default 32x8.
    uint32_t wgX = 32, wgY = 8;
    if (const char* wgText = std::getenv("SPEKTRA_FILM_WG")) {
        unsigned x = 0, y = 0;
        if (std::sscanf(wgText, "%ux%u", &x, &y) == 2 && x > 0 && y > 0) {
            wgX = x;
            wgY = y;
        }
    }
    const uint32_t sweepGroupsX = (w + wgX - 1u) / wgX;
    const uint32_t sweepGroupsY = (h + wgY - 1u) / wgY;
    // Compute tiling: Tiled splits full-res 2D passes into tileW x tileH
    // activeRect tiles (bit-identical: transients stay full-frame). Downsample
    // + 1D + 1x1 passes stay full-frame. Tiled+synthesis/boost rejected in
    // validateRecordInfo.
    const bool tiled =
        ri.tilingMode == GpuRenderTilingMode::Tiled;
    const uint32_t tileW =
        tiled ? std::min(std::max(ri.tileWidth, 64u), w) : w;
    const uint32_t tileH =
        tiled ? std::min(std::max(ri.tileHeight, 64u), h) : h;
    struct TileRect {
        uint32_t ox = 0, oy = 0, tw = 0, th = 0;
    };
    std::vector<TileRect> tiles;
    if (tiled) {
        for (uint32_t oy = 0; oy < h; oy += tileH) {
            for (uint32_t ox = 0; ox < w; ox += tileW) {
                tiles.push_back({ox, oy, std::min(tileW, w - ox),
                                 std::min(tileH, h - oy)});
            }
        }
    } else {
        tiles.push_back({0, 0, w, h});
    }
    // IO push: width/height + row-major sensor->linear-sRGB as 3x vec4
    // rows (vec4 rows keep push-constant alignment unambiguous; the output
    // shader declares only width/height and ignores the rest).
    uint32_t ioPush[16] = {w, h, 0u, 0u};
    static_assert(sizeof(ri.sensorToLinearSrgb) == 9 * sizeof(float),
                  "sensor matrix must be 9 floats");
    std::memcpy(&ioPush[4], &ri.sensorToLinearSrgb[0], 3 * sizeof(float));
    std::memcpy(&ioPush[8], &ri.sensorToLinearSrgb[3], 3 * sizeof(float));
    std::memcpy(&ioPush[12], &ri.sensorToLinearSrgb[6], 3 * sizeof(float));

    const auto computeBarrier = [&]() {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier,
                             0, nullptr, 0, nullptr);
    };
    const auto dispatchCore = [&](VkPipeline pipeline, VkDescriptorSet set,
                                  uint32_t op, uint32_t gx, uint32_t gy,
                                  uint32_t gz = 1, uint32_t component = 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                impl.corePipelineLayout, 0, 1, &set, 0, nullptr);
        if (tiles.size() <= 1 || (gx <= 1u && gy <= 1u)) {
            // Full-frame (or 1x1 constants): single dispatch.
            push.op = op;
            push.pad1 = component;
            push.pad2 = 0;
            push.activeOriginX = 0;
            push.activeOriginY = 0;
            push.activeWidth = w;
            push.activeHeight = h;
            vkCmdPushConstants(cmd, impl.corePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd, gx, gy, gz);
            computeBarrier();
            return;
        }
        // Tile step inferred from the full-frame group count so sweep (wgXxwgY),
        // 32x8, and 1x1 call sites all tile correctly.
        const uint32_t stepX = (w + std::max(gx, 1u) - 1u) / std::max(gx, 1u);
        const uint32_t stepY = (h + std::max(gy, 1u) - 1u) / std::max(gy, 1u);
        for (const TileRect& t : tiles) {
            push.op = op;
            push.pad1 = component;
            push.pad2 = 0;
            push.activeOriginX = t.ox;
            push.activeOriginY = t.oy;
            push.activeWidth = t.tw;
            push.activeHeight = t.th;
            vkCmdPushConstants(cmd, impl.corePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (t.tw + stepX - 1u) / stepX,
                          (t.th + stepY - 1u) / stepY, gz);
        }
        computeBarrier();
    };
    // Exposure/develop carry adaptation flags in pad1 (upstream
    // colorAdaptationFlags), not a component index.
    const auto dispatchAdapt = [&](VkPipeline pipeline, VkDescriptorSet set,
                                   uint32_t op) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                impl.corePipelineLayout, 0, 1, &set, 0, nullptr);
        const uint32_t stepX = (w + groupsX - 1u) / groupsX;
        const uint32_t stepY = (h + groupsY - 1u) / groupsY;
        for (const TileRect& t : tiles) {
            push.op = op;
            push.pad1 = adaptFlags;
            push.pad2 = 0;
            push.activeOriginX = t.ox;
            push.activeOriginY = t.oy;
            push.activeWidth = t.tw;
            push.activeHeight = t.th;
            vkCmdPushConstants(cmd, impl.corePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd, (t.tw + stepX - 1u) / stepX,
                          (t.th + stepY - 1u) / stepY, 1);
        }
        computeBarrier();
    };
    // Descriptor state persists across records: sets whose sources vary per
    // record (halation/scatter sources, develop input, DIR/grain routing)
    // are rebound every record, including the restore-to-default.
    const auto rebindBuffer = [&](VkDescriptorSet set, uint32_t binding,
                                  VkBuffer buffer) {
        VkDescriptorBufferInfo info{};
        info.buffer = buffer;
        info.offset = 0;
        info.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    };
    // Diffusion dispatch helpers + sequence runner (upstream
    // dispatchDiffusion/dispatchDiffusionActiveSized/dispatchDiffusionSized/
    // dispatchDiffusionSequence with default backend tuning: group size 2,
    // downsample auto). Shared by the camera slot (diffusionSets[0]) and the
    // print slot (diffusionSets[2]).
    enum : uint32_t {
        kDifOpClear = 0u,
        kDifOpBlurX = 1u,
        kDifOpBlurYAccumulate = 2u,
        kDifOpResolve = 3u,
        kDifOpRawToLog = 4u,
        kDifOpGroupBlurX = 5u,
        kDifOpGroupBlurYAccumulate = 6u,
        kDifOpDownsample = 7u,
        kDifOpDownsampleBlurX = 8u,
        kDifOpDownsampleBlurY = 9u,
        kDifOpDownsampleUpsampleAccumulate = 10u,
        kDifOpDownsampleGroupBlurX = 11u,
        kDifOpDownsampleGroupBlurY = 12u,
        kDifOpDownsampleGroupUpsampleAccumulate = 13u,
        kDifGroupSize = 2u
    };
    const auto dispatchDiffusion = [&](VkDescriptorSet set, uint32_t op,
                                       uint32_t component) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl.diffusionPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                impl.corePipelineLayout, 0, 1, &set, 0,
                                nullptr);
        const uint32_t stepX = (w + groupsX - 1u) / groupsX;
        const uint32_t stepY = (h + groupsY - 1u) / groupsY;
        for (const TileRect& t : tiles) {
            push.op = op;
            push.pad1 = component;
            push.pad2 = 0;
            push.activeOriginX = t.ox;
            push.activeOriginY = t.oy;
            push.activeWidth = t.tw;
            push.activeHeight = t.th;
            vkCmdPushConstants(cmd, impl.corePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd, (t.tw + stepX - 1u) / stepX,
                          (t.th + stepY - 1u) / stepY, 1);
        }
        computeBarrier();
    };
    const auto dispatchDiffusionActiveSized = [&](VkDescriptorSet set,
                                                  uint32_t op, uint32_t value1,
                                                  uint32_t value2) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl.diffusionPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                impl.corePipelineLayout, 0, 1, &set, 0,
                                nullptr);
        const uint32_t stepX = (w + groupsX - 1u) / groupsX;
        const uint32_t stepY = (h + groupsY - 1u) / groupsY;
        for (const TileRect& t : tiles) {
            push.op = op;
            push.pad1 = value1;
            push.pad2 = value2;
            push.activeOriginX = t.ox;
            push.activeOriginY = t.oy;
            push.activeWidth = t.tw;
            push.activeHeight = t.th;
            vkCmdPushConstants(cmd, impl.corePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd, (t.tw + stepX - 1u) / stepX,
                          (t.th + stepY - 1u) / stepY, 1);
        }
        computeBarrier();
    };
    const auto dispatchDiffusionSized = [&](VkDescriptorSet set, uint32_t op,
                                            uint32_t value1, uint32_t value2,
                                            uint32_t dispW, uint32_t dispH) {
        push.op = op;
        push.pad1 = value1;
        push.pad2 = value2;
        push.activeOriginX = 0;
        push.activeOriginY = 0;
        push.activeWidth = w;
        push.activeHeight = h;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl.diffusionPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                impl.corePipelineLayout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, impl.corePipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(cmd, (std::max(dispW, 1u) + 31u) / 32u,
                      (std::max(dispH, 1u) + 7u) / 8u, 1);
        computeBarrier();
    };
    const auto runDiffusionSequence = [&](VkDescriptorSet set,
                                          const std::vector<diffusion::Component>&
                                              components) {
        const auto packDiffusionGroup = [](uint32_t groupCount,
                                           uint32_t downsampleScale) {
            return (std::min(downsampleScale, 0xffffu) << 16u) |
                   std::min(groupCount, 0xffffu);
        };
        const uint32_t componentCount =
            static_cast<uint32_t>(components.size());
        dispatchDiffusion(set, kDifOpClear, 0u);
        for (uint32_t component = 0u; component < componentCount;) {
            const uint32_t downsampleScale =
                diffusion::downsampleScaleForSigma(
                    components[component].sigmaPx);
            uint32_t groupCount = 1u;
            while (component + groupCount < componentCount &&
                   groupCount < kDifGroupSize &&
                   diffusion::downsampleScaleForSigma(
                       components[component + groupCount].sigmaPx) ==
                       downsampleScale) {
                ++groupCount;
            }
            if (downsampleScale <= 1u) {
                if (groupCount <= 1u) {
                    dispatchDiffusion(set, kDifOpBlurX, component);
                    dispatchDiffusion(set, kDifOpBlurYAccumulate, component);
                } else {
                    dispatchDiffusionActiveSized(set, kDifOpGroupBlurX,
                                                 component, groupCount);
                    dispatchDiffusionActiveSized(
                        set, kDifOpGroupBlurYAccumulate, component,
                        groupCount);
                }
                component += groupCount;
                continue;
            }
            const uint32_t reducedW =
                (w + downsampleScale - 1u) / downsampleScale;
            const uint32_t reducedH =
                (h + downsampleScale - 1u) / downsampleScale;
            dispatchDiffusionSized(set, kDifOpDownsample, downsampleScale, 0u,
                                   reducedW, reducedH);
            if (groupCount <= 1u) {
                const uint32_t packed =
                    packDiffusionGroup(1u, downsampleScale);
                dispatchDiffusionSized(set, kDifOpDownsampleBlurX, component,
                                       packed, reducedW, reducedH);
                dispatchDiffusionSized(set, kDifOpDownsampleBlurY, component,
                                       packed, reducedW, reducedH);
                dispatchDiffusionActiveSized(
                    set, kDifOpDownsampleUpsampleAccumulate, component,
                    packed);
            } else {
                const uint32_t packed =
                    packDiffusionGroup(groupCount, downsampleScale);
                dispatchDiffusionSized(set, kDifOpDownsampleGroupBlurX,
                                       component, packed, reducedW, reducedH);
                dispatchDiffusionSized(set, kDifOpDownsampleGroupBlurY,
                                       component, packed, reducedW, reducedH);
                dispatchDiffusionActiveSized(
                    set, kDifOpDownsampleGroupUpsampleAccumulate, component,
                    packed);
            }
            component += groupCount;
        }
        dispatchDiffusion(set, kDifOpResolve, 0u);
    };

    const auto diagnosticTap = [&](const char* name, VkBuffer buffer) {
        if (!ri.diagnosticTap || !buffer) return;
        if (!impl.diagnosticTransfers || ri.tilingMode != GpuRenderTilingMode::LegacyFullFrame)
            throw std::invalid_argument("spektrafilm: diagnostic tap requires full-frame transfer-enabled engine");
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
        ri.diagnosticTap(ri.diagnosticUserData, cmd, name, buffer, w, h);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    };

    // Per-record image bindings (views vary per frame).
    VkDescriptorImageInfo inputImage{};
    inputImage.imageView = ri.input.view;
    inputImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo outputImage{};
    outputImage.imageView = ri.output.view;
    outputImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet imageWrite{};
    imageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    imageWrite.dstBinding = 1;
    imageWrite.descriptorCount = 1;
    imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    imageWrite.dstSet = slot.inputSet;
    imageWrite.pImageInfo = &inputImage;
    vkUpdateDescriptorSets(device, 1, &imageWrite, 0, nullptr);
    imageWrite.dstSet = slot.outputSet;
    imageWrite.pImageInfo = &outputImage;
    vkUpdateDescriptorSets(device, 1, &imageWrite, 0, nullptr);
    // Glow-tap image for the scatter-quotient export, bound here (before
    // the first dispatch) with the other per-record image bindings. The
    // mid-record export then only rebinds the source buffers.
    if (ri.glowGainOutput.view != VK_NULL_HANDLE) {
        VkDescriptorImageInfo glowImage{};
        glowImage.imageView = ri.glowGainOutput.view;
        glowImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet glowWrite{};
        glowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        glowWrite.dstSet = slot.glowRatioSet;
        glowWrite.dstBinding = 2;
        glowWrite.descriptorCount = 1;
        glowWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        glowWrite.pImageInfo = &glowImage;
        vkUpdateDescriptorSets(device, 1, &glowWrite, 0, nullptr);
    }
    // Debug tap: visualize an intermediate buffer through the output image.
    // 1=source(float) 2=filmRaw(log) 3=filmDensity 4=static hanatos table
    // 5=frame constants (host-known content, control experiment).
    // Visualizer shader maps the value range visibly; see spectra_output_vis.comp.
    if (const char* tapText = std::getenv("SPEKTRA_FILM_DEBUG_TAP")) {
        const int tap = std::atoi(tapText);
        VkBuffer tapBuffer = slot.destination;
        if (tap == 1) {
            tapBuffer = slot.source;
        } else if (tap == 2) {
            tapBuffer = slot.filmRaw;
        } else if (tap == 3) {
            tapBuffer = slot.filmDensity;
        } else if (tap == 4) {
            tapBuffer = impl.statics[Impl::kHanatosResponse].buffer;
        } else if (tap == 5) {
            tapBuffer = slot.frameFloats;
        } else if (tap == 6) {
            tapBuffer = impl.statics[Impl::kInputToSrgb].buffer;
        } else if (tap == 7) {
            tapBuffer = impl.statics[Impl::kDecodeLuts].buffer;
        }
        impl.writeBufferSet(slot.outputSet, {{0, tapBuffer}});
    }

    // 0. Input image -> float buffer. Layouts stay GENERAL throughout
    // (caller contract, as with TonemapEngine), so plain memory barriers
    // cover visibility — no VkImage handles needed. The source access mask
    // covers both compute producers (preview demosaic) and transfer uploads
    // (still paths / test harnesses).
    VkMemoryBarrier inputReady{};
    inputReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    inputReady.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    inputReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &inputReady,
                         0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl.inputPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            impl.ioPipelineLayout, 0, 1, &slot.inputSet, 0, nullptr);
    vkCmdPushConstants(cmd, impl.ioPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(ioPush), ioPush);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
    computeBarrier();
    diagnosticTap("source_linear_srgb", slot.source);
    if (profileUpto <= 1) {
        return;
    }
    // Tile-memory path (memory-saving engines): per-tile chain into the
    // working-size arena with center accumulation. Full-frame records are
    // rejected on such engines (no full-frame effect scratch exists).
    // Compute tiling (full-frame engine) + boost has no readback split and
    // stays rejected; full-frame non-tiled + boost uses local reduction.
    if (ri.tilingMode == GpuRenderTilingMode::Tiled && !impl.tileMemorySaving &&
        ri.look.halationEnabled && ri.look.halationBoostEv > 0.0f) {
        throw std::invalid_argument(
            "spektrafilm: tiled + halation boost needs a tile-memory engine "
            "and boost milestone");
    }
    if (ri.tilingMode == GpuRenderTilingMode::Tiled && impl.tileMemorySaving) {
        impl.recordTiledMemory(ri, slot, w, h, pixelSizeUm, adaptFlags);
        return;
    }
    if (impl.tileMemorySaving) {
        throw std::invalid_argument(
            "spektrafilm: memory-saving engine needs Tiled records");
    }

    // 1-2. Exposure -> [halation] -> develop. Halation runs on the linear
    // raw before develop; develop reads halationLogRaw when any sub-path
    // ran, else filmRaw (descriptor state persists across records).
    // Exposure writes linear raw (op=1, upstream preExposureRawPath) when a
    // raw consumer follows, else log density (op=0).
    // Buffer presence folds into every path gate: still engines with
    // conditional scratch skip absent effects (caller recreates when the
    // gated flags change, so this only fires on programmer error — never
    // dispatch on a null binding).
    const bool sharedAllocated = slot.sharedS0 != VK_NULL_HANDLE &&
                                 slot.sharedS1 != VK_NULL_HANDLE &&
                                 slot.sharedS2 != VK_NULL_HANDLE;
    const bool halationAllocated = sharedAllocated &&
                                   slot.halationLogRaw != VK_NULL_HANDLE;
    const bool cameraDiffusionAllocated =
        slot.diffusionTemp != VK_NULL_HANDLE &&
        slot.cameraDiffusionRaw != VK_NULL_HANDLE;
    const bool printDiffusionAllocated =
        slot.diffusionTemp != VK_NULL_HANDLE &&
        slot.cameraDiffusionRaw != VK_NULL_HANDLE;
    const bool previewGrainAllocated =
        slot.grainDensity != VK_NULL_HANDLE;
    const bool prodGrainAllocated = slot.grainLayerA != VK_NULL_HANDLE &&
                                    slot.grainDensityB != VK_NULL_HANDLE &&
                                    slot.sharedS0 != VK_NULL_HANDLE;
    const bool scannerAllocated = sharedAllocated;
    const bool dirAllocated = sharedAllocated &&
                              slot.dirDensity != VK_NULL_HANDLE;
    const bool halationFeature =
        !processNegative && ri.look.halationEnabled && halationAllocated;
    const bool halationScatter =
        halationFeature && ri.look.scatterAmount > 0.0f &&
        ri.look.scatterScale > 0.0f;
    const bool halationBounce =
        halationFeature && ri.look.halationAmount > 0.0f &&
        ri.look.halationScale > 0.0f &&
        (ri.look.halationStrengthR > 0.0f ||
         ri.look.halationStrengthG > 0.0f ||
         ri.look.halationStrengthB > 0.0f);
    const bool halationBoost =
        halationFeature && ri.look.halationBoostEv > 0.0f;
    const bool halationPass =
        halationBoost || halationScatter || halationBounce;
    // Camera-diffusion solve (CPU): user params -> GPU components for the
    // sequence below. Upload alongside the frame arrays (same host-write
    // barrier covers the coherent mappings).
    const diffusion::Solve diffusionSolve =
        diffusion::solveCamera(ri.look, pixelSizeUm);
    const bool cameraDiffusionPath =
        !processNegative && diffusionSolve.info.componentCount > 0u &&
        !diffusionSolve.components.empty() && cameraDiffusionAllocated;
    if (cameraDiffusionPath) {
        std::memcpy(slot.mappedDiffusionInfo, &diffusionSolve.info,
                    sizeof(diffusion::Info));
        std::memcpy(slot.mappedDiffusionComponents,
                    diffusionSolve.components.data(),
                    diffusionSolve.components.size() * sizeof(diffusion::Component));
    }
    // Print-diffusion solve (enlarger path): same solver, separate buffers.
    // Upstream printLikeFinalPath = print (0) or process-negative (2).
    const diffusion::Solve printDiffusionSolve =
        diffusion::solvePrint(ri.look, pixelSizeUm);
    const bool printDiffusionPath =
        (ri.look.process == 0 || ri.look.process == 2) &&
        printDiffusionSolve.info.componentCount > 0u &&
        !printDiffusionSolve.components.empty() && printDiffusionAllocated;
    if (printDiffusionPath) {
        std::memcpy(slot.mappedPrintDiffusionInfo, &printDiffusionSolve.info,
                    sizeof(diffusion::Info));
        std::memcpy(slot.mappedPrintDiffusionComponents,
                    printDiffusionSolve.components.data(),
                    printDiffusionSolve.components.size() *
                        sizeof(diffusion::Component));
    }
    if (!processNegative) {
        dispatchAdapt(impl.exposurePipeline, slot.exposureSet,
                      (halationPass || cameraDiffusionPath) ? 1u : 0u);
    }
    if (!processNegative) diagnosticTap((halationPass || cameraDiffusionPath) ? "exposure_linear" : "exposure_log", slot.filmRaw);
    if (profileUpto <= 2) {
        return;
    }
    {
        enum : uint32_t {
            kHalOpBlurX = 1u,
            kHalOpBlurYStore = 2u,
            kHalOpBlurYAccumulate = 3u,
            kHalOpScatterResolve = 4u,
            kHalOpBounceResolveLog = 5u,
            kHalOpRawToLog = 6u,
            kHalOpBoostMax = 7u,
            kHalOpBoostReduceMax = 8u,
            kHalOpBoostApply = 9u,
            kHalSigmaScatterCore = 0u,
            kHalSigmaScatterTail = 1u,
            kHalSigmaBounce = 2u,
            kHalBoostChunkPixels = 256u
        };
        const auto dispatchHalation = [&](VkDescriptorSet set, uint32_t op,
                                          uint32_t sigmaMode,
                                          uint32_t component) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              impl.halationPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    impl.corePipelineLayout, 0, 1, &set, 0,
                                    nullptr);
            const uint32_t stepX = (w + groupsX - 1u) / groupsX;
            const uint32_t stepY = (h + groupsY - 1u) / groupsY;
            for (const TileRect& t : tiles) {
                push.op = op;
                push.pad1 = sigmaMode;
                push.pad2 = component;
                push.activeOriginX = t.ox;
                push.activeOriginY = t.oy;
                push.activeWidth = t.tw;
                push.activeHeight = t.th;
                vkCmdPushConstants(cmd, impl.corePipelineLayout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                                   &push);
                vkCmdDispatch(cmd, (t.tw + stepX - 1u) / stepX,
                              (t.th + stepY - 1u) / stepY, 1);
            }
            computeBarrier();
        };
        const auto dispatchHalation1D = [&](VkDescriptorSet set, uint32_t op,
                                            uint32_t value,
                                            uint32_t itemCount) {
            push.op = op;
            push.pad1 = value;
            push.pad2 = 0;
            push.activeOriginX = 0;
            push.activeOriginY = 0;
            push.activeWidth = w;
            push.activeHeight = h;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              impl.halationPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    impl.corePipelineLayout, 0, 1, &set, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, impl.corePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
            vkCmdDispatch(cmd, (std::max(itemCount, 1u) + 31u) / 32u, 1u, 1u);
            computeBarrier();
        };
        if (halationBoost) {
            const uint32_t chunkCount =
                (w * h + kHalBoostChunkPixels - 1u) / kHalBoostChunkPixels;
            dispatchHalation1D(slot.halationSets[10], kHalOpBoostMax,
                               kHalBoostChunkPixels, chunkCount);
            dispatchHalation1D(slot.halationSets[11], kHalOpBoostReduceMax,
                               chunkCount, 1u);
            dispatchHalation(slot.halationSets[12], kHalOpBoostApply, 0u, 0u);
        }
        // 1b. Camera diffusion sequence on the (possibly boosted) linear
        // raw. Without halation, RawToLog produces the develop input
        // (upstream diffusion set 1).
        if (cameraDiffusionPath) {
            rebindBuffer(slot.diffusionSets[0], 0,
                         halationBoost ? slot.halationBoostedRaw
                                       : slot.filmRaw);
            runDiffusionSequence(slot.diffusionSets[0],
                                 diffusionSolve.components);
            if (!halationPass) {
                dispatchDiffusion(slot.diffusionSets[1], kDifOpRawToLog, 0u);
            }
        }
        const VkBuffer halationScatterSrc =
            cameraDiffusionPath
                ? slot.cameraDiffusionRaw
                : (halationBoost ? slot.halationBoostedRaw : slot.filmRaw);
        if (halationScatter) {
            for (uint32_t s : {0u, 2u, 3u, 5u}) {
                rebindBuffer(slot.halationSets[s], 0, halationScatterSrc);
            }
            dispatchHalation(slot.halationSets[0], kHalOpBlurX,
                             kHalSigmaScatterCore, 0u);
            dispatchHalation(slot.halationSets[1], kHalOpBlurYStore,
                             kHalSigmaScatterCore, 0u);
            dispatchHalation(slot.halationSets[2], 0u, 0u, 0u);
            for (uint32_t c = 0; c < 3; ++c) {
                dispatchHalation(slot.halationSets[3], kHalOpBlurX,
                                 kHalSigmaScatterTail, c);
                dispatchHalation(slot.halationSets[4], kHalOpBlurYAccumulate,
                                 kHalSigmaScatterTail, c);
            }
            dispatchHalation(slot.halationSets[5], kHalOpScatterResolve, 0u,
                             0u);
        }
        const VkBuffer halationBounceSrc =
            halationScatter ? slot.sharedS1 : halationScatterSrc;
        if (halationBounce) {
            rebindBuffer(slot.halationSets[6], 0, halationBounceSrc);
            rebindBuffer(slot.halationSets[7], 0, halationBounceSrc);
            rebindBuffer(slot.halationSets[9], 0, halationBounceSrc);
            dispatchHalation(slot.halationSets[6], 0u, 0u, 0u);
            for (uint32_t b = 0; b < 3; ++b) {
                dispatchHalation(slot.halationSets[7], kHalOpBlurX,
                                 kHalSigmaBounce, b);
                dispatchHalation(slot.halationSets[8], kHalOpBlurYAccumulate,
                                 kHalSigmaBounce, b);
            }
            dispatchHalation(slot.halationSets[9], kHalOpBounceResolveLog, 0u,
                             0u);
        } else if (halationPass) {
            rebindBuffer(slot.halationSets[9], 0, halationBounceSrc);
            dispatchHalation(slot.halationSets[9], kHalOpRawToLog, 0u, 0u);
        }
        rebindBuffer(slot.developSet, 0,
                     (halationPass || cameraDiffusionPath)
                         ? slot.halationLogRaw
                         : slot.filmRaw);
    }
    // Optional glow-factor export (UltraHDR gain-map tap): per-pixel
    // post/pre scatter quotient. Both buffers hold film-linear raw
    // (spectral response x scene x folded exposure), so the stock-dependent
    // response and the exposure cancel: G ~= 1.0 + glow, agnostic of stock,
    // method, and tables. `pre` is the exposed linear raw (filmRaw is
    // linear exactly when a raw consumer follows, i.e. this gate); `post`
    // is the same linear buffer the bounce resolve consumes (scatter output
    // when scatter ran, else diffused/boosted/exposed raw). Must run before
    // develop/DIR/scanner overwrite the shared S0/S1/S2 arena.
    if (ri.glowGainOutput.view != VK_NULL_HANDLE) {
        if (processNegative || (!halationPass && !cameraDiffusionPath)) {
            throw std::invalid_argument(
                "spektrafilm: glow gain output requested without a linear scatter path");
        }
        const VkBuffer scatterSrc = cameraDiffusionPath
                                        ? slot.cameraDiffusionRaw
                                        : (halationBoost ? slot.halationBoostedRaw : slot.filmRaw);
        const VkBuffer linearSrc = halationScatter ? slot.sharedS1 : scatterSrc;
        const VkBuffer preSrc = slot.filmRaw;
        if (preSrc == VK_NULL_HANDLE || linearSrc == VK_NULL_HANDLE) {
            throw std::invalid_argument("spektrafilm: glow gain source missing (scratch mismatch?)");
        }
        if (!impl.glowRatioPipeline) {
            throw std::invalid_argument(
                "spektrafilm: glow gain export needs glowRatioSpirv at create");
        }
        // Last halation/diffusion dispatch ended with WRITE->READ, so both
        // sources are visible. The tap image was bound into the dedicated
        // ratio set at record start; only the source buffers are rebound here
        // (same buffer-rebind pattern as the core sets). outputSet is never
        // touched, so the final output path is identical with or without a tap.
        impl.writeBufferSet(slot.glowRatioSet, {{0, preSrc}, {1, linearSrc}});
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl.glowRatioPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl.glowRatioPipelineLayout, 0, 1,
                                &slot.glowRatioSet, 0, nullptr);
        const uint32_t ratioPush[8] = {w, h, 0u, 0u, 0u, 0u, w, h};
        vkCmdPushConstants(cmd, impl.glowRatioPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ratioPush),
                           ratioPush);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);
        // Full barrier: the export READ of linearSrc (sharedS1 when scatter
        // ran) must complete before DIR/scanner WRITE it, and the image WRITE
        // must be visible to the later gain-map read.
        VkMemoryBarrier glowBarrier{};
        glowBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        glowBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        glowBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &glowBarrier, 0, nullptr, 0,
                             nullptr);
    }
    if (!processNegative) diagnosticTap("pre_develop_log", (halationPass || cameraDiffusionPath) ? slot.halationLogRaw : slot.filmRaw);
    if (!processNegative) {
        dispatchAdapt(impl.developPipeline, slot.developSet, 0);
    }
    if (!processNegative) diagnosticTap("density_pre_dir", slot.filmDensity);
    if (profileUpto <= 3) {
        return;
    }

    // 2c. DIR couplers (skipped on ProcessNegative, upstream dirPath gate).
    // dirAllocated folds in conditional-scratch presence (stills without DIR
    // carry no buffers; caller recreates on amount crossing 0).
    const bool dirPath = !processNegative && ri.look.dirCouplersAmount > 0.0f && dirAllocated;
    const bool dirBlurPath =
        dirPath && ri.look.dirCouplersDiffusionUm > 0.0f;
    const bool dirTailPath =
        dirBlurPath && ri.look.dirCouplersDiffusionTailUm > 0.0f &&
        ri.look.dirCouplersDiffusionTailWeight > 0.0f;
    VkBuffer dirDensitySrc = slot.filmDensity;
    if (dirPath) {
        fillDirFloats(*impl.filmCurves, ri.look, pixelSizeUm,
                      slot.mappedDirFloats);
        fillDirCorrectedCurves(*impl.filmCurves, slot.mappedDirFloats,
                               slot.mappedDirCurves);
        dispatchCore(impl.dirPipeline, slot.dirSets[0], 0, groupsX, groupsY);
        if (dirBlurPath) {
            dispatchCore(impl.dirPipeline, slot.dirSets[1], 1, groupsX, groupsY);
            dispatchCore(impl.dirPipeline, slot.dirSets[2], 2, groupsX, groupsY);
        }
        if (dirTailPath) {
            rebindBuffer(slot.dirSets[3], 0, slot.sharedS2);
            dispatchCore(impl.dirPipeline, slot.dirSets[3], 3, groupsX, groupsY);
            for (uint32_t c = 0; c < 3; ++c) {
                dispatchCore(impl.dirPipeline, slot.dirSets[4], 4, groupsX,
                             groupsY, 1, c);
                dispatchCore(impl.dirPipeline, slot.dirSets[5], 5, groupsX,
                             groupsY, 1, c);
            }
        }
        rebindBuffer(slot.dirSets[6], 2,
                     dirTailPath ? slot.sharedS1
                                 : (dirBlurPath ? slot.sharedS2
                                                : slot.sharedS0));
        // Redevelop reads the develop input: log raw when a raw consumer
        // (halation/camera diffusion) flipped exposure to linear-raw mode,
        // else the film raw (upstream developInputBufferInfo).
        rebindBuffer(slot.dirSets[6], 0,
                     (halationPass || cameraDiffusionPath)
                         ? slot.halationLogRaw
                         : slot.filmRaw);
        dispatchCore(impl.dirPipeline, slot.dirSets[6], 6, groupsX, groupsY);
        rebindBuffer(slot.grainSet, 0, slot.dirDensity);
        dirDensitySrc = slot.dirDensity;
    } else {
        // Restore: descriptor state persists across records.
        rebindBuffer(slot.grainSet, 0, slot.filmDensity);
    }

    if (!processNegative) diagnosticTap("density_post_dir", dirDensitySrc);

    // 2b. Grain between develop and print (skipped on ProcessNegative,
    // upstream grainFeatureEnabled gate): preview single dispatch,
    // production 10-dispatch dye-cloud (ops 1..10), synthesis 10-dispatch
    // sampling (ops 11,2,3,4,5,6,12,8,9,13 on the production sets).
    const bool previewGrain = !processNegative && ri.look.grainEnabled &&
                                ri.look.grainModel == 0 &&
                                previewGrainAllocated;
    const bool productionGrain = !processNegative && ri.look.grainEnabled &&
                                 ri.look.grainModel == 1 &&
                                 prodGrainAllocated;
    const bool synthesisGrain = !processNegative && ri.look.grainEnabled &&
                                ri.look.grainModel == 2 &&
                                prodGrainAllocated;
    {
        const VkBuffer densitySrc = previewGrain  ? slot.grainDensity
                                    : productionGrain ? slot.grainDensityB
                                    : synthesisGrain  ? slot.grainDensityB
                                                      : dirDensitySrc;
        if (processNegative) {
            rebindBuffer(slot.finalSet, 0, slot.source);
            rebindBuffer(slot.printSets[0], 0, slot.source);
        } else {
            rebindBuffer(slot.finalSet, 0, densitySrc);
            rebindBuffer(slot.printSets[0], 0, densitySrc);
        }
    }
    if (previewGrain) {
        dispatchCore(impl.grainPipeline, slot.grainSet, 0, groupsX, groupsY, 1);
    }
    if (productionGrain) {
        // Production sets 0/3 read the pre-grain density: DIR output when
        // the DIR pass ran, else the plain developed density.
        rebindBuffer(slot.grainProdSets[0], 0, dirDensitySrc);
        rebindBuffer(slot.grainProdSets[3], 0, dirDensitySrc);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 1, groupsX,
                     groupsY, 9);
        // Sliced layer blur (ops 14/15): 3 of 9 components per dispatch,
        // exchanged via sharedS0 scratch. Bit-identical to legacy ops 2/3
        // (per-component math independent; barrier per dispatch).
        for (uint32_t layerBase = 0u; layerBase < 9u; layerBase += 3u) {
            dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 14, groupsX,
                         groupsY, 3, layerBase);
            dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 15, groupsX,
                         groupsY, 3, layerBase);
        }
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 4, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 5, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 6, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 7, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[1], 8, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[2], 9, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[3], 10, groupsX,
                     groupsY);
    }
    if (synthesisGrain) {
        // Synthesis sets 0/3 read the pre-grain density: DIR output when
        // the DIR pass ran, else the plain developed density.
        rebindBuffer(slot.grainProdSets[0], 0, dirDensitySrc);
        rebindBuffer(slot.grainProdSets[3], 0, dirDensitySrc);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 11, groupsX,
                     groupsY, 9);
        // Sliced layer blur, same as production path above.
        for (uint32_t layerBase = 0u; layerBase < 9u; layerBase += 3u) {
            dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 14, groupsX,
                         groupsY, 3, layerBase);
            dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 15, groupsX,
                         groupsY, 3, layerBase);
        }
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 4, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 5, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 6, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[0], 12, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[1], 8, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[2], 9, groupsX,
                     groupsY);
        dispatchCore(impl.grainPipeline, slot.grainProdSets[3], 13, groupsX,
                     groupsY);
    }

    // 3-4. PrintScan frame constants (1x1) + full, or the print-diffusion
    // split. ProcessNegative uses op4 PrintRawFromNegativeLight reading the
    // linear source (upstream timedPrintRawFromNegativeLight); print-like
    // finals (process 0/2) run print diffusion + glare. RCM disables all
    // scanner post (upstream !rcmOutput gate) and keeps timeline linear.
    const bool printLikeFinal = ri.look.process == 0 || ri.look.process == 2;
    const bool scanGlarePath = !rcmOutput && ri.look.scannerEnabled &&
                               printLikeFinal && ri.look.glarePercent > 0.0f;
    const bool scanBlurPath = !rcmOutput && ri.look.scannerEnabled &&
                              ri.look.scannerMtf50LpMm > 0.0f;
    const bool scanUnsharpPath = !rcmOutput && ri.look.scannerEnabled &&
                                 ri.look.scannerUnsharpRadiusUm > 0.0f &&
                                 ri.look.scannerUnsharpAmount > 0.0f;
    const bool scannerPath = scannerAllocated &&
                               (scanGlarePath || scanBlurPath ||
                                scanUnsharpPath);
    if (printDiffusionPath) {
        rebindBuffer(slot.printSets[1], 1,
                     scannerPath ? slot.sharedS0 : slot.destination);
        dispatchCore(impl.printScanPipeline, slot.printSets[0], 3, 1, 1);
        dispatchCore(impl.printScanPipeline, slot.printSets[0],
                     processNegative ? 4u : 1u, sweepGroupsX, sweepGroupsY);
        runDiffusionSequence(slot.diffusionSets[2],
                             printDiffusionSolve.components);
        dispatchCore(impl.printScanPipeline, slot.printSets[1], 2,
                     sweepGroupsX, sweepGroupsY, 1, scannerPath ? 1u : 0u);
    } else {
        rebindBuffer(slot.finalSet, 1,
                     scannerPath ? slot.sharedS0 : slot.destination);
        dispatchCore(impl.printScanPipeline, slot.finalSet, 3, 1, 1);
        dispatchCore(impl.printScanPipeline, slot.finalSet, 0, sweepGroupsX,
                     sweepGroupsY, 1, scannerPath ? 1u : 0u);
    }
    if (scannerPath) {
        // Shared mapping: scanA->S0, scanB->S1, scanC/glareA->S2.
        const VkBuffer postGlare = scanGlarePath ? slot.sharedS1 : slot.sharedS0;
        const VkBuffer postBlur = scanBlurPath ? slot.sharedS0 : postGlare;
        const VkBuffer unsharpBuf = scanBlurPath ? slot.sharedS1 : slot.sharedS0;
        if (scanGlarePath) {
            dispatchCore(impl.scannerPipeline, slot.scanSets[0], 0, groupsX,
                         groupsY);
            if (ri.look.glareBlur > 0.0f) {
                dispatchCore(impl.scannerPipeline, slot.scanSets[1], 1, groupsX,
                             groupsY);
                dispatchCore(impl.scannerPipeline, slot.scanSets[2], 2, groupsX,
                             groupsY);
            }
            dispatchCore(impl.scannerPipeline, slot.scanSets[3], 3, groupsX,
                         groupsY);
        }
        if (scanBlurPath) {
            rebindBuffer(slot.scanSets[4], 0, postGlare);
            dispatchCore(impl.scannerPipeline, slot.scanSets[4], 4, groupsX,
                         groupsY);
            dispatchCore(impl.scannerPipeline, slot.scanSets[5], 5, groupsX,
                         groupsY);
        }
        if (scanUnsharpPath) {
            rebindBuffer(slot.scanSets[6], 0, postBlur);
            dispatchCore(impl.scannerPipeline, slot.scanSets[6], 6, groupsX,
                         groupsY);
            rebindBuffer(slot.scanSets[7], 1, unsharpBuf);
            dispatchCore(impl.scannerPipeline, slot.scanSets[7], 7, groupsX,
                         groupsY);
        }
        rebindBuffer(slot.scanSets[8], 0, postBlur);
        rebindBuffer(slot.scanSets[8], 2,
                     scanUnsharpPath ? unsharpBuf : postBlur);
        dispatchCore(impl.scannerPipeline, slot.scanSets[8], 8, groupsX,
                     groupsY);
    }
    diagnosticTap("display_float", slot.destination);
    if (profileUpto <= 4) {
        return;
    }

    // 5. Float buffer -> output image (RGBA8 clamp-pack, or RGBA16F signal
    // passthrough for HDR/RCM). Caller transitions output afterwards (same
    // split as RawDevelopRecorder: compute-write -> fragment-read).
    // Dither flag is env-gated (SPEKTRA_FILM_OUTPUT_DITHER=1); default off so
    // reference parity holds bit-exact.
    VkMemoryBarrier outputReady{};
    outputReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    outputReady.srcAccessMask = 0;
    outputReady.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &outputReady,
                         0, nullptr, 0, nullptr);
    {
        const char* ditherText = std::getenv("SPEKTRA_FILM_OUTPUT_DITHER");
        ioPush[2] = (ditherText && (ditherText[0] == '1' || ditherText[0] == 't' ||
                                    ditherText[0] == 'y'))
                        ? 1u
                        : 0u;
    }
    VkPipeline outputPipeline = impl.outputPipeline;
    if (ri.output.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        if (!impl.outputHalfPipeline) {
            throw std::invalid_argument(
                "spektrafilm: RGBA16F output needs hdrOutputSpirv at create");
        }
        outputPipeline = impl.outputHalfPipeline;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, outputPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            impl.ioPipelineLayout, 0, 1, &slot.outputSet, 0, nullptr);
    vkCmdPushConstants(cmd, impl.ioPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(ioPush), ioPush);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
}

}  // namespace spektrafilm_native
