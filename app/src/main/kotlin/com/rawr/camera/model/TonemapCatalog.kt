package com.rawr.camera.model

import kotlin.math.roundToInt

/**
 * Single source of truth for the regular (non-Film-Sim) tonemap controls.
 *
 * The canonical parameter order, labels, ranges, defaults, units and JSON keys
 * live here and are consumed by the capture strip, the Settings "Tone" group
 * and the Renderer recipe so none of them redefine the list or the numbers.
 */
/** Where a control runs relative to the render (see tonemap.comp). */
enum class TonemapGroup(val label: String) {
    /** Scene-linear gain before everything. */
    Exposure("Exposure"),

    /** Before the render, in its input encoding (DaVinci Intermediate, LUT input, sRGB/Rec.709). */
    Grade("Grade"),

    /** After the render, on the display image. */
    Output("Output")
}

data class TonemapParam(
    /** Capture/settings enum binding; null for render exposure (not a scrub target). */
    val param: ToneParameter?,
    /** Stable JSON key used by capture/renderer recipes. */
    val jsonKey: String,
    /** Compact strip label. */
    val shortLabel: String,
    /** Settings/renderer slider label. */
    val longLabel: String,
    val minimum: Float,
    val maximum: Float,
    val defaultValue: Float,
    val step: Float,
    val decimals: Int,
    val group: TonemapGroup,
    val unit: String = ""
)

object TonemapCatalog {
    val renderExposure =
        TonemapParam(
            param = null,
            jsonKey = "renderExposure",
            shortLabel = "EV",
            longLabel = "Render Exposure",
            minimum = TonemapControlContract.EXPOSURE_MIN_EV,
            maximum = TonemapControlContract.EXPOSURE_MAX_EV,
            defaultValue = TonemapControlContract.EXPOSURE_NEUTRAL_EV,
            step = .1f,
            decimals = 1,
            group = TonemapGroup.Exposure,
            unit = " EV"
        )

    val highlights = tone(ToneParameter.Highlights, "highlights", "HL", "Highlights", TonemapGroup.Grade)
    val shadows = tone(ToneParameter.Shadows, "shadows", "SHD", "Shadows", TonemapGroup.Grade)
    val midtones = tone(ToneParameter.Midtones, "midtones", "MID", "Midtones", TonemapGroup.Grade)
    val contrast = tone(ToneParameter.Contrast, "contrast", "CON", "Contrast", TonemapGroup.Grade)
    val saturation = tone(ToneParameter.Saturation, "saturation", "SAT", "Saturation", TonemapGroup.Grade)
    val vibrance = tone(ToneParameter.Vibrance, "vibrance", "VIB", "Vibrance", TonemapGroup.Grade)
    val whites = tone(ToneParameter.Whites, "whites", "WHT", "Whites", TonemapGroup.Output)
    val blacks = tone(ToneParameter.Blacks, "blacks", "BLK", "Blacks", TonemapGroup.Output)

    /**
     * Canonical strip/settings order, grouped by where each control runs.
     * Display order only: the shader's processing order inside Grade differs.
     */
    val tone: List<TonemapParam> =
        listOf(highlights, shadows, midtones, contrast, saturation, vibrance, whites, blacks)

    /** Every tone control (render exposure included), in canonical order. */
    val all: List<TonemapParam> = listOf(renderExposure) + tone

    private val byParameter = tone.associateBy { requireNotNull(it.param) }

    fun forParameter(parameter: ToneParameter): TonemapParam = byParameter.getValue(parameter)

    fun valueOf(state: CaptureUiState, parameter: ToneParameter): Int = state.toneValueFor(parameter)

    /** True when [parameter] sits at its default in [state]. */
    fun isDefault(state: CaptureUiState, parameter: ToneParameter): Boolean =
        valueOf(state, parameter) == forParameter(parameter).defaultValue.roundToInt()

    private fun tone(
        param: ToneParameter,
        jsonKey: String,
        shortLabel: String,
        longLabel: String,
        group: TonemapGroup
    ) = TonemapParam(
        param = param,
        jsonKey = jsonKey,
        shortLabel = shortLabel,
        longLabel = longLabel,
        minimum = TonemapControlContract.TONE_UI_MIN,
        maximum = TonemapControlContract.TONE_UI_MAX,
        defaultValue = TonemapControlContract.TONE_UI_NEUTRAL,
        step = 1f,
        decimals = 0,
        group = group
    )
}
