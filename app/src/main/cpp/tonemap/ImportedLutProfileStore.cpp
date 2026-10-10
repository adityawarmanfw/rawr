#include "ImportedLutProfileStore.h"

#include <tonemap/color/ColorSpace.h>
#include <tonemap/lut/Lut3D.h>

#include <fstream>
#include <sstream>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rawrcam::tonemap_integration {
namespace {

constexpr int kProfileVersion = 1;
constexpr int kMaxGamutId = static_cast<int>(tonemap::color::Gamut::FujifilmFGamutC);
constexpr int kMaxTransferId = static_cast<int>(tonemap::color::TransferFunction::Rec709);

bool safeProfileId(std::string_view value) {
    return !value.empty() && value.find("..") == std::string_view::npos && value.find('/') == std::string_view::npos &&
           value.find('\\') == std::string_view::npos;
}

bool safeStagePath(std::string_view value) {
    constexpr std::string_view prefix = "lut_profiles/files/";
    return value.rfind(prefix, 0) == 0 && value.find("..") == std::string_view::npos &&
           value.find('\\') == std::string_view::npos;
}

struct ProfileMetadata {
    int version = 0;
    int inputGamut = -1;
    int inputTransfer = -1;
    int outputGamut = -1;
    int outputTransfer = -1;
    int afterLut = 1;
    // Per-profile TONE mirror of the Kotlin ProfileTone. Native render path
    // receives tone via tonemapParams (frozen from the active profile), so
    // these values are validated for file integrity but not applied here.
    // Missing keys (pre-per-profile-tone files) mean neutral tone.
    float toneRenderExposure = 0.0f;
    float toneBlacks = 0.0f;
    float toneShadows = 0.0f;
    float toneContrast = 0.0f;
    float toneMidtones = 0.0f;
    float toneHighlights = 0.0f;
    float toneWhites = 0.0f;
    float toneSaturation = 0.0f;
    float toneVibrance = 0.0f;
    std::vector<std::string> stages;
};

float parseToneFloat(const std::string& value, float fallback) {    try {
        const float parsed = std::stof(value);
        if (!std::isfinite(parsed)) return fallback;
        return parsed;
    } catch (...) {
        return fallback;
    }
}

bool inToneRange(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

ProfileMetadata parseMetadata(std::istream& stream) {
    ProfileMetadata metadata;
    std::string line;
    while (std::getline(stream, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        if (key == "version")
            metadata.version = std::stoi(value);
        else if (key == "input_gamut")
            metadata.inputGamut = std::stoi(value);
        else if (key == "input_transfer")
            metadata.inputTransfer = std::stoi(value);
        else if (key == "output_gamut")
            metadata.outputGamut = std::stoi(value);
        else if (key == "output_transfer")
            metadata.outputTransfer = std::stoi(value);
        else if (key == "after_lut")
            metadata.afterLut = std::stoi(value);
        else if (key == "tone_render_exposure")
            metadata.toneRenderExposure = parseToneFloat(value, 0.0f);
        else if (key == "tone_blacks")
            metadata.toneBlacks = parseToneFloat(value, 0.0f);
        else if (key == "tone_shadows")
            metadata.toneShadows = parseToneFloat(value, 0.0f);
        else if (key == "tone_contrast")
            metadata.toneContrast = parseToneFloat(value, 0.0f);
        else if (key == "tone_midtones")
            metadata.toneMidtones = parseToneFloat(value, 0.0f);
        else if (key == "tone_highlights")
            metadata.toneHighlights = parseToneFloat(value, 0.0f);
        else if (key == "tone_whites")
            metadata.toneWhites = parseToneFloat(value, 0.0f);
        else if (key == "tone_saturation")
            metadata.toneSaturation = parseToneFloat(value, 0.0f);
        else if (key == "tone_vibrance")
            metadata.toneVibrance = parseToneFloat(value, 0.0f);
        else if (key == "stage")
            metadata.stages.push_back(value);
    }
    return metadata;
}

void validateMetadata(const ProfileMetadata& metadata) {
    const bool idsValid =
        metadata.version == kProfileVersion && metadata.inputGamut >= 0 && metadata.inputGamut <= kMaxGamutId &&
        metadata.outputGamut >= 0 && metadata.outputGamut <= kMaxGamutId && metadata.inputTransfer >= 0 &&
        metadata.inputTransfer <= kMaxTransferId && metadata.outputTransfer >= 0 &&
        metadata.outputTransfer <= kMaxTransferId && (metadata.afterLut == 0 || metadata.afterLut == 1);
    // Tone keys are optional (pre-per-profile-tone files omit them → neutral);
    // when present they must be finite and in range.
    const bool toneValid = inToneRange(metadata.toneRenderExposure, -5.0f, 5.0f) &&
                           inToneRange(metadata.toneBlacks, -100.0f, 100.0f) &&
                           inToneRange(metadata.toneShadows, -100.0f, 100.0f) &&
                           inToneRange(metadata.toneContrast, -100.0f, 100.0f) &&
                           inToneRange(metadata.toneMidtones, -100.0f, 100.0f) &&
                           inToneRange(metadata.toneHighlights, -100.0f, 100.0f) &&
                           inToneRange(metadata.toneWhites, -100.0f, 100.0f) &&
                           inToneRange(metadata.toneSaturation, -100.0f, 100.0f) &&
                           inToneRange(metadata.toneVibrance, -100.0f, 100.0f);
    if (!idsValid || !toneValid || metadata.stages.empty() ||
        metadata.stages.size() > tonemap::lut::kMaxGpuLutStages) {
        throw std::runtime_error("invalid imported LUT profile metadata");
    }
}

// RAWR NTRL look (tonemap/luts/rawr_ntrl.cube): sRGB in,
// sRGB out, applied directly on the existing render like an imported
// sRGB/sRGB profile, everywhere (preview, stills, replay, video). Parsed once; the chain is copied per tonemap build.
const tonemap::lut::LutChain& rawrNtrlChain() {
    static const tonemap::lut::LutChain chain = [] {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverlength-strings"
        static constexpr const char kCube[] =
#include "tonemap/BuiltInRawrNtrlLut.inc"
            ;
#pragma clang diagnostic pop
        std::istringstream in(std::string(kCube, sizeof(kCube) - 1));
        tonemap::lut::LutChain c{};
        c.placement = tonemap::lut::LutPlacement::RenderTransform;
        c.intensity = 1.0f;
        c.inputSpace = {tonemap::color::Gamut::SRgbRec709, tonemap::color::TransferFunction::SRgb};
        c.outputSpace = {tonemap::color::Gamut::SRgbRec709, tonemap::color::TransferFunction::SRgb};
        c.afterAction = tonemap::lut::LutAfterAction::UseDirectly;
        c.stages.push_back(tonemap::lut::parseCube(in, 65));
        return c;
    }();
    return chain;
}

}  // namespace

std::optional<tonemap::lut::LutChain> loadRenderTransformLut(ColorRenderProfile profile, const std::string& filesDir,
                                                             const std::string& importedProfileId) {
    if (profile == ColorRenderProfile::RawrBase) return rawrNtrlChain();
    if (profile != ColorRenderProfile::UserLut) return std::nullopt;
    if (!safeProfileId(importedProfileId)) throw std::invalid_argument("invalid imported LUT profile id");

    std::ifstream file(filesDir + "/lut_profiles/" + importedProfileId + ".rawrprofile");
    if (!file) throw std::runtime_error("cannot open imported LUT profile");
    const auto metadata = parseMetadata(file);
    validateMetadata(metadata);

    tonemap::lut::LutChain chain{};
    chain.placement = tonemap::lut::LutPlacement::RenderTransform;
    chain.intensity = 1.0f;
    chain.inputSpace = {
        static_cast<tonemap::color::Gamut>(metadata.inputGamut),
        static_cast<tonemap::color::TransferFunction>(metadata.inputTransfer),
    };
    // outputSpace is only consumed when afterAction == ConvertToSrgb
    // (shader: lutSrgb = cst(lutOut, outGamut, outTf → sRGB)); with
    // UseDirectly the stored values persist but are ignored by design.
    chain.outputSpace = {
        static_cast<tonemap::color::Gamut>(metadata.outputGamut),
        static_cast<tonemap::color::TransferFunction>(metadata.outputTransfer),
    };
    chain.afterAction = metadata.afterLut == 0 ? tonemap::lut::LutAfterAction::UseDirectly
                                               : tonemap::lut::LutAfterAction::ConvertToSrgb;

    for (const auto& relativePath : metadata.stages) {
        if (!safeStagePath(relativePath)) throw std::runtime_error("invalid LUT stage path");
        chain.stages.push_back(tonemap::lut::parseCubeFile(filesDir + "/" + relativePath, 65));
    }
    return chain;
}

}  // namespace rawrcam::tonemap_integration
