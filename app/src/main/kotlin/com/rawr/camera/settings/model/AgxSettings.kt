package com.rawr.camera.settings.model

import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.log10
import kotlin.math.roundToInt

/**
 * AgX render profile controls: every darktable AgX parameter except its
 * desktop aids (pickers, auto tune, dynamic range scaling, keep pivot on
 * diagonal, base primaries). Order, JSON keys and preset rows mirror
 * `tonemap::AgxControl`, `kAgxControlKeys` and `kAgxPresetControls` in
 * native/tonemap_engine/include/tonemap/Agx.h (AgxPresetSyncTest checks).
 */
enum class AgxSection(val label: String) { Range("RANGE"), Curve("CURVE"), Look("LOOK"), Primaries("PRIM") }

/** Strip/Settings sub-groups; [label] is the strip's L2 tab. */
enum class AgxGroup(val section: AgxSection, val label: String, val title: String) {
    Range(AgxSection.Range, "EXPOSURE", "Input Exposure Range"),
    Curve(AgxSection.Curve, "BASIC", "Curve"),
    AdvancedCurve(AgxSection.Curve, "ADVANCED", "Advanced Curve"),
    Look(AgxSection.Look, "LOOK", "Look"),
    Before(AgxSection.Primaries, "BEFORE", "Primaries Before Tone Mapping"),
    After(AgxSection.Primaries, "AFTER", "Primaries After Tone Mapping")
}

enum class AgxParameter {
    WhiteEv, BlackEv,
    Contrast, ShoulderPower, ToePower, PivotEv, PivotOutput, ShoulderStart, ToeStart, TargetWhite, TargetBlack, Gamma,
    LookSlope, LookLift, LookBrightness, LookSaturation, PreserveHue,
    RedInset, RedRotation, GreenInset, GreenRotation, BlueInset, BlueRotation,
    MasterOutset, MasterUnrotation, RedOutset, RedUnrotation, GreenOutset, GreenUnrotation, BlueOutset, BlueUnrotation
}

/** All AgX controls in [AgxParameter] (= native) order. */
data class AgxValues(val values: List<Float>) {
    init {
        require(values.size == AgxParameter.entries.size)
    }

    fun valueOf(parameter: AgxParameter): Float = values[parameter.ordinal]

    fun with(parameter: AgxParameter, value: Float): AgxValues =
        AgxValues(values.toMutableList().also { it[parameter.ordinal] = value })

    fun toNativeArray(): FloatArray = values.toFloatArray()
}

private fun agx(vararg v: Float) = AgxValues(v.toList())

/** Native also has Blender base (id 1); the app does not offer it. Saved selections fall back to [Default]. */
enum class AgxPreset(val label: String, val nativeId: Int, val defaults: AgxValues) {
    Default("AgX", 0, agx(
        6.5f, -10.0f,
        3.0f, 3.3f, 1.5f, 0.0f, 0.18f, 0.0f, 0.0f, 1.0f, 0.0f, 2.2f,
        1.0f, 0.0f, 1.0f, 1.0f, 0.6f,
        0.2946245f, 0.03540329f, 0.25861925f, -0.02108586f, 0.14641371f, -0.06305724f,
        1.0f, 0.0f, 0.2907764f, 0.03540329f, 0.2631554f, -0.02108586f, 0.04581072f, -0.06305724f)),
    BlenderPunchy("AgX Punchy", 2, agx(
        6.5f, -10.0f,
        2.4535246f, 1.5f, 1.5f, 0.0f, 0.18f, 0.0f, 0.0f, 1.0f, 0.0f, 2.4f,
        1.0f, 0.0f, 0.5486968f, 1.4f, 0.6f,
        0.2946245f, 0.03540329f, 0.25861925f, -0.02108586f, 0.14641371f, -0.06305724f,
        1.0f, 0.0f, 0.2907764f, 0.03540329f, 0.2631554f, -0.02108586f, 0.04581072f, -0.06305724f)),
    Sigmoid("AgX Sigmoid", 3, agx(
        6.5f, -10.0f,
        3.07f, 3.71f, 1.4f, 0.0f, 0.18f, 0.0f, 0.0f, 1.0f, 0.0f, 2.2f,
        1.0f, 0.0f, 1.0f, 1.0f, 1.0f,
        0.2946245f, 0.03540329f, 0.25861925f, -0.02108586f, 0.14641371f, -0.06305724f,
        1.0f, 0.0f, 0.2907764f, 0.03540329f, 0.2631554f, -0.02108586f, 0.04581072f, -0.06305724f)),
    Smooth("AgX Smooth", 4, agx(
        6.5f, -10.0f,
        2.98f, 3.02f, 1.61f, 0.0f, 0.18f, 0.0f, 0.0f, 1.0f, 0.0f, 2.2f,
        1.0f, 0.0f, 1.0f, 1.0f, 0.0f,
        0.1f, 0.034906585f, 0.1f, -0.017453292f, 0.15f, -0.05235988f,
        0.0f, 1.0f, 0.1f, 0.034906585f, 0.1f, -0.017453292f, 0.15f, -0.05235988f))
}

