package com.rawr.camera.model

/** Semantic live/persisted tone state shared by Capture and Settings. */
data class ImageToneState(
    val renderExposure: Float = 0f,
    val blacks: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val shadows: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val contrast: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    // Legacy property name retained for persisted-model compatibility; V2 semantics are Midtones -100..+100.
    val midtones: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val highlights: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val whites: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val saturation: Float = TonemapControlContract.SATURATION_NEUTRAL,
    val vibrance: Float = TonemapControlContract.VIBRANCE_NEUTRAL,
    val colorRenderingStrength: Float = 1f,
    val wbTemperature: Float = 0f,
    val wbTint: Float = 0f,
    val outputColorSpaceId: String,
    val transferFunctionId: String,
    val jpegQuality: Float = 98f,
    val jpegChromaSubsamplingId: String = "jpeg.422"
)
