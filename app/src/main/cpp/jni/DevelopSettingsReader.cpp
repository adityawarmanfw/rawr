#include "jni/DevelopSettingsReader.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
namespace rawrcam::jni {
namespace {
void applyDenoise(develop::DevelopSettings& out, const DenoiseIntent& denoise) {
    out.denoiseDetail = denoise.waveletDetail;
    out.denoiseForceY = denoise.waveletForceY;
    out.denoiseMaxScale = denoise.waveletMaxScale;
    // Method switch (P1c): method 0 = profiled wavelet, 1 = galosh lanes.
    // The master toggle parks everything; wavelet runs iff master on and
    // method is wavelet; lanes run iff master on and method is galosh.
    // Lane ints are validated in parseDenoiseSpec; the intent arrives
    // pre-arbitrated from DenoiseConfig.resolve(), this stays as backstop.
    const int method = denoise.method;
    const bool master = denoise.master;
    out.denoiseStrength = (master && method == 0) ? denoise.waveletStrength : 0.0f;
    // GALOSH blind denoisers: modes mirror galosh::GaloshRawMode /
    // galosh::GaloshYuvMode (0 off, 1 full, 2 chroma-only).
    const int rawMode = (master && method == 1) ? denoise.rawMode : 0;
    out.galoshRawMode = (rawMode == 1 || rawMode == 2) ? rawMode : 0;
    out.galoshStrength = denoise.rawStrength;
    out.galoshLuma = denoise.rawLuma;
    out.galoshChroma = denoise.rawChroma;
    const int yuvMode = (master && method == 1) ? denoise.yuvMode : 0;
    out.galoshYuvMode = (yuvMode == 1 || yuvMode == 2) ? yuvMode : 0;
    out.galoshYuvStrengthY = denoise.yuvStrengthY;
    out.galoshYuvStrengthC = denoise.yuvStrengthC;
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                        "GALOSH_JNI_MAP master=%d method=%d raw=%d yuv=%d wavelet=%.2f", master ? 1 : 0, method,
                        out.galoshRawMode, out.galoshYuvMode, out.denoiseStrength);
}
}  // namespace
DenoiseIntent parseDenoiseSpec(JNIEnv* env, jintArray modes, jfloatArray strengths) {
    DenoiseIntent out{};
    const jsize modeLen = modes ? env->GetArrayLength(modes) : 0;
    const jsize strengthLen = strengths ? env->GetArrayLength(strengths) : 0;
    if (!((modeLen == 4 && strengthLen == 7) || (modeLen == 5 && strengthLen == 8))) {
        __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative",
                            "DENOISE_SPEC_INVALID modes=%d strengths=%d (want 4/7 or 5/8); denoise off",
                            static_cast<int>(modeLen), static_cast<int>(strengthLen));
        return out;
    }
    jint m[5]{};
    jfloat s[8]{};
    env->GetIntArrayRegion(modes, 0, modeLen, m);
    env->GetFloatArrayRegion(strengths, 0, strengthLen, s);
    out.master = m[0] != 0;
    out.method = std::clamp(static_cast<int>(m[1]), 0, 1);
    out.rawMode = (m[2] == 1 || m[2] == 2) ? static_cast<int>(m[2]) : 0;
    out.yuvMode = (m[3] == 1 || m[3] == 2) ? static_cast<int>(m[3]) : 0;
    out.waveletMaxScale = (modeLen == 5) ? std::clamp(static_cast<int>(m[4]), 1, 7) : 7;
    out.waveletStrength = std::clamp(static_cast<float>(s[0]), 0.0f, 8.0f);
    out.waveletDetail = std::clamp(static_cast<float>(s[1]), 0.0f, 1.8f);
    out.waveletForceY = (strengthLen == 8 && std::isfinite(static_cast<float>(s[7])))
                            ? std::clamp(static_cast<float>(s[7]), 0.0f, 1.0f)
                            : 0.25f;
    out.rawStrength = std::clamp(static_cast<float>(s[2]), 0.0f, 8.0f);
    out.rawLuma = std::clamp(static_cast<float>(s[3]), 0.0f, 8.0f);
    out.rawChroma = std::clamp(static_cast<float>(s[4]), 0.0f, 8.0f);
    out.yuvStrengthY = std::clamp(static_cast<float>(s[5]), 0.0f, 8.0f);
    out.yuvStrengthC = std::clamp(static_cast<float>(s[6]), 0.0f, 8.0f);
    return out;
}