data class AgxSpec(
    val parameter: AgxParameter,
    val group: AgxGroup,
    /** Recipe JSON key; matches native kAgxControlKeys. */
    val jsonKey: String,
    val shortLabel: String,
    val longLabel: String,
    val minimum: Float,
    val maximum: Float,
    val step: Float,
    /** Decimals of the displayed (scaled) value. */
    val decimals: Int,
    /** Shown as value * displayScale with [unit]. */
    val displayScale: Float = 1f,
    val unit: String = ""
) {
    /** Decimals of the stored value (percent and degree controls store fractions and radians). */
    val valueDecimals: Int get() = decimals + ceil(log10(displayScale.toDouble()) - 1e-6).toInt().coerceAtLeast(0)

    /** EV units are implied in the compact strip cells. */
    val stripUnit: String get() = if (unit == " EV") "" else unit

    fun clamp(value: Float): Float = value.coerceIn(minimum, maximum)

    fun format(value: Float, unit: String = this.unit): String {
        val shown = value * displayScale
        val text = if (decimals == 0) shown.roundToInt().toString() else "%.${decimals}f".format(java.util.Locale.US, shown)
        val signed = if (minimum < 0f && maximum > 0f && !text.startsWith("-")) "+$text" else text
        return signed + unit
    }

    fun isDefault(value: Float, default: Float): Boolean = abs(value - default) <= step * 0.5f
}

/**
 * Single source for the AgX controls in the capture strip, Settings and
 * recipes. Ranges are darktable's slider soft ranges (widened where a preset
 * or phone use needs it); native clamps to darktable's hard limits.
 */
object AgxCatalog {
    private const val DEG = 57.29578f
    private const val ROT = 0.5236f // ±30°, darktable's limit
    private fun pct(p: AgxParameter, g: AgxGroup, key: String, short: String, long: String, max: Float = 0.99f,
                    min: Float = 0f, step: Float = 0.01f, decimals: Int = 0) =
        AgxSpec(p, g, key, short, long, min, max, step, decimals, 100f, "%")
    private fun deg(p: AgxParameter, g: AgxGroup, key: String, short: String, long: String) =
        AgxSpec(p, g, key, short, long, -ROT, ROT, 0.5f / DEG, 1, DEG, "°")

