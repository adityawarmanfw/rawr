#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "color/UltraHdrParams.h"

namespace rawrcam::encoding::jpeg {

enum class ChromaSubsampling : std::uint8_t {
    Yuv444 = 0,
    Yuv422 = 1,
    Yuv420 = 2,
};

// UltraHDR (JPEG_R) output intent. Disabled by default; the file stays a
// legacy SDR JPEG. When enabled, StillImageRenderer records a full-res
// recovery map on GPU (multi-channel RGB by default, preserving hue) and
// this writer dual-encodes + muxes (MPF + GContainer XMP + hdrgm +
// ISO 21496-1). Log2 fields mirror
// gainmap::GainmapParams; use toGainmapParams() so render and mux can never
// drift apart field-by-field.

struct JpegCaptureContext {
    int outputFd = -1;
    int deviceRotationDegrees = 0;
    std::int64_t wallClockUnixMillis = 0;
    std::int16_t utcOffsetMinutes = 0;
    std::string deviceMake;
    std::string deviceModel;
    std::string displayName;
    int quality = 98;
    ChromaSubsampling subsampling = ChromaSubsampling::Yuv420;
    rawrcam::color::UltraHdrParams ultraHdr;
    std::string rendererDisplayName;
    // Processing provenance for EXIF; these values do not control development.
    bool appliedLensShadingCorrection = false;
    uint32_t appliedFccSteps = 1;
    // Frozen per-frame metadata filled after the same RAW snapshot used by DNG is captured.
    std::uint16_t exifOrientation = 1;
    std::int64_t exposureTimeNs = 0;
    std::int32_t sensitivity = 0;
    std::optional<float> aperture;
    std::optional<float> focalLengthMm;

    // Frozen public metadata. Keep implementation-specific algorithm names out of this report.
    std::string imageDescription;
    // Kotlin-built film block (stock/paper names + values), empty when film
    // was off at shutter time. Spliced into the description when the shot
    // rendered through film.
    std::string filmDescription;
    double demosaicMs = 0.0;
    double colorProcessingMs = 0.0;
    double highlightReconstructionMs = 0.0;
    double refinementMs = 0.0;
    double tonemapMs = 0.0;
    double denoiseMs = 0.0;
    // GALOSH-YUV blind denoise (P1c); 0 when off.
    double galoshYuvMs = 0.0;
    double gainmapMs = 0.0;
    // Filled after rendering (not persisted): defringe + Inpaint Opposed tone
    // time, the effective highlight stage (-1 unknown, 0 off, 1 colour
    // propagation, 2 Inpaint Opposed) and whether defringe ran.
    double postTailMs = 0.0;
    int highlightStage = -1;
    bool defringeEnabled = false;
    double renderTotalMs = 0.0;
    // Wall-clock attribution of renderTotalMs so EXIF parts sum to the
    // total: pre-submit setup, submit→fence gaps beyond the GPU ranges,
    // post-fence readback + checksum.
    double renderSetupMs = 0.0;
    // Breakdown only (already inside demosaicMs / renderSetupMs).
    double demosaicSetupMs = 0.0;
    double renderEngineBuildMs = 0.0;
    double queueGapsMs = 0.0;
    double readbackMs = 0.0;
    // True when the shot rendered through film (tonemapMs then measures the
    // film record); EXIF labels the stage accordingly.
    bool filmRendered = false;
    // Reserved completion provenance. Memory fallback bypasses this writer
    // and emits the corresponding DNG-substitution outcome directly.
    bool filmFallbackMemory = false;

    // Multiframe TIMINGS assembly (writer-side). Set only by the multiframe
    // path; single-frame keeps the legacy block byte-identical. All stage
    // times in ms; multiframeTimingsBlock is preformatted in run().
    bool hasMultiframeTimings = false;
    std::string multiframeTimingsBlock;
    double multiframeTotalMs = 0.0;
    double mfBaseReadMs = 0.0;
    double mfProjectMs = 0.0;
    double mfRgbPrepMs = 0.0;
    bool mfDirectRgb = false;
};

}  // namespace rawrcam::encoding::jpeg
