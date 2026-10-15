#pragma once
// spektrafilm: record-only Vulkan film-simulation stage (private-use spike).
//
// Mirrors the TonemapEngine contract: the caller owns the Vulkan device,
// command buffers, images, synchronization, and submission. record() only
// appends commands and performs no allocation, submit, or wait.
//
// Pipeline position: AFTER demosaic, INSTEAD of tonemap.
//   RawPreview (linear RGBA16F, half-res or quarter-res) -> SpektraFilm ->
//   swapchain / still JPEG path (RGBA8 sRGB).
// Never before demosaic: the simulation needs full RGB triples per pixel.
//
// Scope: core path — FilmExposure -> [Halation] -> CurveDevelop -> [DIR]
// -> [Grain] -> PrintScan -> [ScannerPost]. Bracketed stages are skipped by
// parameter contract (amount 0 / enabled false), not by shader edit.
// Film/paper stocks are fixed at create time; per-frame controls below.
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace spektrafilm_native {

struct VulkanContext {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    const VkAllocationCallbacks* allocator = nullptr;
};

// Scene-linear RGB input. Values >1.0 are valid (HDR highlights feed
// halation/grain paths in the full pipeline; core path clamps at output).
struct LinearRgbImageView {
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Display-ready output. SDR (default) packs Rec.709/sRGB transfer-encoded
// 0..1 to UNORM (clamped). HDR roles (PQ/HLG) and RCM timeline keep their
// signal when the view is RGBA16F (recommended: 8-bit PQ banding, RCM linear
// headroom/negatives are clipped by UNORM). Format is per-record.
struct SrgbImageView {
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Glow-factor export (UltraHDR gain-map tap). When view is set, record()
// writes the per-pixel post/pre scatter quotient (dimensionless) to this
// image via a dedicated ratio dispatch (no new math in the film chain).
// Both buffers hold film-linear raw (spectral response x scene x folded
// exposure), so the stock-dependent response and the exposure cancel:
// G ~= 1.0 + glow, with no per-stock scale to calibrate. Null view
// (default) disables the export; all existing callers are unaffected.
// Domain: quotient of film linear raws, NOT white-balanced camera RGB.
// Gain-map callers multiply their scene-linear HDR tap by
// 1 + (min(max(G,1),glowMax)-1) x glowStrength (see gainmap::GlowImageView):
// glow-free pixels keep the pure scene tap, so no color matrix applies here.
// Must be full-res RGBA16F GENERAL when set; dims must equal input dims.
struct GlowGainImageView {
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
};

// ColorSpace ids match upstream SpektraParameters.h (vendored).
struct FilmLook {
    int32_t film = 2;            // kodak_portra_400 (create-time fixed)
    int32_t paper = 4;           // kodak_portra_endura (create-time fixed)
    int32_t inputColorSpace = 15;   // LinearRec709
    int32_t outputColorSpace = 25;  // Rec709Gamma24
    // Spectral upsampling model. User-visible choice: it changes the final
    // look (measured cost is ~equal across methods on mobile; pick by eye).
    // Preview and still must use the same value (WYSIWYG): 0=Hanatos2025,
    // 1=Mallett2019, 2=Hanatos2026 (default). Create-time fixed like
    // film/paper.
    int32_t rgbToRawMethod = 2;
    // Workflow: 0=PrintSimulation (print the negative; correct for negative
    // stocks), 1=ScanNegative (present the developed film directly; correct
    // for positive/reversal stocks, which would otherwise print negative).
    // 2=ProcessNegative (develop the negative directly from scene light via
    // paper Hanatos tables; skips exposure/develop/DIR/grain/halation/diffusion
    // on the film path, mirrors upstream finalProcessNegative).
    // Per-frame settable; paper Hanatos tables rebuild when filtration or
    // printTiming change (see updateProcessNegativeTables()).
    int32_t process = 0;
    // Print timing: 0=FilteredEnlarger (TH-KG3 + CMY filtration), 1=APD
    // (Academy printer density). Per-frame settable; APD requires generated
    // academy data (kSpektraAcademyPrinterDensityEnabled) else record rejects.
    int32_t printTiming = 0;
    // ScanNegative only: normalize against clear/D-max references and invert
    // negative stocks for display-style viewing. Leave false for positive
    // stocks (already positive); true for negative stocks in scan mode.
    // Per-frame settable.
    bool scanNegativeInvert = false;
    float filmExposureEv = 0.0f;    // per-frame settable, ±10
    float printExposureEv = 0.0f;   // per-frame settable, ±10
    // Auto-exposure (upstream RenderParams autoExposure/autoExposureMethod).
    // record() does not meter or apply this flag itself. Offline callers may
    // meter CPU pixels and resolve filmExposureEv before recording. The live
    // camera app uses the frame's AE post-gain for both preview and stills to
    // preserve camera exposure compensation. Method 0=center-weighted
    // gaussian sigma 0.2, 1=median.
    bool autoExposure = false;        // per-frame
    int32_t autoExposureMethod = 0;   // per-frame, 0..1
    // Cap on positive per-frame exposure lifts applied by the app record
    // sites (AE post-gain fold): past ~+1 EV the
    // negative shoulder rotates hues measurably (fire-frame replay: flame
    // red-chroma 0.52 at EV 0 -> ~0.46 by +1.5, 0.41 by +2.7 with DIR
    // amplifying), so lifts clamp here and dark scenes render darker-but-
    // honest instead of bright-but-broken. Darkening (negative) placements
    // pass through; callers still clamp the final sum to +-10 EV. Preview
    // and stills apply the same cap (WYSIWYG).
    static constexpr float kMaxFilmLiftEv = 1.0f;
    // Film development. Mode 0=Standard, 1=Experimental. Stops ±2.
    int32_t filmPushPullMode = 0;   // per-frame settable
    float filmPushPullStops = 0.0f;  // per-frame settable
    float filmGamma = 1.0f;          // per-frame settable, >0
    // Print development and paper shaping. Push/pull stops ±2, gamma >0.
    float printPushPullStops = 0.0f;  // per-frame settable
    float printGamma = 1.0f;          // per-frame settable
    float printShadowShape = 0.0f;    // per-frame settable
    float printHighlightShape = 0.0f;  // per-frame settable
    // Retained-silver looks. Negative bleach >0 leaves the shader's fast
    // spectral path (costs more); 0 disables. Leuco coupling typically 1.
    float negativeBleachBypassAmount = 0.0f;  // per-frame, >=0
    float negativeLeucoCyanCoupling = 1.0f;   // per-frame, finite
    float printBleachBypassAmount = 0.0f;     // per-frame, >=0
    // Preflash (fogging exposure) and enlarger CMY filtration. All live
    // per-frame: on the PrintSimulation path the shaders consume these from
    // the frame constants only (verified against SpektraPrintScan.comp; the
    // baked paper-weight tables serve the ProcessNegative path, which is
    // out of scope).
    float preflashExposure = 0.0f;      // per-frame, >=0
    float preflashMFilterShift = 0.0f;  // per-frame
    float preflashYFilterShift = 0.0f;  // per-frame
    float filterC = 0.0f;           // per-frame
    float filterMShift = 0.0f;      // per-frame
    float filterYShift = 0.0f;      // per-frame
    // DIR couplers (development inhibitor releasing): interlayer interimage
    // effects on the developed densities. All live per-frame; amount 0
    // disables the DIR pass entirely (upstream default). The 12 gammas are
    // physical calibration (upstream defaults); exposed for completeness.
    float dirCouplersAmount = 0.0f;              // per-frame, UI 0..1; older values clamp to 1
    float dirCouplersDiffusionUm = 20.0f;        // per-frame, >=0
    float dirCouplersDiffusionTailUm = 200.0f;   // per-frame, >=0
    float dirCouplersDiffusionTailWeight = 0.06f;  // per-frame, 0..1
    float dirCouplersInhibitionSameLayer = 1.0f;   // per-frame, >=0
    float dirCouplersInhibitionInterlayer = 1.0f;  // per-frame, >=0
    float dirCouplersGammaSameLayerR = 0.336f;   // per-frame
    float dirCouplersGammaSameLayerG = 0.319f;   // per-frame
    float dirCouplersGammaSameLayerB = 0.273f;   // per-frame
    float dirCouplersGammaRToG = 0.353f;         // per-frame
    float dirCouplersGammaRToB = 0.302f;         // per-frame
    float dirCouplersGammaGToR = 0.154f;         // per-frame
    float dirCouplersGammaGToB = 0.353f;         // per-frame
    float dirCouplersGammaBToR = 0.168f;        // per-frame
    float dirCouplersGammaBToG = 0.226f;        // per-frame
    // Halation (emulsion scatter + base bounce + highlight boost) on the
    // linear raw between exposure and develop. All live per-frame; disabled
    // unless halationEnabled with at least one of scatter/bounce/boost
    // active (upstream default off). Strengths default to the OFX values;
    // when left at default the per-stock preset (antihalation tag) wins.
    // First-sigma is always stock-driven when the profile carries it.
    bool halationEnabled = false;            // per-frame
    float scatterAmount = 1.0f;              // per-frame, >=0
    float scatterScale = 1.0f;               // per-frame, >=0
    float halationAmount = 1.0f;             // per-frame, >=0
    float halationScale = 1.0f;              // per-frame, >=0
    float halationStrengthR = 0.05f;         // per-frame, >=0
    float halationStrengthG = 0.015f;        // per-frame, >=0
    float halationStrengthB = 0.0f;          // per-frame, >=0
    float halationFirstSigmaUmR = 65.0f;     // per-frame, >0
    float halationFirstSigmaUmG = 65.0f;     // per-frame, >0
    float halationFirstSigmaUmB = 65.0f;     // per-frame, >0
    float halationBoostEv = 0.0f;            // per-frame, >=0
    float halationBoostRange = 0.3f;         // per-frame, 0..1
    float halationProtectEv = 4.0f;          // per-frame, >=0
    // Camera diffusion (lens-filter glow: core + halo + bloom Gaussian
    // mixture) on the linear raw after boost, before halation scatter.
    // All live per-frame; disabled unless enabled with strength/scale > 0
    // and the solver yields components (upstream default off). Family:
    // 0=Glimmerglass, 1=BlackProMist, 2=ProMist, 3=CineBloom.
    bool cameraDiffusionEnabled = false;       // per-frame
    int32_t cameraDiffusionFamily = 1;         // per-frame, 0..3
    float cameraDiffusionStrength = 0.5f;      // per-frame, >=0
    float cameraDiffusionSpatialScale = 1.0f;  // per-frame, >=0
    float cameraDiffusionHaloWarmth = 0.0f;    // per-frame
    float cameraDiffusionCoreIntensity = 1.0f;  // per-frame, >=0
    float cameraDiffusionCoreSize = 1.0f;       // per-frame, >0
    float cameraDiffusionHaloIntensity = 1.0f;  // per-frame, >=0
    float cameraDiffusionHaloSize = 1.0f;       // per-frame, >0
    float cameraDiffusionBloomIntensity = 1.0f;  // per-frame, >=0
    float cameraDiffusionBloomSize = 1.0f;       // per-frame, >0
    // Print diffusion (same filter model in the enlarger path): applied to
    // the print raw between the two PrintScan halves. Mirrors the camera
    // fields; only runs on the print workflow (process 0).
    bool printDiffusionEnabled = false;       // per-frame
    int32_t printDiffusionFamily = 1;         // per-frame, 0..3
    float printDiffusionStrength = 0.5f;      // per-frame, >=0
    float printDiffusionSpatialScale = 1.0f;  // per-frame, >=0
    float printDiffusionHaloWarmth = 0.0f;    // per-frame
    float printDiffusionCoreIntensity = 1.0f;  // per-frame, >=0
    float printDiffusionCoreSize = 1.0f;       // per-frame, >0
    float printDiffusionHaloIntensity = 1.0f;  // per-frame, >=0
    float printDiffusionHaloSize = 1.0f;       // per-frame, >0
    float printDiffusionBloomIntensity = 1.0f;  // per-frame, >=0
    float printDiffusionBloomSize = 1.0f;       // per-frame, >0
    // Scanner post (print/scan finishing): glare, MTF blur, unsharp mask.
    // All live per-frame; scannerEnabled false (upstream default) skips the
    // pass entirely. Glare applies to print finals; MTF/unsharp run on both
    // print and scan finals.
    bool scannerEnabled = false;              // per-frame
    bool scannerWhiteCorrection = false;      // per-frame
    bool scannerBlackCorrection = false;      // per-frame
    float scannerWhiteLevel = 0.98f;          // per-frame
    float scannerBlackLevel = 0.01f;          // per-frame
    float glarePercent = 0.03f;               // per-frame, >=0
    float glareRoughness = 0.7f;              // per-frame
    float glareBlur = 0.5f;                   // per-frame, px sigma >=0
    float scannerMtf50LpMm = 60.0f;           // per-frame, >0 runs MTF blur
    float scannerUnsharpRadiusUm = 5.0f;      // per-frame, >=0
    float scannerUnsharpAmount = 0.7f;        // per-frame, >=0
    // Camera-plane filtration (UV/IR cut). Bakes into the film spectral
    // tables like enlarger filtration; use updateCameraFilters() debounced.
    bool cameraUvFilterEnabled = false;  // baked
    float cameraUvCutNm = 410.0f;        // baked
    bool cameraIrFilterEnabled = false;  // baked
    float cameraIrCutNm = 675.0f;        // baked
    // Printer-light balance (additive points) + gang/calibration switches.
    float printerLightsR = 0.0f;  // per-frame
    float printerLightsG = 0.0f;  // per-frame
    float printerLightsB = 0.0f;  // per-frame
    bool printerLightsGang = false;       // per-frame
    bool printerLightCalibration = true;  // per-frame
    // Enlarger geometry. Scale clamps to [1,32] like the reference.
    float enlargerScale = 1.0f;           // per-frame
    float enlargerOffsetXPercent = 0.0f;  // per-frame
    float enlargerOffsetYPercent = 0.0f;  // per-frame
    // Grain (preview/production/synthesis models). Model 0 (Preview) is a
    // single closed-form dispatch; model 1 (Production) is the 10-dispatch
    // dye-cloud path (9-deep layer ops + microstructure + blurs); model 2
    // (Synthesis) is the 10-dispatch sampling path (ops 11,2,3,4,5,6,12,8,9,13
    // on the production descriptor sets, upstream kGrainOp*). Tiled rendering
    // rejects synthesis (needs full-frame pre-grain milestone).
    // Film format selects capture gate size (pixel pitch source).
    bool grainEnabled = false;      // per-frame
    int32_t grainModel = 0;         // 0 Preview, 1 Production, 2 Synthesis
    int32_t filmFormat = 4;         // 0..7 (Standard8..Imax70), per-frame
    float grainAmount = 1.0f;       // per-frame, >= 0
    float grainSaturation = 1.0f;   // per-frame, finite (shader clamps 0..1)
    bool grainSublayersEnabled = true;  // per-frame
    int32_t grainSubLayerCount = 1;     // per-frame, >= 1
    float grainParticleAreaUm2 = 0.1f;  // per-frame, > 0
    float grainParticleScaleR = 1.2f;   // per-frame, > 0
    float grainParticleScaleG = 1.0f;   // per-frame, > 0
    float grainParticleScaleB = 2.5f;   // per-frame, > 0
    float grainParticleScaleLayer0 = 6.0f;  // per-frame
    float grainParticleScaleLayer1 = 1.0f;  // per-frame
    float grainParticleScaleLayer2 = 0.4f;  // per-frame
    float grainDensityMinR = 0.04f;  // per-frame
    float grainDensityMinG = 0.05f;  // per-frame
    float grainDensityMinB = 0.06f;  // per-frame
    float grainUniformityR = 0.99f;  // per-frame
    float grainUniformityG = 0.97f;  // per-frame
    float grainUniformityB = 0.98f;  // per-frame
    float grainFinalBlurUm = 11.8f;      // per-frame
    float grainBlurDyeCloudsUm = 1.0f;   // per-frame
    float grainMicroStructureScale = 0.2f;    // filled (production path)
    float grainMicroStructureSigmaNm = 30.0f;  // filled (production path)
    uint32_t grainSeed = 1u;          // per-frame
    bool grainAnimate = false;        // per-frame; seed evolves with timeSec
    // Grain synthesis sampling params (upstream RenderParams grainSynthesis*,
    // SpektraParameters.h:225-243). Only read when grainModel==2.
    float grainSynthesisSize = 1.0f;              // per-frame, 0.25..4
    float grainSynthesisAmount = 1.0f;            // per-frame, 0..3
    float grainSynthesisSharpness = 1.0f;         // per-frame, >=0.25 effective
    float grainSynthesisQuality = 1.0f;           // per-frame, 0.25..4
    int32_t grainSynthesisSamples = 128;          // per-frame, 1..1024 (x quality)
    float grainSynthesisMeanRadiusUm = 0.25f;     // per-frame, finite
    float grainSynthesisRadiusStdDevRatio = 0.0f;  // per-frame, finite
    float grainSynthesisObservationSigmaUm = 1.0f;  // per-frame, finite
    float grainSynthesisCellSizeRatio = 1.0f;     // per-frame, finite
    float grainSynthesisMaxRadiusQuantile = 0.999f;  // per-frame, finite
    float grainSynthesisCoverageEpsilon = 0.0001f;  // per-frame, finite
    int32_t grainSynthesisMaxGrainsPerCell = 32;  // per-frame, 1..128
    float grainSynthesisRadiusScaleR = 1.2f;      // per-frame, finite
    float grainSynthesisRadiusScaleG = 1.0f;      // per-frame, finite
    float grainSynthesisRadiusScaleB = 2.5f;      // per-frame, finite
    float grainSynthesisLayerScale0 = 6.0f;       // per-frame, finite
    float grainSynthesisLayerScale1 = 1.0f;       // per-frame, finite
    float grainSynthesisLayerScale2 = 0.4f;       // per-frame, finite
    bool grainSynthesisLayered = true;            // per-frame
    // Output role + HDR (upstream OutputRole/HdrTransfer/HdrToneMapping).
    // 0=DisplaySdr (RGBA8), 1=DisplayHdr (PQ/HLG 0..1 signal, still packed to
    // the caller's RGBA8 image; half/float output is a follow-up), 2=RCM
    // timeline (linear, skips scanner + black/white correction).
    // Per-frame settable; outputColorSpace stays baked-equality.
    int32_t outputRole = 0;                 // per-frame, 0..2
    int32_t hdrTransfer = 0;                // per-frame, 0=PQ, 1=HLG
    int32_t hdrToneMapping = 1;             // per-frame, 0=SoftRolloff, 1=HardClip
    float hdrReferenceWhiteNits = 203.0f;   // per-frame, >=1
    float hdrPeakNits = 1000.0f;            // per-frame, >refWhite
    float hdrExposureEv = 0.0f;             // per-frame, finite
    // Color adaptation (upstream colorAdaptationFlags). Master off forces 0;
    // SDR output gamut (OKLch lightness/chroma) compress only runs when the
    // corresponding bits are set; input compression selects the second half
    // of the Hanatos pairs; curve smoothing switches film/paper interp to
    // Hermite. Per-frame settable.
    bool colorAdaptation = false;                        // per-frame
    bool colorAdaptationInputCompression = true;         // per-frame
    bool colorAdaptationCurveSmoothing = true;           // per-frame
    bool colorAdaptationOutputLightnessCompression = true;  // per-frame
    bool colorAdaptationOutputChromaCompression = true;     // per-frame
};

// Halation-boost milestone (tiled two-phase): (maxRaw, rawX0, a, k), same
// layout as the Halation op8 output. Phase 1 (recordBoostMilestone) derives
// it on GPU; the caller submits, waits, reads it back, and feeds it to
// phase 2 via SpektraFilmRecordInfo::boostInfo.
struct SpektraFilmBoostMilestone {
    float values[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};

namespace tables {
// Auto-exposure metering (upstream measureAutoExposureEv 2137-2211, CPU
// port). The reference meters the GPU-resident decoded input; this variant
// meters a CPU Bayer snapshot with identical math (256px long edge,
// Rec.709-luma fallback weights, median vs center-weighted gaussian
// sigma 0.2, EV = -log2(Y/0.184), 0 on degenerate input). Returns the EV to
// ADD to filmExposureEv (NOT clamped; caller clamps to its EV range).
struct BayerMeterInput {
    const uint16_t* pixels = nullptr;  // Bayer mosaic, row-major uint16
    uint32_t width = 0;                // pixels
    uint32_t height = 0;
    uint32_t rowStridePixels = 0;  // uint16 stride (0 == width)
    int32_t cfaPattern = 0;        // 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR
    float blackRggb[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // per-channel black, DN
    float whiteLevel = 1023.0f;                     // saturation, DN
    float wbRggb[4] = {1.0f, 1.0f, 1.0f, 1.0f};     // white-balance gains
};
float autoExposureEvFromBayer(const BayerMeterInput& in,
                              int32_t method) noexcept;
}  // namespace tables

// Compute tiling mode (upstream GpuRenderTilingMode). LegacyFullFrame issues
// one dispatch per pass over the full image. Tiled splits every full-res
// 2D pass into tileW x tileH compute tiles via the shader activeRect
// (bit-identical to full-frame: transients stay full-frame, so halo reads
// come from completed previous passes; the per-look tile overlap sizes the
// follow-up tile-sized arenas).
enum class GpuRenderTilingMode : int32_t {
    LegacyFullFrame = 0,
    Tiled = 1,
};

// Camera-plane UV/IR cut baked into the film spectral tables.
struct CameraFilters {
    bool uvEnabled = false;
    float uvCutNm = 410.0f;
    bool irEnabled = false;
    float irCutNm = 675.0f;
};

struct SpektraFilmCreateInfo {
    VulkanContext context{};
    // Reserved for future device-local transient uploads; record() never
    // submits. Static tables are host-visible (TonemapEngine pattern).
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    uint32_t maxFramesInFlight = 3;
    uint32_t maxWidth = 1024;   // transient arena sizing (quarter-res ceiling)
    uint32_t maxHeight = 768;
    FilmLook look{};  // film/paper baked into static tables at create time
    // Still engines: allocate effect scratch only for effects enabled in
    // `look` (halation/diffusion/print/scanner/production-grain buffers are
    // ~6.6GB at full-res and must not be reserved when off). The caller must
    // recreate when those enable flags change (record() only runs effects
    // whose buffers exist). Preview engines leave false (quarter-res arena
    // is cheap; instant toggling).
    bool conditionalEffectScratch = false;
    // Diagnostic-only transfer sources; off in ordinary application renders.
    bool diagnosticTransfers = false;
    // Tiled memory saving (stills): effect scratch is allocated at tile
    // working size ((maxTile + 2*overlap), overlap from baked allocation
    // gates) instead of full-frame; only slot.source/destination stay
    // full-frame. Tiled records then run the chain per tile into tile scratch
    // and accumulate centers to the full-frame destination. Full-frame
    // (LegacyFullFrame) records are rejected on such engines. Tiled records
    // on non-memory-saving engines use compute tiling on full-frame buffers.
    // The caller must recreate when baked gates change such that the
    // per-record overlap exceeds the arena overlap (record() rejects loudly):
    // effect enables + grain model + DIR amount crossing 0.
    bool tiledMemorySaving = false;
    uint32_t maxTileWidth = 512u;   // 64..8192 (tile arena sizing)
    uint32_t maxTileHeight = 256u;  // 64..8192
    // SPIR-V inputs (compiled at build time by CMake, like raw_preview).
    const uint32_t* inputSpirv = nullptr;  // spectra_input.comp
    size_t inputSpirvBytes = 0;
    const uint32_t* exposureSpirv = nullptr;
    size_t exposureSpirvBytes = 0;
    const uint32_t* developSpirv = nullptr;
    size_t developSpirvBytes = 0;
    const uint32_t* printScanSpirv = nullptr;
    size_t printScanSpirvBytes = 0;
    const uint32_t* outputSpirv = nullptr;
    size_t outputSpirvBytes = 0;
    // Optional half-float output shader (spektra_output_half.comp): selected
    // per record when the output view is RGBA16F. Null disables half output
    // (half records are rejected); the RGBA8 path is unaffected.
    const uint32_t* hdrOutputSpirv = nullptr;
    size_t hdrOutputSpirvBytes = 0;
    // Optional boost-milestone shader (spektra_boost_milestone.comp): required
    // for recordBoostMilestone(); RGBA8/full paths are unaffected when null.
    const uint32_t* boostMilestoneSpirv = nullptr;
    size_t boostMilestoneSpirvBytes = 0;
    // Optional glow-ratio shader (spektra_glow_ratio.comp): required when a
    // record sets glowGainOutput; null disables glow export (records with a
    // glow view are rejected). Ordinary film/RGBA8/full paths are unaffected.
    const uint32_t* glowRatioSpirv = nullptr;
    size_t glowRatioSpirvBytes = 0;
    const uint32_t* grainSpirv = nullptr;  // SpektraGrain.comp
    size_t grainSpirvBytes = 0;
    const uint32_t* dirSpirv = nullptr;  // SpektraDir.comp
    size_t dirSpirvBytes = 0;
    const uint32_t* halationSpirv = nullptr;  // SpektraHalation.comp
    size_t halationSpirvBytes = 0;
    const uint32_t* diffusionSpirv = nullptr;  // SpektraDiffusion.comp
    size_t diffusionSpirvBytes = 0;
    const uint32_t* scannerSpirv = nullptr;  // SpektraScannerPost.comp
    size_t scannerSpirvBytes = 0;
    // Raw float blobs (assets/*.f32, build-time generated).
    const float* hanatosSpectra = nullptr;  // W*H*wavelengthCount floats
    size_t hanatosSpectraFloats = 0;
    const float* gamutCompression = nullptr;  // 26*18 floats
    size_t gamutCompressionFloats = 0;
};

struct SpektraFilmRecordInfo {
    // Render-side adaptive luminance curve: lift/key/highlight EV, baseline HAL gain.
    // Independent of the film look and its creative exposure; zero lift is identity.
    std::array<float, 4> adaptiveTone{0.0f, -4.0f, 0.0f, 1.0f};
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    LinearRgbImageView input{};
    SrgbImageView output{};
    // Optional glow-factor export (see GlowGainImageView). Must be full-res
    // RGBA16F GENERAL when set; dims must equal input dims.
    GlowGainImageView glowGainOutput{};
    uint32_t frameSlot = 0;
    // Frame time in seconds. Drives the animated grain seed
    // (floor(time*24+0.5) when grainAnimate); stills reuse their preview
    // frame's timestamp for an exact match.
    double timeSec = 0.0;
    // Look overrides applied to this frame. Stock/method/colorspace fields
    // must equal the baked look (recreate to change); all other FilmLook
    // fields are live per frame except filterC/M/YShift (Tier 2).
    FilmLook look{};
    // Row-major 3x3 sensor->linear-sRGB (Rec.709 primaries) applied to the
    // input image on load. The film model declares LinearRec709 input, so a
    // caller whose buffer is sensor-native must pass its color matrix here
    // (identity = buffer already linear sRGB). Live per frame.
    float sensorToLinearSrgb[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                   0.0f, 0.0f, 0.0f, 1.0f};
    // Compute tiling request (per-record; mirrors upstream RenderParams
    // gpuRenderTiling). LegacyFullFrame ignores tileW/H. Tiled splits
    // full-res passes into tileW x tileH tiles (64..8192, clamped to the
    // frame). Synthesis grain still needs a full-frame record. Tiled boost
    // needs a tile-memory engine and a completed recordBoostMilestone().
    GpuRenderTilingMode tilingMode = GpuRenderTilingMode::LegacyFullFrame;
    uint32_t tileWidth = 512u;
    uint32_t tileHeight = 256u;
    // Chunked submits (watchdog safety): a tiled record may cover a tile
    // sub-range [tileFirst, tileFirst + tileCount) in row-major tile order
    // instead of the whole grid. Chunks are independent (disjoint centers,
    // shared read-only inputs) but must ALL be submitted+waited, in any
    // order, before the output is consumed. tileFinalize=false skips the
    // output-image dispatch (only the last chunk needs it; the dispatch is
    // idempotent so leaving it true everywhere is also correct, just slower).
    // Defaults cover the whole grid with finalize: single-submit behavior.
    uint32_t tileFirst = 0u;
    uint32_t tileCount = 0xFFFFFFFFu;
    bool tileFinalize = true;
    // Optional hook run after every tiled pass (after its barrier). The caller
    // may submit what is recorded so far and re-begin the same command buffer
    // before returning, bounding one GPU submission well below a whole tile
    // so another queue (e.g. a live preview) is not starved. Tiled mode only.
    void (*passBoundary)(void*) = nullptr;
    void* passBoundaryUserData = nullptr;
    // Optional full-frame-path GPU buffer tap. Callback records transfer copies
    // only; the caller owns destinations until the submitted work completes.
    // Requires createInfo.diagnosticTransfers; buffers contain row-major float4.
    void (*diagnosticTap)(void*, VkCommandBuffer, const char*, VkBuffer, uint32_t, uint32_t) = nullptr;
    void* diagnosticUserData = nullptr;
    // Tiled boost milestone (phase 2): set from readBoostMilestone() after a
    // phase-1 submit+wait. Tiled + boost records reject without it.
    bool hasBoostInfo = false;
    float boostInfo[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};

class SpektraFilm {
   public:
    static bool validateCreateInfo(const SpektraFilmCreateInfo& createInfo,
                                   const char** reason = nullptr) noexcept;
    static bool validateRecordInfo(const SpektraFilmRecordInfo& recordInfo,
                                   uint32_t maxFramesInFlight,
                                   uint32_t maxWidth, uint32_t maxHeight,
                                   const char** reason = nullptr) noexcept;

    explicit SpektraFilm(const SpektraFilmCreateInfo& createInfo);
    ~SpektraFilm();
    SpektraFilm(const SpektraFilm&) = delete;
    SpektraFilm& operator=(const SpektraFilm&) = delete;
    SpektraFilm(SpektraFilm&&) = delete;
    SpektraFilm& operator=(SpektraFilm&&) = delete;

    // Records only. Images must be in VK_IMAGE_LAYOUT_GENERAL. Input is
    // consumed via copyImageToBuffer; output is left in GENERAL with film
    // data written (caller transitions to presentation, as with tonemap).
    void record(const SpektraFilmRecordInfo& recordInfo);

    // Boost milestone phase 1 (tiled + halation boost only): records input
    // copy, linear-raw exposure per tile, per-tile atomic chunk maxima, and
    // the BoostReduceMax into host-visible readback. The caller submits,
    // waits for completion, then calls readBoostMilestone(slot) and feeds
    // the values to phase 2 via RecordInfo::boostInfo. Rejects when the
    // engine lacks tile scratch, the look has no boost, tiling is off, or
    // the milestone shader was not provided at create.
    void recordBoostMilestone(const SpektraFilmRecordInfo& recordInfo);

    // Copies the 16B milestone readback for frameSlot (call only after the
    // phase-1 submit completed; memory is host-coherent).
    SpektraFilmBoostMilestone readBoostMilestone(uint32_t frameSlot) const;

    // Pure-CPU boost-info derivation (mirrors Halation op8): lets callers
    // inspect/clamp the milestone or synthesize it in tests.
    static SpektraFilmBoostMilestone computeBoostInfo(float globalMaxRaw,
                                                      float protectEv,
                                                      float range,
                                                      float boostEv) noexcept;

    // Rebuilds film spectral tables for new camera filtration (CPU
    // seconds-scale; call debounced, e.g. on slider release) and re-uploads
    // them. Must be called while the GPU is idle on prior spektra work
    // (after queue wait-idle for in-flight spektra frames). Afterwards
    // record() accepts looks carrying these exact values; anything else is
    // rejected.
    void updateCameraFilters(const CameraFilters& filters);

    // Rebuilds paper Hanatos pairs for live ProcessNegative filtration /
    // printTiming (CPU seconds-scale; call debounced while GPU idle, same
    // contract as updateCameraFilters). The baked look's film/paper are kept;
    // only filtration + timing + preflashExposure are taken from `look`.
    void updateProcessNegativeTables(const FilmLook& look);

    // CPU-side query: would record() write glowGainOutput for this look and
    // geometry? Mirrors the record() export gate (process mode, halation
    // sub-paths, camera-diffusion solve) so callers can allocate the transient
    // and wire the gain-map glow input before recording. Assumes the engine
    // was built for this look (conditional-scratch match, as stills ensure);
    // record() still verifies buffers and throws on mismatch. V1 supports
    // full-frame records only (tile-memory engines and Tiled tiling return
    // false: caller falls back to the pure scene tap).
    static bool willWriteGlowGain(const FilmLook& look, uint32_t width, uint32_t height,
                                  GpuRenderTilingMode tilingMode, bool tileMemorySaving) noexcept;

    CameraFilters bakedCameraFilters() const noexcept;

    uint32_t maxFramesInFlight() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spektrafilm_native
