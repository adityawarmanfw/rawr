#include <cassert>
#include <cstdio>
#include <string>

#include "encoding/jpeg/JpegPublicDescription.h"

using namespace rawrcam;
using namespace rawrcam::encoding;

namespace {
bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
}  // namespace

int main() {
    develop::DevelopSettings develop;
    develop.demosaicAlgorithm = develop::DemosaicAlgorithm::DualAmazeVng4;
    develop.dualAutoContrast = false;
    develop.dualContrastPercent = 25.0f;
    develop.fccSteps = 3;
    develop.lensShadingCorrectionEnabled = true;
    develop.highlightReconstructionMethod = 1;
    tonemap::TonemapParams tone{};
    tone.shadowLiftEV = 38.0f;

    const std::string neutral = jpeg::buildPublicDescription(develop, tone, "RAWR NTRL", 98,
                                                             jpeg::ChromaSubsampling::Yuv420, false, "");
    assert(neutral.rfind(std::string(kCapturedWith) + "\nSingle frame\n\nPARAMETERS\nJPEG output parameters:\n", 0) == 0);
    assert(contains(neutral, "Captured with Rawr v"));
    assert(contains(neutral, "- Demosaic: AMaZE+VNG4\n- Dual auto contrast: 25.0%\n"));
    assert(contains(neutral, "- FCC steps: 3\n- Vignette correction (LSC): on\n"));
    assert(contains(neutral, "- Highlight reconstruction: on (Inpaint Opposed)\n"));
    assert(contains(neutral, "- JPEG quality: 98, 4:2:0\n- Color profile: RAWR NTRL\n"));
    assert(!contains(neutral, "Chroma denoise"));
    // Same slider format as the multiframe section.
    assert(contains(neutral, "Tonemap parameters:\n- Exposure: +0.00 EV\n- Blacks: +0.0\n- Shadows: +38.0\n"));
    assert(contains(neutral, "- AE post-gain: 1.00x\n"));
    assert(!contains(neutral, "AgX parameters"));
    assert(contains(neutral, "- Color profile: RAWR NTRL\n\nAdaptive tone:\n- Shadow lift: off\n\nTonemap parameters:\n"));
    assert(!contains(neutral, "Denoise"));

    // AgX profiles list every control before the shared sliders; the
    // profile is resolved from the develop settings, not the tone.
    develop.colorRenderProfile = tonemap_integration::ColorRenderProfile::AgX;
    tone.agx = tonemap::agxPreset(tonemap::AgxPreset::Sigmoid);
    tone.agx.primaries.rotation[0] = -0.1f;
    const std::string agx = jpeg::buildPublicDescription(develop, tone, "AgX Sigmoid", 98,
                                                         jpeg::ChromaSubsampling::Yuv420, false, "");
    assert(contains(agx, "- Color profile: AgX Sigmoid\n\nAdaptive tone:\n- Shadow lift: off\n\nAgX parameters:\n"
                            "- White Relative Exposure: 6.5 EV\n"));
    assert(contains(agx, "- Black Relative Exposure: -10.0 EV\n"));
    assert(contains(agx, "- Contrast: 3.07\n"));
    assert(contains(agx, "- Shoulder Power: 3.71\n"));
    assert(contains(agx, "- Pivot Relative Exposure: +0.00 EV\n"));
    assert(contains(agx, "- Pivot Target Output: 18.0%\n"));
    assert(contains(agx, "- Preserve Hue: 100%\n"));
    assert(contains(agx, "- Red Rotation: -5.7 deg\n"));
    assert(contains(agx, "- Blue Reverse Rotation: -3.6 deg\n\nTonemap parameters:\n"));

    // controls -> params -> controls round-trips every AgX control.
    const auto controls = tonemap::agxToControls(tone.agx);
    const auto back = tonemap::agxToControls(tonemap::agxFromControls(controls.data()));
    for (size_t i = 0; i < controls.size(); ++i) assert(controls[i] == back[i]);

    // The adaptive shadow lift feeds tone and film, so both print it.
    tone.adaptiveTone = {0.42f, -3.1f, 1.2f};
    const std::string lifted = jpeg::buildPublicDescription(develop, tone, "AgX Sigmoid", 98,
                                                            jpeg::ChromaSubsampling::Yuv420, false, "");
    assert(contains(lifted, "Adaptive tone:\n- Shadow lift: +0.42 EV\n- Scene key: -3.10 EV\n"
                            "- Highlight anchor: +1.20 EV\n\nAgX parameters:\n"));

    // Film renders keep the output block and replace the tonemap with film.
    const std::string film = jpeg::buildPublicDescription(develop, tone, "AgX Sigmoid", 98,
                                                          jpeg::ChromaSubsampling::Yuv420, true,
                                                          "Film simulation:\n- Stock: Gold 200");
    assert(contains(film, "- Highlight anchor: +1.20 EV\n\nFilm simulation:\n- Stock: Gold 200\n"));
    assert(!contains(film, "AgX parameters") && !contains(film, "Tonemap parameters"));

    // Denoise sections print only for stages that ran.
    develop.galoshRawMode = 2;
    const std::string denoised = jpeg::buildPublicDescription(develop, tone, "AgX Sigmoid", 98,
                                                              jpeg::ChromaSubsampling::Yuv420, false, "", 1.5f,
                                                              1e-4f, 2e-6f);
    assert(contains(denoised, "\nDenoise parameters:\n- Strength: 1.50\n- Detail: 1.00\n"));
    assert(contains(denoised, "\nDenoise (galosh-raw) parameters:\n- Mode: chroma-only\n"));
    assert(!contains(denoised, "galosh-yuv"));
    // Pipeline order: denoise sections precede the AgX and tonemap sections.
    assert(denoised.find("Denoise parameters:") < denoised.find("AgX parameters:"));
    assert(denoised.find("Denoise (galosh-raw)") < denoised.find("Tonemap parameters:"));

    // Renderer exports swap the verb and the mode line, nothing else.
    const std::string rendered = jpeg::buildPublicDescription(develop, tone, "AgX Sigmoid", 98,
                                                              jpeg::ChromaSubsampling::Yuv420, false, "", 1.5f,
                                                              1e-4f, 2e-6f, /*rendererExport=*/true);
    assert(rendered.rfind(std::string(kRenderedWith) + "\nDNG re-render\n\nPARAMETERS\n", 0) == 0);
    assert(rendered.substr(rendered.find("\n\nPARAMETERS")) == denoised.substr(denoised.find("\n\nPARAMETERS")));

    std::puts("public_description_test ok");
    return 0;
}
