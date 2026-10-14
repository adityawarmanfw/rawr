#pragma once

#include <iomanip>
#include <sstream>
#include <string>

#include "develop/DevelopSettings.h"
#include "encoding/ProcessingParameters.h"
#include "encoding/SoftwareName.h"
#include "encoding/jpeg/AgxDescription.h"
#include "encoding/jpeg/JpegCaptureContext.h"
#include "tonemap/ColorRenderProfile.h"
#include "tonemap/TonemapMath.h"

namespace rawrcam::encoding::jpeg {

inline const char* demosaicLabel(develop::DemosaicAlgorithm algorithm) {
    switch (algorithm) {
        case develop::DemosaicAlgorithm::Rcd: return "RCD";
        case develop::DemosaicAlgorithm::Vng4: return "VNG4";
        case develop::DemosaicAlgorithm::DualRcdVng4: return "RCD+VNG4";
        case develop::DemosaicAlgorithm::Amaze: return "AMaZE";
        case develop::DemosaicAlgorithm::DualAmazeVng4: return "AMaZE+VNG4";
    }
    return "n/a";
}

inline const char* subsamplingLabel(ChromaSubsampling subsampling) {
    return subsampling == ChromaSubsampling::Yuv444 ? "4:4:4"
           : subsampling == ChromaSubsampling::Yuv422 ? "4:2:2"
                                                      : "4:2:0";
}

// JPEG output parameters from the develop settings a still renders with.
inline OutputParameters outputParametersFrom(const develop::DevelopSettings& develop, int jpegQuality,
                                             ChromaSubsampling subsampling, const std::string& colorProfile) {
    OutputParameters out;
    out.demosaicLabel = demosaicLabel(develop.demosaicAlgorithm);
    const bool dual = develop.demosaicAlgorithm == develop::DemosaicAlgorithm::DualRcdVng4 ||
                      develop.demosaicAlgorithm == develop::DemosaicAlgorithm::DualAmazeVng4;
    if (dual) {
        std::ostringstream percent;
        percent << std::fixed << std::setprecision(1) << develop.dualContrastPercent << '%';
        out.dualLabel = develop.dualAutoContrast ? "auto" : percent.str();
    }
    out.fccSteps = develop.fccSteps;
    out.lscEnabled = develop.lensShadingCorrectionEnabled;
    out.highlightReconstructionEnabled = develop.highlightReconstructionEnabled;
    out.highlightMethodLabel = highlightMethodLabel(develop.highlightReconstructionMethod);
    out.jpegQuality = jpegQuality;
    out.subsampling = subsamplingLabel(subsampling);
    out.colorProfile = colorProfile.empty() ? "RAWR NTRL" : colorProfile;
    return out;
}

// Tonemap sliders plus the AgX block when the colour render profile renders
// through AgX (resolved here, so callers pass the tone as frozen).
inline ToneParameters toneParametersFrom(const tonemap::TonemapParams& tone,
                                         tonemap_integration::ColorRenderProfile profile) {
    ToneParameters out;
    out.exposureEV = tone.exposureEV;
    out.blacks = tone.blackPointEV;
    out.shadows = tone.shadowLiftEV;
    out.midtones = tone.midtoneLiftEV;
    out.contrast = tone.contrast;
    out.whites = tone.whitePointEV;
    out.highlights = tone.highlightBiasEV;
    out.saturation = tone.saturation;
    out.vibrance = tone.vibrance;
    out.aePostGain = tone.aePostGain;
    out.adaptiveLiftEv = tone.adaptiveTone.liftEv;
    out.adaptiveKeyEv = tone.adaptiveTone.keyEv;
    out.adaptiveHighlightEv = tone.adaptiveTone.highlightEv;
    if (tonemap_integration::isAgxProfile(profile)) out.agxControls = formatAgxControls(tone.agx, "- ");
    return out;
}

// Human-readable settings block stamped into JPEG/DNG ImageDescription and
// XMP. Single owner for still captures and Renderer exports, laid out like
// the multiframe one: header and mode line, then PARAMETERS in pipeline
// order (output, denoise, adaptive tone, AgX, tonemap or film). A film
// render reports the film block instead of a tonemap that never touched the
// pixels; denoise sections print only for stages that ran.
inline std::string buildPublicDescription(const develop::DevelopSettings& develop, const tonemap::TonemapParams& tone,
                                          const std::string& colorProfile, int jpegQuality,
                                          ChromaSubsampling subsampling, bool filmRendered,
                                          const std::string& filmDescription, float denoiseStrength = 0.0f,
                                          float denoiseNoiseA = 0.0f, float denoiseNoiseB = 0.0f,
                                          bool rendererExport = false) {
    std::ostringstream out;
    out << (rendererExport ? kRenderedWith : kCapturedWith) << '\n'
        << (rendererExport ? "DNG re-render" : "Single frame") << "\n\nPARAMETERS\n"
        << formatOutputParameters(outputParametersFrom(develop, jpegQuality, subsampling, colorProfile));
    out << std::fixed << std::setprecision(2);
    // Profiled wavelet denoise (pre-WB linear).
    if (denoiseStrength > 0.0f) {
        out << "\nDenoise parameters:\n"
            << "- Strength: " << denoiseStrength << '\n'
            << "- Detail: " << develop.denoiseDetail << '\n'
            << std::setprecision(9) << "- Noise A: " << denoiseNoiseA << '\n'
            << "- Noise B: " << denoiseNoiseB << '\n'
            << std::setprecision(2);
    }
    // GALOSH blind denoisers (1 full, 2 chroma-only). Labels match the
    // TIMINGS stage names ("Denoise (galosh-yuv)").
    if (develop.galoshRawMode == 1 || develop.galoshRawMode == 2) {
        out << "\nDenoise (galosh-raw) parameters:\n"
            << "- Mode: " << (develop.galoshRawMode == 1 ? "full" : "chroma-only") << '\n'
            << "- Strength: " << develop.galoshStrength << '\n'
            << "- Luma: " << develop.galoshLuma << '\n'
            << "- Chroma: " << develop.galoshChroma << '\n';
    }
    if (develop.galoshYuvMode == 1 || develop.galoshYuvMode == 2) {
        out << "\nDenoise (galosh-yuv) parameters:\n"
            << "- Mode: " << (develop.galoshYuvMode == 1 ? "full" : "chroma-only") << '\n'
            << "- Strength Y: " << develop.galoshYuvStrengthY << '\n'
            << "- Strength C: " << develop.galoshYuvStrengthC << '\n';
    }
    const ToneParameters described = toneParametersFrom(tone, develop.colorRenderProfile);
    out << '\n' << formatAdaptiveTone(described) << '\n';
    if (filmRendered)
        out << (filmDescription.empty() ? std::string("Film simulation") : filmDescription) << '\n';
    else
        out << formatToneParameters(described);
    return out.str();
}

}  // namespace rawrcam::encoding::jpeg
