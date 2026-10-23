#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

// ImageDescription sections shared by single-frame, multiframe and Renderer
// outputs, so every file names the same settings the same way. Engine-free:
// callers map their develop settings and tonemap values into these.
namespace rawrcam::encoding {

// "JPEG output parameters:" block. Empty labels omit their line.
struct OutputParameters {
    std::string demosaicLabel = "n/a";
    std::string dualLabel = "n/a";
    std::string chromaDenoiseLabel;  // multiframe burst chroma denoise only
    std::uint32_t fccSteps = 1;
    std::uint32_t captureSharpenIterations = 0;
    bool lscEnabled = false;
    bool highlightReconstructionEnabled = true;
    std::string highlightMethodLabel;  // "Inpaint Opposed", "color propagation"
    int jpegQuality = 98;
    std::string subsampling = "4:2:2";
    std::string colorProfile = "RAWR NTRL";
};

// Tonemap slider values plus the pre-formatted AgX block.
struct ToneParameters {
    float exposureEV = 0.0f;
    float blacks = 0.0f;
    float shadows = 0.0f;
    float midtones = 0.0f;
    float contrast = 0.0f;
    float whites = 0.0f;
    float highlights = 0.0f;
    float saturation = 0.0f;
    float vibrance = 0.0f;
    float aePostGain = 1.0f;
    // Capture-time adaptive shadow lift (tonemap::AdaptiveToneParams, log2
    // luminance); feeds tone and film alike. liftEv 0 = off.
    float adaptiveLiftEv = 0.0f;
    float adaptiveKeyEv = -4.0f;
    float adaptiveHighlightEv = 0.0f;
    // "- Label: value" AgX lines; empty unless the colour render profile is AgX.
    std::string agxControls;
};

inline std::string highlightMethodLabel(std::uint32_t method) {
    return method == 1u ? "Inpaint Opposed" : "color propagation";
}

inline std::string formatOutputParameters(const OutputParameters& output) {
    std::ostringstream out;
    out << "JPEG output parameters:\n"
        << "- Demosaic: " << output.demosaicLabel << '\n'
        << "- Dual auto contrast: " << output.dualLabel << '\n';
    if (!output.chromaDenoiseLabel.empty()) out << "- Chroma denoise: " << output.chromaDenoiseLabel << '\n';
    out << "- FCC steps: " << output.fccSteps << '\n'
        << "- Vignette correction (LSC): " << (output.lscEnabled ? "on" : "off") << '\n'
        << "- Highlight reconstruction: " << (output.highlightReconstructionEnabled ? "on" : "off");
    if (output.highlightReconstructionEnabled && !output.highlightMethodLabel.empty())
        out << " (" << output.highlightMethodLabel << ')';
    out << '\n' << "- Capture sharpening: ";
    const auto iterations = output.captureSharpenIterations;
    if (iterations == 0) {
        out << "off";
    } else {
        const char* level = iterations == 2 ? "Low" : iterations == 3 ? "Medium" :
                            iterations == 4 ? "Sharp" : iterations == 6 ? "Sharpest" : "Custom";
        out << level << " (" << iterations << " iterations)";
    }
    out << '\n'
        << "- JPEG quality: " << output.jpegQuality << ", " << output.subsampling << '\n'
        << "- Color profile: " << output.colorProfile << '\n';
    return out.str();
}

// "Adaptive tone:" section, printed before tone or film (it feeds both).
inline std::string formatAdaptiveTone(const ToneParameters& tone) {
    std::ostringstream out;
    out << std::showpos << std::fixed << std::setprecision(2) << "Adaptive tone:\n";
    if (!(tone.adaptiveLiftEv > 0.0f)) {
        out << "- Shadow lift: off\n";
    } else {
        out << "- Shadow lift: " << tone.adaptiveLiftEv << " EV\n"
            << "- Scene key: " << tone.adaptiveKeyEv << " EV\n"
            << "- Highlight anchor: " << tone.adaptiveHighlightEv << " EV\n";
    }
    return out.str();
}

// "AgX parameters:" (when present) then "Tonemap parameters:".
inline std::string formatToneParameters(const ToneParameters& tone) {
    const auto signed1 = [](float value) {
        std::ostringstream out;
        out << std::showpos << std::fixed << std::setprecision(1) << value;
        return out.str();
    };
    std::ostringstream out;
    if (!tone.agxControls.empty()) out << "AgX parameters:\n" << tone.agxControls << '\n';
    out << "Tonemap parameters:\n"
        << "- Exposure: " << std::showpos << std::fixed << std::setprecision(2) << tone.exposureEV << std::noshowpos
        << " EV\n"
        << "- Blacks: " << signed1(tone.blacks) << '\n'
        << "- Shadows: " << signed1(tone.shadows) << '\n'
        << "- Midtones: " << signed1(tone.midtones) << '\n'
        << "- Contrast: " << signed1(tone.contrast) << '\n'
        << "- Whites: " << signed1(tone.whites) << '\n'
        << "- Highlights: " << signed1(tone.highlights) << '\n'
        << "- Saturation: " << signed1(tone.saturation) << '\n'
        << "- Vibrance: " << signed1(tone.vibrance) << '\n'
        << "- AE post-gain: " << std::fixed << std::setprecision(2) << tone.aePostGain << "x\n"
        << "- Shoulder anchor (internal): 2.0 EV\n";
    return out.str();
}

}  // namespace rawrcam::encoding