    val all: List<AgxSpec> = listOf(
        AgxSpec(AgxParameter.WhiteEv, AgxGroup.Range, "whiteEv", "WHITE", "White Relative Exposure", 1f, 12f, 0.1f, 1, unit = " EV"),
        AgxSpec(AgxParameter.BlackEv, AgxGroup.Range, "blackEv", "BLACK", "Black Relative Exposure", -20f, -1f, 0.1f, 1, unit = " EV"),
        AgxSpec(AgxParameter.Contrast, AgxGroup.Curve, "contrast", "CON", "Contrast", 0.1f, 5f, 0.01f, 2),
        AgxSpec(AgxParameter.ShoulderPower, AgxGroup.Curve, "shoulderPower", "SHLDR", "Shoulder Power", 1f, 5f, 0.01f, 2),
        AgxSpec(AgxParameter.ToePower, AgxGroup.Curve, "toePower", "TOE", "Toe Power", 1f, 5f, 0.01f, 2),
        AgxSpec(AgxParameter.PivotEv, AgxGroup.Curve, "pivotEv", "PIVOT", "Pivot Relative Exposure", -5f, 5f, 0.05f, 2, unit = " EV"),
        pct(AgxParameter.PivotOutput, AgxGroup.Curve, "pivotOutput", "PIV.Y", "Pivot Target Output", 0.5f, 0.05f, 0.001f, 1),
        pct(AgxParameter.ShoulderStart, AgxGroup.AdvancedCurve, "shoulderStart", "SH.ST", "Shoulder Start", max = 1f),
        pct(AgxParameter.ToeStart, AgxGroup.AdvancedCurve, "toeStart", "TOE.ST", "Toe Start", max = 1f),
        pct(AgxParameter.TargetWhite, AgxGroup.AdvancedCurve, "targetWhite", "T.WHT", "Target White", 1f, 0.5f, 0.005f, 1),
        pct(AgxParameter.TargetBlack, AgxGroup.AdvancedCurve, "targetBlack", "T.BLK", "Target Black", 0.025f, 0f, 0.0005f, 2),
        AgxSpec(AgxParameter.Gamma, AgxGroup.AdvancedCurve, "gamma", "GAMMA", "Curve Y Gamma", 1f, 5f, 0.01f, 2),
        AgxSpec(AgxParameter.LookSlope, AgxGroup.Look, "lookSlope", "SLOPE", "Slope", 0f, 2f, 0.01f, 2),
        AgxSpec(AgxParameter.LookLift, AgxGroup.Look, "lookLift", "LIFT", "Lift", -0.5f, 0.5f, 0.01f, 2),
        AgxSpec(AgxParameter.LookBrightness, AgxGroup.Look, "lookBrightness", "BRIGHT", "Brightness", 0.1f, 2f, 0.01f, 2),
        pct(AgxParameter.LookSaturation, AgxGroup.Look, "lookSaturation", "SAT", "Saturation", max = 2f),
        pct(AgxParameter.PreserveHue, AgxGroup.Look, "preserveHue", "HUE", "Preserve Hue", max = 1f),
        pct(AgxParameter.RedInset, AgxGroup.Before, "redAttenuation", "R ATT", "Red Attenuation"),
        deg(AgxParameter.RedRotation, AgxGroup.Before, "redRotation", "R ROT", "Red Rotation"),
        pct(AgxParameter.GreenInset, AgxGroup.Before, "greenAttenuation", "G ATT", "Green Attenuation"),
        deg(AgxParameter.GreenRotation, AgxGroup.Before, "greenRotation", "G ROT", "Green Rotation"),
        pct(AgxParameter.BlueInset, AgxGroup.Before, "blueAttenuation", "B ATT", "Blue Attenuation"),
        deg(AgxParameter.BlueRotation, AgxGroup.Before, "blueRotation", "B ROT", "Blue Rotation"),
        pct(AgxParameter.MasterOutset, AgxGroup.After, "purity", "PURITY", "Master Purity Boost", max = 2f),
        pct(AgxParameter.MasterUnrotation, AgxGroup.After, "rotationReversal", "REVERSE", "Master Rotation Reversal", max = 2f),
        pct(AgxParameter.RedOutset, AgxGroup.After, "redPurity", "R PUR", "Red Purity Boost"),
        deg(AgxParameter.RedUnrotation, AgxGroup.After, "redReverseRotation", "R REV", "Red Reverse Rotation"),
        pct(AgxParameter.GreenOutset, AgxGroup.After, "greenPurity", "G PUR", "Green Purity Boost"),
        deg(AgxParameter.GreenUnrotation, AgxGroup.After, "greenReverseRotation", "G REV", "Green Reverse Rotation"),
        pct(AgxParameter.BlueOutset, AgxGroup.After, "bluePurity", "B PUR", "Blue Purity Boost"),
        deg(AgxParameter.BlueUnrotation, AgxGroup.After, "blueReverseRotation", "B REV", "Blue Reverse Rotation")
    )

    fun spec(parameter: AgxParameter): AgxSpec = all[parameter.ordinal]

    fun group(group: AgxGroup): List<AgxSpec> = all.filter { it.group == group }

    fun byJsonKey(key: String): AgxSpec? = all.firstOrNull { it.jsonKey == key }

    /** Clamps every control to its slider range and keeps the pivot inside the exposure range. */
    fun sanitized(values: AgxValues, fallback: AgxValues): AgxValues {
        val clamped = all.fold(values) { acc, spec ->
            val v = acc.valueOf(spec.parameter)
            acc.with(spec.parameter, if (v.isFinite()) spec.clamp(v) else fallback.valueOf(spec.parameter))
        }
        val pivot = clamped.valueOf(AgxParameter.PivotEv)
            .coerceIn(clamped.valueOf(AgxParameter.BlackEv) + 0.5f, clamped.valueOf(AgxParameter.WhiteEv) - 0.5f)
        return clamped.with(AgxParameter.PivotEv, pivot)
    }

    /** Older saved entries held five controls: white EV, shoulder, toe, purity, preserve hue. */
    fun fromLegacy(five: List<Float>, preset: AgxPreset): AgxValues = preset.defaults
        .with(AgxParameter.WhiteEv, five[0])
        .with(AgxParameter.ShoulderPower, five[1])
        .with(AgxParameter.ToePower, five[2])
        .with(AgxParameter.MasterOutset, five[3])
        .with(AgxParameter.PreserveHue, five[4])
}
