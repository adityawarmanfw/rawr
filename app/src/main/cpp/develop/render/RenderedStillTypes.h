#pragma once

#include <gainmap/GainmapCompute.h>
#include <spektrafilm/SpektraFilm.h>
#include <tonemap/TonemapEngine.h>

#include <array>
#include <cstdint>
#include <string>

#include "develop/highlight/LensShadingMapSnapshot.h"
#include "color/DcpProfile.h"
#include "tonemap/ColorRenderProfile.h"

struct AAssetManager;
struct AAsset;

namespace rawrcam::develop::rendered {
struct RenderedStillContext {
    color::DcpProfilePtr dcpProfile;
    float dcpWeight1 = 1.0f;
    uint64_t requestId = 0;
    uint64_t timestampNs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t presentationQuarterTurns = 0;
    std::array<float, 3> whiteBalanceRgb{1.0f, 1.0f, 1.0f};
    std::array<float, 9> cameraToWorkingColumnMajor{};
    tonemap::TonemapParams tonemapParams = tonemap::TonemapPresets::NeutralBaseline().params;
    // Film simulation for stills. When enabled (and the film engine builds),
    // the still renders through film at half resolution instead of tonemap.
    // sensorToLinearSrgb is row-major sensor->linear-sRGB (the film input
    // space); timeSec derives from timestampNs (grain frozen per shot).
    bool filmEnabled = false;
    bool rendererStrict = false;
    // Renderer owns the dependency key and keeps the post engine alive.
    bool reuseLinearResult = false;
    bool filmTiled = false;
    float lutStrength = 1.0f;
    void (*filmDiagnosticTap)(void*, VkCommandBuffer, const char*, VkBuffer, uint32_t, uint32_t) = nullptr;
    void* filmDiagnosticUserData = nullptr;
    bool replayBypassFcc = false;  // Diagnostic intervention, never set by normal captures.
    // Still FCC medians chroma/Y and rescales by the pixel's own Y. RT's
    // absolute chroma drags recovered highlights' (Y >> 1) chroma into dark
    // pixels at window edges as red bands; diagnostic replay can override it.
    bool normalizedFcc = true;
    // Edge-aware FCC average: bilateral sigma in Y units (0 = legacy uniform
    // 3x3). Production still default 0, wired from JpegCaptureContext.
    float fccEdgeSigma = 0.0f;
    // Luminance-preserving chroma bound: >0 keeps display-range RGB in [0,1]
    // and allows scene-linear HDR headroom above 1. 0 = legacy unbounded.
    float fccChromaBound = 0.0f;
    // Capture sharpening iterations on the demosaiced pre-WB image, before
    // post-demosaic (0 = off). The Renderer applies it to its full-resolution
    // demosaic cache instead and leaves this 0.
    uint32_t captureSharpenIterations = 0;
    // Axial-defringe strength, forwarded to PostDemosaicProcessor (0 = off).
    float defringeStrength = 0.0f;
    float defringeEdgeThreshold = 0.02f;
    float defringeLumaFloor = 0.08f;
    // Profiled wavelet denoise, forwarded to PostDemosaicProcessor pre-WB
    // (0 = off). Raw strength multiplier (validated 1..2, up to 8); detail is
    // the shadow-preservation fulcrum (higher preserves more texture).
    float denoiseStrength = 0.0f;
    float denoiseDetail = 1.0f;
    // Luma-band force (Y0 threshold scale, 0..1; shipped tuning 0.25) and
    // wavelet band count (1..7, shipped 7), forwarded to
    // PostDemosaicProcessor. Out-of-range values clamp at record time.
    float denoiseForceY = 0.25f;
    int denoiseMaxScale = 7;
    // Normalized-domain noise model (green): variance = a*x + b, resolved
    // from the frame SENSOR_NOISE_PROFILE with the demosaic normalization.
    float denoiseNoiseA = 0.0f;
    float denoiseNoiseB = 0.0f;
    // 1A multiframe sensor-clip OR evidence. Set by MfsrCaptureJob when
    // the reference RAW sensor bits were ORed into the clip mask; used only
    // for accurate logging/diagnostics (behavior already baked into the mask).
    bool multiframeSensorClipOrApplied = false;
    // True for any multiframe caller (derived or sensor-OR mask), so the
    // evidence log never misattributes a merged-RGB mask as sensor_cfa.
    bool clipFromMultiframe = false;
    spektrafilm_native::FilmLook filmLook{};
    std::array<float, 9> sensorToLinearSrgb{1, 0, 0, 0, 1, 0, 0, 0, 1};
    rawrcam::tonemap_integration::ColorRenderProfile colorRenderProfile =
        rawrcam::tonemap_integration::ColorRenderProfile::RawrBase;
    std::string importedLutProfileId;
    uint32_t fccSteps = 1;
    bool diagnosticsEnabled = false;
    std::array<uint32_t, 4> diagnosticRoi{};  // Zero extent means full-frame dump.
    bool highlightReconstructionEnabled = true;
    uint32_t highlightReconstructionMethod = 0;
    float highlightThreshold = 1.0f;
    float highlightCompression = 45.0f;
    uint32_t cfaPattern = 0;
    highlight::LensShadingMapSnapshot lensShading;
    // Optional geometric undistort applied in the WB pass (pre-WB camera-linear
    // RGB resample). Coefficients are full-pixel-array ActiveArea pixels.
    // Disabled by default; gated on calibration presence upstream.
    bool distortionCorrectionEnabled = false;
    // [fx, fy, cx, cy, s] from LENS_INTRINSIC_CALIBRATION.
    std::array<float, 5> lensIntrinsic{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    // [k1, k2, k3, p1, p2] from LENS_DISTORTION.
    std::array<float, 5> lensDistortion{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    bool hasLensCalibration = false;
    // Lens profile calibration (OpenCV rational) in place of the Camera2
    // model: lensIntrinsic/lensDistortion hold fx,fy,cx,cy / k1,k2,p1,p2,k3 in
    // this frame's pixels, plus k4..k6 and the output focal.
    bool rationalLensDistortion = false;
    std::array<float, 3> lensDistortionDenominator{0.0f, 0.0f, 0.0f};
    std::array<float, 2> lensOutputFocal{0.0f, 0.0f};
    // Lens profile mirror (geometry::kLensMirror* bits), after undistort.
    uint32_t lensMirror = 0;
    // UltraHDR gain map (GPU-direct, still-capture only). When enabled, the
    // still records a full-res recovery map (multi-channel RGB by default)
    // from the scene-linear HDR tap (post-demosaic, white-balanced *camera*
    // RGB) plus an optional film glow quotient (post/pre scatter ratio, so
    // halation/camera-diffusion glow carries HDR headroom with hue) against
    // the tonemap/film output (SDR base) in the same command buffer.
    // Disabled by default; preview never allocates map resources.
    bool ultraHdrEnabled = false;
    gainmap::GainmapParams gainmapParams{};
    // Calibrated camera->linear-sRGB (row-major) for the HDR input. The
    // gainmap default is AP1->sRGB, which only fits an AP1 working space;
    // coordinators must set this from the same calibrated matrix the tonemap
    // and film paths use, or every gain is systematically biased.
    std::array<float, 9> gainmapCstRowMajor{1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool hasGainmapCst = false;
};

struct RenderedStillCompletion {
    uint64_t requestId = 0;
    bool success = false;
    std::string error;
    // True when the shot rendered through film: tonemapMs then measures the
    // film record (timestamps wrap it), and EXIF labels it "Film".
    bool filmRendered = false;
    // Film RAM-gate/setup allocation rejection: no JPEG pixels are produced.
    // Capture persistence must substitute DNG. Other record failures retain
    // their existing handling.
    bool filmFallbackMemory = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t presentationQuarterTurns = 0;
    double totalMs = 0.0;
    double colorProcessingMs = 0.0;
    double highlightReconstructionMs = 0.0;
    double refinementMs = 0.0;
    double tonemapMs = 0.0;
    // Profiled wavelet denoise dispatch (timestamps 10->11) when enabled.
    double denoiseMs = 0.0;
    // GPU gain map dispatch (timestamps 8->9) when ultraHdrEnabled.
    double gainmapMs = 0.0;
    // Defringe plus the Inpaint Opposed tone tap (timestamps 16->17).
    double postTailMs = 0.0;
    double captureSharpenMs = 0.0;
    // Effective highlight stage for the timing report: 0 off, 1 colour
    // propagation, 2 Inpaint Opposed (timed inside colour processing).
    int highlightStage = 0;
    // HL was off in settings but UltraHDR turned it on (the gain map needs it).
    bool defringeEnabled = false;
    // Map content telemetry (computed on the CPU readback post-fence, so it
    // reports what the file will actually carry): max stored texel (0..1),
    // fraction of texels at >= 90% of the boost level, and mean. Answers
    // "did the boost fire?" per shot without GPU readback of the mask.
    float gainmapMaxStored = 0.0f;
    float gainmapBoostedFrac = 0.0f;
    float gainmapMeanStored = 0.0f;
    // Wall-clock attribution so EXIF parts sum to totalMs: pre-submit setup
    // (resource creation, LUT load, TonemapEngine/PostDemosaicProcessor and
    // per-capture SpektraFilm construction, command recording), submit→fence
    // gaps beyond the GPU timestamp ranges (queue contention/scheduling),
    // and post-fence readback (+ diagnostic dumps when enabled).
    double renderSetupMs = 0.0;
    // Part of renderSetupMs spent loading the LUT and building the tonemap /
    // post-demosaic engines (pipeline compile); ~0 when persistence reuses them.
    double engineBuildMs = 0.0;
    double queueGapsMs = 0.0;
    double readbackMs = 0.0;
    uint64_t pixelBytes = 0;
};

}  // namespace rawrcam::develop::rendered
