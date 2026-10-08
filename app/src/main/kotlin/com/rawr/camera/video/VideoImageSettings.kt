package com.rawr.camera.video

import com.rawr.camera.settings.model.SettingsValues
import org.json.JSONObject

/** Value passed through the UI/native boundary. Video can later persist its own copy. */
data class VideoImageSettings(
    val lensShadingEnabled: Boolean,
    val highlightEnabled: Boolean,
    val highlightMethod: Int,
    val highlightThreshold: Float,
    val highlightCompression: Float,
    val fccSteps: Int,
    val defringeStrength: Float,
    val defringeEdgeThreshold: Float,
    val defringeLumaFloor: Float,
    val waveletDenoiseStrength: Float,
    val waveletDenoiseDetail: Float,
    val waveletDenoiseLuma: Float,
    val waveletDenoiseScales: Int
) {
    fun toJson(): JSONObject = JSONObject()
        .put("lensShadingEnabled", lensShadingEnabled)
        .put("highlightEnabled", highlightEnabled)
        .put("highlightMethod", highlightMethod)
        .put("highlightThreshold", highlightThreshold)
        .put("highlightCompression", highlightCompression)
        .put("fccSteps", fccSteps)
        .put("defringeStrength", defringeStrength)
        .put("defringeEdgeThreshold", defringeEdgeThreshold)
        .put("defringeLumaFloor", defringeLumaFloor)
        .put("waveletDenoiseStrength", waveletDenoiseStrength)
        .put("waveletDenoiseDetail", waveletDenoiseDetail)
        .put("waveletDenoiseLuma", waveletDenoiseLuma)
        .put("waveletDenoiseScales", waveletDenoiseScales)
}

fun SettingsValues.toVideoImageSettings(): VideoImageSettings {
    return VideoImageSettings(
        videoLensShadingEnabled,
        videoHighlightEnabled,
        videoHighlightMethod,
        videoHighlightThreshold,
        videoHighlightCompression,
        // Video FCC is a single step when enabled.
        if (videoFccEnabled) 1 else 0,
        if (videoDefringeEnabled) videoDefringeStrength else 0f,
        videoDefringeEdgeThreshold,
        videoDefringeLumaFloor,
        if (videoDenoiseEnabled) videoDenoiseStrength else 0f,
        videoDenoiseDetail,
        videoDenoiseLuma,
        videoDenoiseScales
    )
}
