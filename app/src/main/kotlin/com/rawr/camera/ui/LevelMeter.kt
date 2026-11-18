package com.rawr.camera.ui

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.roundToInt
import kotlin.math.sqrt

/** One smoothed posture sample. Angles are degrees; the screen is portrait-locked. */
internal data class LevelReading(
    /** Horizon roll relative to the nearest screen quadrant, positive = phone turned counter-clockwise
     *  (a clockwise turn is negative; the capture metadata flips this, see CaptureLevelSensor). */
    val rollDeg: Float = 0f,
    /** Nearest 90-degree quadrant of the phone about the lens axis (0 = upright portrait). */
    val quadrant: Int = 0,
    /** Lens elevation: +90 = sky, 0 = horizon, -90 = ground. */
    val elevationDeg: Float = 0f,
    /**
     * 0 near the horizon, 1 when the lens points at the sky or ground, with a smooth ramp between
     * so the display changes gradually instead of switching at one angle.
     */
    val flatBlend: Float = 0f,
    /** Bubble offset toward the high side, in screen axes (x right, y down), degrees of tilt. */
    val bubbleXDeg: Float = 0f,
    val bubbleYDeg: Float = 0f,
    /** Roll is inside the lock band. */
    val rollLevel: Boolean = false,
    /** Elevation is inside the lock band. */
    val pitchLevel: Boolean = false,
    /** Total tilt from flat is inside the lock band (meaningful when [flat]). */
    val tiltLevel: Boolean = false
) {
    /** True once the sky/ground display is the dominant one. */
    val flat: Boolean get() = flatBlend >= 0.5f
}

/**
 * Turns gravity samples into [LevelReading]s. Holds the filter and the hysteresis memory, so one
 * instance serves one sensor stream. Input is the "world up" direction in device axes
 * (x right, y toward the top edge, z out of the screen); the lens looks along -z.
 */
internal class LevelTracker {
    private var ux = 0f
    private var uy = 1f
    private var uz = 0f
    private var primed = false
    private var rollLocked = false
    private var pitchLocked = false
    private var tiltLocked = false

    fun reset() {
        primed = false
        rollLocked = false
        pitchLocked = false
        tiltLocked = false
    }

    fun update(x: Float, y: Float, z: Float): LevelReading? {
        val n = sqrt(x * x + y * y + z * z)
        if (n < 1e-3f) return null
        val nx = x / n
        val ny = y / n
        val nz = z / n
        if (!primed) {
            ux = nx; uy = ny; uz = nz
            primed = true
        } else {
            // Heavier smoothing close to level so the line does not shimmer; light when moving.
            val a = if (abs(nx - ux) + abs(ny - uy) + abs(nz - uz) < 0.03f) 0.12f else 0.35f
            ux += a * (nx - ux)
            uy += a * (ny - uy)
            uz += a * (nz - uz)
            val m = sqrt(ux * ux + uy * uy + uz * uz)
            ux /= m; uy /= m; uz /= m
        }
        return read()
    }

    private fun read(): LevelReading {
        val elevation = asin((-uz).coerceIn(-1f, 1f)) * RAD
        val t = ((abs(elevation) - FLAT_BLEND_START_DEG) / (FLAT_BLEND_END_DEG - FLAT_BLEND_START_DEG))
            .coerceIn(0f, 1f)
        val blend = t * t * (3f - 2f * t)

        // Sky/ground view: tilt from flat and the direction of the high side.
        val len = hypot(ux, uy)
        val tilt = asin(len.coerceIn(0f, 1f)) * RAD
        val scale = if (len > 1e-4f) tilt / len else 0f
        tiltLocked = hysteresis(tiltLocked, tilt)

        // Horizon view: roll about the lens axis, relative to the nearest screen quadrant.
        val roll = atan2(ux, uy) * RAD
        val k = (roll / 90f).roundToInt()
        val quadrant = (k * 90 + 360) % 360
        val dev = roll - k * 90f
        rollLocked = hysteresis(rollLocked, dev)
        pitchLocked = hysteresis(pitchLocked, elevation)

        return LevelReading(
            rollDeg = dev,
            quadrant = quadrant,
            elevationDeg = elevation,
            flatBlend = blend,
            bubbleXDeg = ux * scale,
            bubbleYDeg = -uy * scale,
            rollLevel = rollLocked,
            pitchLevel = pitchLocked,
            tiltLevel = tiltLocked
        )
    }

    private fun hysteresis(locked: Boolean, angle: Float): Boolean =
        if (locked) abs(angle) <= LOCK_EXIT_DEG else abs(angle) <= LOCK_ENTER_DEG

    companion object {
        const val LOCK_ENTER_DEG = 0.8f
        const val LOCK_EXIT_DEG = 1.5f
        /** Elevation band over which the display eases from horizon to sky/ground. */
        const val FLAT_BLEND_START_DEG = 60f
        const val FLAT_BLEND_END_DEG = 75f
        private const val RAD = (180.0 / PI).toFloat()
    }
}
