#pragma once
#include "develop/DevelopSettings.h"
#include "jni/JsonObject.h"
namespace rawrcam::jni {
struct DenoiseIntent {
    bool master = false;
    float waveletStrength = 0.0f;
    float waveletDetail = 1.0f;
    float waveletForceY = 0.25f;
    int waveletMaxScale = 7;
};
DenoiseIntent parseDenoiseSpec(JNIEnv* env, jintArray modes, jfloatArray strengths);
struct CaptureDevelopInput {
    jboolean pipelineDiagnosticsEnabled;
    jint colorRenderProfile;
    std::string importedLutProfileId;
    jboolean dualAutoContrast;
    jfloat dualContrastPercent;
    jint fccSteps;
    jboolean defringeEnabled;
    jfloat defringeStrength, defringeEdgeThreshold, defringeLumaFloor;
    DenoiseIntent denoise;
    jboolean lensShadingCorrectionEnabled, highlightReconstructionEnabled, distortionCorrectionEnabled;
    jint demosaicAlgorithm;
};
develop::DevelopSettings readCaptureDevelopSettings(const CaptureDevelopInput& input);
develop::DevelopSettings readRendererDevelopSettings(Json& json);
}  // namespace rawrcam::jni
