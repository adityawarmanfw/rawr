package com.rawr.camera.model

import kotlin.math.roundToInt

/**
 * Single owner of exposure/focus display formatting.
 *
 * Previously duplicated between NativeCapabilityProjection (candidate labels)
 * and NativeCaptureScreenController (applied readouts); the branches are
 * identical, so both call sites share these verbatim.
 */
internal object ExposureFormat {
    fun formatShutterDegree(angleDeg: Double): String {
        if (angleDeg > 0.0 && angleDeg < 0.001) return "<0.001°"
        val label = java.math.BigDecimal.valueOf(angleDeg)
            .setScale(5, java.math.RoundingMode.HALF_UP)
            .stripTrailingZeros()
            .toPlainString()
        return "${label}°"
    }

    fun formatShutterNs(ns: Long): String {
        val seconds = ns / 1_000_000_000.0
        return if (seconds < 0.5) {
            "1/${(1.0 / seconds).roundToInt().coerceAtLeast(1)}"
        } else if (seconds < 10.0) {
            "%.1fs".format(seconds).replace(".0s", "s")
        } else {
            "${seconds.roundToInt()}s"
        }
    }

    fun formatEv(ev: Double): String = "%+.1f".format(ev)

    /** Lens readouts at or below this (≥ 100 m) read as ∞; HALs rarely report exactly 0 D. */
    const val INFINITY_DIOPTERS = .01f

    fun formatFocusDistance(diopters: Float): String = when {
        diopters <= INFINITY_DIOPTERS -> {
            "∞"
        }

        else -> {
            val meters = 1f / diopters
            if (meters < 1f) "%.2f m".format(meters) else "%.1f m".format(meters)
        }
    }
}
