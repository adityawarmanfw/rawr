package com.rawr.camera.settings.model

import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * AgX render profile controls. Preset ids and defaults mirror
 * `tonemap::agxPreset()` in native/tonemap_engine/include/tonemap/Agx.h;
 * native rebuilds the full darktable parameter set from the preset and
 * overrides only these five values.
 */
data class AgxValues(
    val whiteEv: Float = 6.5f,
    val shoulderPower: Float = 3.3f,
    val toePower: Float = 1.5f,
    val purity: Float = 1f,
    val preserveHue: Float = 0.6f
) {
    fun valueOf(parameter: AgxParameter): Float = when (parameter) {
        AgxParameter.WhiteEv -> whiteEv
        AgxParameter.ShoulderPower -> shoulderPower
        AgxParameter.ToePower -> toePower
        AgxParameter.Purity -> purity
        AgxParameter.PreserveHue -> preserveHue
    }

    fun with(parameter: AgxParameter, value: Float): AgxValues = when (parameter) {
        AgxParameter.WhiteEv -> copy(whiteEv = value)
        AgxParameter.ShoulderPower -> copy(shoulderPower = value)
        AgxParameter.ToePower -> copy(toePower = value)
        AgxParameter.Purity -> copy(purity = value)
        AgxParameter.PreserveHue -> copy(preserveHue = value)
    }

    /** Native control order: white EV, shoulder, toe, purity, preserve hue. */
    fun toNativeArray(): FloatArray = floatArrayOf(whiteEv, shoulderPower, toePower, purity, preserveHue)
}

/** Native also has Blender base (id 1); the app does not offer it. Saved selections fall back to [Default]. */
enum class AgxPreset(val label: String, val nativeId: Int, val defaults: AgxValues) {
    Default("AgX", 0, AgxValues()),
    BlenderPunchy("AgX Punchy", 2, AgxValues(shoulderPower = 1.5f)),
    Sigmoid("AgX Sigmoid", 3, AgxValues(shoulderPower = 3.71f, toePower = 1.40f, preserveHue = 1f)),
    Smooth("AgX Smooth", 4, AgxValues(shoulderPower = 3.02f, toePower = 1.61f, purity = 0f, preserveHue = 0f))
}

enum class AgxParameter { WhiteEv, ShoulderPower, ToePower, Purity, PreserveHue }

data class AgxSpec(
    val parameter: AgxParameter,
    /** Stable recipe JSON key; native RendererJni reads the same names. */
    val jsonKey: String,
    val shortLabel: String,
    val longLabel: String,
    val minimum: Float,
    val maximum: Float,
    val step: Float,
    val decimals: Int,
    /** Shown as value * displayScale with [unit]. */
    val displayScale: Float = 1f,
    val unit: String = ""
) {
    /** Decimals of the stored value (percent controls store fractions). */
    val valueDecimals: Int get() = decimals + kotlin.math.log10(displayScale).roundToInt()

    fun clamp(value: Float): Float = value.coerceIn(minimum, maximum)

    fun format(value: Float): String {
        val shown = value * displayScale
        val text = if (decimals == 0) shown.roundToInt().toString() else "%.${decimals}f".format(java.util.Locale.US, shown)
        return text + unit
    }

    fun isDefault(value: Float, default: Float): Boolean = abs(value - default) <= step * 0.5f
}

/** Single source for the AgX sliders in the capture strip, Settings and recipes. */
object AgxCatalog {
    val whiteEv = AgxSpec(AgxParameter.WhiteEv, "whiteEv", "RNG", "Highlight Range", 3f, 12f, 0.1f, 1, unit = " EV")
    val shoulder = AgxSpec(AgxParameter.ShoulderPower, "shoulderPower", "SHL", "Highlight Softness", 1f, 6f, 0.05f, 2)
    val toe = AgxSpec(AgxParameter.ToePower, "toePower", "TOE", "Shadow Softness", 0.5f, 4f, 0.05f, 2)
    val purity = AgxSpec(AgxParameter.Purity, "purity", "PUR", "Purity", 0f, 2f, 0.01f, 0, 100f, "%")
    val preserveHue = AgxSpec(AgxParameter.PreserveHue, "preserveHue", "HUE", "Preserve Hue", 0f, 1f, 0.01f, 0, 100f, "%")

    val all: List<AgxSpec> = listOf(whiteEv, shoulder, toe, purity, preserveHue)

    fun spec(parameter: AgxParameter): AgxSpec = all.first { it.parameter == parameter }

    fun sanitized(values: AgxValues, fallback: AgxValues): AgxValues = all.fold(values) { acc, spec ->
        val v = acc.valueOf(spec.parameter)
        acc.with(spec.parameter, if (v.isFinite()) spec.clamp(v) else fallback.valueOf(spec.parameter))
    }
}
