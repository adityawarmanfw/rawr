package com.rawr.camera.model

import java.util.Locale

/** Lightweight capture-screen projection of the film-sim preset + full look. */
data class FilmSimQuickOption(val id: String, val label: String)

data class FilmSimQuickParam(
    val key: String,
    val shortLabel: String,
    val value: Float,
    val defaultValue: Float,
    val minimum: Float,
    val maximum: Float,
    val step: Float,
    val decimals: Int,
    val displayValue: String,
    val isDefault: Boolean,
    /** Live readout while scrubbing: value * displayScale, displayDecimals, unit (AgX % and °). */
    val displayScale: Float = 1f,
    val displayDecimals: Int = decimals,
    val unit: String = ""
)

data class FilmSimQuickDiscrete(
    val key: String,
    val shortLabel: String,
    val options: List<String>,
    val selectedIndex: Int
) {
    val displayValue: String get() = options.getOrElse(selectedIndex) { "?" }
}

data class FilmSimQuickFlag(
    val key: String,
    val shortLabel: String,
    val enabled: Boolean
)

sealed interface FilmStripItem {
    data class Numeric(val key: String) : FilmStripItem
    data class Discrete(val key: String) : FilmStripItem
    data class Flag(val key: String) : FilmStripItem
}

data class FilmSimQuickSubsection(val key: String, val label: String, val items: List<FilmStripItem>)

data class FilmSimQuickSection(val key: String, val label: String, val subsections: List<FilmSimQuickSubsection>) {
    /** All items across subsections, in order. */
    val items: List<FilmStripItem> get() = subsections.flatMap { it.items }
    /** Numeric keys in section order (legacy accessor for the compact cut). */
    val paramKeys: List<String> get() = items.filterIsInstance<FilmStripItem.Numeric>().map { it.key }
}

data class FilmSimQuickState(
    val activePresetName: String,
    val selectedPresetId: String?,
    val modified: Boolean,
    val presets: List<FilmSimQuickOption> = emptyList(),
    val sections: List<FilmSimQuickSection> = emptyList(),
    val params: Map<String, FilmSimQuickParam> = emptyMap(),
    val discretes: Map<String, FilmSimQuickDiscrete> = emptyMap(),
    val flags: Map<String, FilmSimQuickFlag> = emptyMap(),
    val grainEnabled: Boolean = false
) {
    companion object {
        fun empty() = FilmSimQuickState(
            activePresetName = "Custom",
            selectedPresetId = null,
            modified = false
        )
    }
}
/** Display formatting for strip values. Signed when the range spans negative; units live in the label. */
fun formatFilmStripValue(value: Float, minimum: Float, decimals: Int): String {
    val factor = Math.pow(10.0, decimals.coerceIn(0, 3).toDouble()).toFloat()
    val rounded = kotlin.math.round(value * factor) / factor
    // Normalize -0.0 to 0 so signed params never show "-0.0".
    val clean = if (rounded == 0f) 0f else rounded
    val body = "%.${decimals.coerceIn(0, 3)}f".format(Locale.US, clean)
    return if (minimum < 0f && clean >= 0f) "+$body" else body
}