develop::DevelopSettings readCaptureDevelopSettings(const CaptureDevelopInput& input) {
    develop::DevelopSettings out{};
    out.pipelineDiagnosticsEnabled = input.pipelineDiagnosticsEnabled == JNI_TRUE;
    rawrcam::tonemap_integration::ColorRenderProfile profile{};
    if (rawrcam::tonemap_integration::colorRenderProfileFromId(
            static_cast<uint32_t>(std::max(input.colorRenderProfile, 0)), &profile))
        out.colorRenderProfile = profile;
    out.importedLutProfileId = input.importedLutProfileId;
    out.dualAutoContrast = input.dualAutoContrast == JNI_TRUE;
    out.dualContrastPercent = std::clamp(static_cast<float>(input.dualContrastPercent), 0.0f, 100.0f);
    out.fccSteps = static_cast<std::uint32_t>(std::clamp(static_cast<int>(input.fccSteps), 1, 8));
    out.defringeStrength =
        (input.defringeEnabled == JNI_TRUE) ? std::clamp(static_cast<float>(input.defringeStrength), 0.0f, 1.0f) : 0.0f;
    out.defringeEdgeThreshold = std::clamp(static_cast<float>(input.defringeEdgeThreshold), 0.005f, 0.2f);
    out.defringeLumaFloor = std::clamp(static_cast<float>(input.defringeLumaFloor), 0.0f, 0.5f);
    applyDenoise(out, input.denoise);
    out.lensShadingCorrectionEnabled = input.lensShadingCorrectionEnabled == JNI_TRUE;
    out.highlightReconstructionEnabled = input.highlightReconstructionEnabled == JNI_TRUE;
    out.distortionCorrectionEnabled = input.distortionCorrectionEnabled == JNI_TRUE;
    switch (input.demosaicAlgorithm) {
        case 2:
            out.demosaicAlgorithm = rawrcam::develop::DemosaicAlgorithm::Vng4;
            break;
        case 3:
            out.demosaicAlgorithm = rawrcam::develop::DemosaicAlgorithm::DualRcdVng4;
            break;
        default:
            out.demosaicAlgorithm = rawrcam::develop::DemosaicAlgorithm::Rcd;
            break;
    }
    return out;
}
develop::DevelopSettings readRendererDevelopSettings(Json& json) {
    develop::DevelopSettings out{};
    out.importedLutProfileId = json.text("importedProfileId");
    if (out.importedLutProfileId == "null") out.importedLutProfileId.clear();
    out.colorRenderProfile = out.importedLutProfileId.empty()
                                 ? rawrcam::tonemap_integration::ColorRenderProfile::RawrBase
                                 : rawrcam::tonemap_integration::ColorRenderProfile::UserLut;
    if (json.text("colorRenderProfile") == "SRgb") {
        out.colorRenderProfile = rawrcam::tonemap_integration::ColorRenderProfile::SRgb;
        out.importedLutProfileId.clear();
    }
    out.fccSteps = uint32_t(json.number("fccSteps", 1));
    out.fccEdgeSigma = .08f;
    out.fccChromaBound = 1;
    out.defringeStrength = json.flag("defringeEnabled", true) ? float(json.number("defringeStrength", 1)) : 0;
    out.defringeEdgeThreshold = float(json.number("defringeEdgeThreshold", .02));
    out.defringeLumaFloor = float(json.number("defringeLumaFloor", .08));
    out.denoiseStrength =
        json.flag("denoiseEnabled", false) ? std::clamp(float(json.number("denoiseStrength", 1)), 0.f, 8.f) : 0;
    out.denoiseDetail = std::clamp(float(json.number("denoiseDetail", 1)), 0.f, 1.8f);
    out.denoiseForceY = std::clamp(float(json.number("denoiseLuma", 0.25)), 0.f, 1.f);
    out.denoiseMaxScale = std::clamp(static_cast<int>(json.number("denoiseScales", 7)), 1, 7);
    DenoiseIntent denoise{};
    denoise.master = json.flag("denoiseEnabled", false);
    denoise.method = std::clamp(static_cast<int>(json.number("denoiseMethod", 0)), 0, 1);
    denoise.waveletStrength = out.denoiseStrength;
    denoise.waveletDetail = out.denoiseDetail;
    denoise.waveletForceY = out.denoiseForceY;
    denoise.waveletMaxScale = out.denoiseMaxScale;
    denoise.yuvMode = static_cast<int>(json.number("galoshYuvMode", 0));
    denoise.yuvStrengthY = std::clamp(float(json.number("galoshYuvStrengthY", 1)), 0.f, 8.f);
    denoise.yuvStrengthC = std::clamp(float(json.number("galoshYuvStrengthC", 1)), 0.f, 8.f);
    applyDenoise(out, denoise);
    out.highlightReconstructionEnabled = json.flag("highlightReconstructionEnabled", true);
    out.highlightReconstructionMethod = std::clamp(uint32_t(json.number("highlightReconstructionMethod", 0)), 0u, 1u);
    out.highlightThreshold = std::clamp(float(json.number("highlightThreshold", 1)), 0.5f, 2.0f);
    out.highlightCompression = std::clamp(float(json.number("highlightCompression", 100)), 0.0f, 300.0f);

    return out;
}
}  // namespace rawrcam::jni
