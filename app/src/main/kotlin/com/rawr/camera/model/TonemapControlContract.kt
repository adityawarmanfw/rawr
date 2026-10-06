package com.rawr.camera.model

import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * Application-facing Tonemap v2 control contract.
 *
 * Creative tonal controls are presented as conventional -100..+100 values.
 * Native TonemapEngine receives bounded semantic parameters with explicit units.
 */
object TonemapControlContract {
    const val EXPOSURE_MIN_EV = -5f
    const val EXPOSURE_MAX_EV = 5f
    const val EXPOSURE_NEUTRAL_EV = 0f
    const val EXPOSURE_MIN_TENTHS = -50
    const val EXPOSURE_MAX_TENTHS = 50

    const val TONE_UI_MIN = -100f
    const val TONE_UI_MAX = 100f
    const val TONE_UI_NEUTRAL = 0f

    const val FIXED_SHOULDER_START_EV = 2f

    const val SATURATION_MIN = -100f
    const val SATURATION_MAX = 100f
    const val SATURATION_NEUTRAL = 0f
    const val VIBRANCE_MIN = -100f
    const val VIBRANCE_MAX = 100f
    const val VIBRANCE_NEUTRAL = 0f

    // TonemapEngine now consumes the photographic controls directly as
    // conventional -100..+100 values. No hidden EV/exponent remapping.
    private fun toneNative(value: Float): Float = value.coerceIn(TONE_UI_MIN, TONE_UI_MAX)

    fun blackPointNativeFromUi(value: Float): Float = toneNative(value)

    fun whitePointNativeFromUi(value: Float): Float = toneNative(value)

    fun shadowNativeFromUi(value: Float): Float = toneNative(value)

    fun midtoneNativeFromUi(value: Float): Float = toneNative(value)

    fun contrastNativeFromUi(value: Float): Float = toneNative(value)

    fun highlightNativeFromUi(value: Float): Float = toneNative(value)

    // Legacy persisted native values were implementation-space coordinates.
    // Migration maps valid old ranges back to the normalized UI once; current
    // values are already normalized and bypass these helpers.
    fun blackUiFromLegacyNative(value: Float): Float =
        if (value in -12f..-8f) ((value + 10f) / 2f * 100f).coerceIn(TONE_UI_MIN, TONE_UI_MAX) else TONE_UI_NEUTRAL

    fun whiteUiFromLegacyNative(value: Float): Float =
        if (value in 3f..10f) ((6f - value) / 2f * 100f).coerceIn(TONE_UI_MIN, TONE_UI_MAX) else TONE_UI_NEUTRAL

    fun shadowFromQuickControl(value: Int): Float = value.coerceIn(-100, 100).toFloat()

    fun shadowToQuickControl(value: Float): Int = value.roundToInt().coerceIn(-100, 100)

    fun highlightFromQuickControl(value: Int): Float = value.coerceIn(-100, 100).toFloat()

    fun highlightToQuickControl(value: Float): Int = value.roundToInt().coerceIn(-100, 100)

    fun exposureEvFromTenths(tenths: Int): Float =
        tenths.coerceIn(EXPOSURE_MIN_TENTHS, EXPOSURE_MAX_TENTHS) / 10f
}

/** Render exposure quantized to the 0.1 EV slider step. */
fun renderExposureTenthsOf(ev: Float): Int =
    if (ev.isFinite()) {
        (ev * 10f).roundToInt().coerceIn(TonemapControlContract.EXPOSURE_MIN_TENTHS, TonemapControlContract.EXPOSURE_MAX_TENTHS)
    } else {
        0
    }
