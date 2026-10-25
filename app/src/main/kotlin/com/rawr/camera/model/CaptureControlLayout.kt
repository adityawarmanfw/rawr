package com.rawr.camera.model

/**
 * Capture-screen control arrangement. Classic = viewfinder rails; Compact = button strip by the shutter; Pro = Gcam
 * style: one ruler for the selected value, a single row of live value tiles, and dedicated FILTERS and PARAMS buttons.
 */
enum class CaptureControlLayout {
    Classic,
    Compact,
    Pro;

    /** Compact and Pro share the button-strip geometry (no viewfinder rails). */
    val usesButtonStrip: Boolean get() = this != Classic
}
