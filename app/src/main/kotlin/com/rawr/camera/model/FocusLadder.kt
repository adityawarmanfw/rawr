package com.rawr.camera.model

import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sign

enum class FocusAnchor { Near, Hyperfocal, Infinity }

/** A magnetic focus stop. [normalized] uses the native MF scale: 0 = closest, 1 = ∞ (0 D). */
data class FocusStop(val normalized: Float, val diopters: Float, val anchor: FocusAnchor)

/** Live scrub position: diopters, plus the anchor currently holding it and drag spent escaping. */
data class FocusScrubPosition(val diopters: Float, val heldAnchor: FocusStop? = null, val escapeDp: Float = 0f)

/**
 * Continuous manual-focus scrub scale. Drag maps to diopters with a gain that
 * depends on where the lens is: fine through the macro end, faster across the
 * middle, fine again in the far zone. Exact magnetic anchors sit at NEAR,
 * hyperfocal and ∞ (exactly 0 D); leaving one costs [ANCHOR_RELEASE_DP].
 * Continuous (not quantized) so a drag starts exactly at the current lens
 * position and never jumps.
 */
class FocusLadder private constructor(val minimumFocusDistance: Float, val anchors: List<FocusStop>) {
    private val macroStart = minimumFocusDistance * (1f - MACRO_ZONE_FRACTION)

    fun normalizedOf(diopters: Float): Float =
        if (diopters <= 0f) 1f else (1f - diopters / minimumFocusDistance).coerceIn(0f, 1f)

    fun dioptersOf(normalized: Float): Float = ((1f - normalized) * minimumFocusDistance).coerceIn(0f, minimumFocusDistance)

    /** Diopters moved per dp of drag at [diopters]. */
    fun dioptersPerDp(diopters: Float): Float = when {
        diopters < FAR_ZONE_DIOPTERS -> FAR_DIOPTERS_PER_DP
        diopters >= macroStart -> minimumFocusDistance * MACRO_GAIN_FRACTION
        else -> minimumFocusDistance * MID_GAIN_FRACTION
    }

    fun anchorNear(normalized: Float, radius: Float): FocusStop? =
        anchors.filter { abs(it.normalized - normalized) <= radius }.minByOrNull { abs(it.normalized - normalized) }

    fun start(diopters: Float): FocusScrubPosition {
        val d = if (diopters.isFinite()) diopters.coerceIn(0f, minimumFocusDistance) else minimumFocusDistance
        return FocusScrubPosition(d, anchors.firstOrNull { it.diopters == d })
    }

    /**
     * Advances by [deltaDp] (positive = farther, i.e. fewer diopters). Returns
     * the new position and whether it just landed on an anchor (haptic cue).
     */
    fun scrub(from: FocusScrubPosition, deltaDp: Float): Pair<FocusScrubPosition, Boolean> {
        var d = from.diopters
        var remaining = deltaDp
        from.heldAnchor?.let { held ->
            // Pushing outward past an end stop never banks escape distance.
            val outward = (held.anchor == FocusAnchor.Infinity && remaining > 0f) ||
                (held.anchor == FocusAnchor.Near && remaining < 0f)
            val escape = if (outward) 0f else from.escapeDp + remaining
            if (abs(escape) < ANCHOR_RELEASE_DP) return from.copy(escapeDp = escape) to false
            remaining = escape - sign(escape) * ANCHOR_RELEASE_DP
        }
        while (remaining != 0f) {
            val stepDp = sign(remaining) * min(1f, abs(remaining))
            val next = (d - stepDp * dioptersPerDp(d)).coerceIn(0f, minimumFocusDistance)
            val lo = min(d, next)
            val hi = max(d, next)
            // Only anchors ahead of the start count: one the lens sits on is being left.
            val hit = anchors
                .filter { it.diopters in lo..hi && it.diopters != d }
                .minByOrNull { abs(it.diopters - d) }
            if (hit != null) return FocusScrubPosition(hit.diopters, hit) to true
            if (next == d) break
            d = next
            remaining -= stepDp
        }
        return FocusScrubPosition(d) to false
    }

    companion object {
        /** Beyond 2 m (diopters below this) is the far zone. */
        const val FAR_ZONE_DIOPTERS = .5f
        const val FAR_DIOPTERS_PER_DP = .004f
        /** Top share of the diopter range treated as macro. */
        const val MACRO_ZONE_FRACTION = .4f
        const val MACRO_GAIN_FRACTION = .002f
        const val MID_GAIN_FRACTION = .004f
        /** Drag needed to pull off a magnetic anchor. */
        const val ANCHOR_RELEASE_DP = 10f

        private val cache = HashMap<Pair<Float, Float>, FocusLadder>()

        fun forCapability(capability: ManualFocusCapability): FocusLadder? =
            if (!capability.supported || !(capability.minimumFocusDistance > 0f) ||
                !capability.minimumFocusDistance.isFinite()
            ) {
                null
            } else {
                synchronized(cache) {
                    cache.getOrPut(capability.minimumFocusDistance to capability.hyperfocalDistance) {
                        build(capability.minimumFocusDistance, capability.hyperfocalDistance)
                    }
                }
            }

        fun build(minimumFocusDistance: Float, hyperfocalDistance: Float = 0f): FocusLadder {
            require(minimumFocusDistance > 0f && minimumFocusDistance.isFinite())
            val m = minimumFocusDistance
            val anchors = mutableListOf(FocusStop(0f, m, FocusAnchor.Near), FocusStop(1f, 0f, FocusAnchor.Infinity))
            // A hyperfocal this close to ∞ (common on mains, e.g. 52 m) is ∞ in practice.
            if (hyperfocalDistance.isFinite() && hyperfocalDistance > MIN_HYPERFOCAL_DIOPTERS && hyperfocalDistance < m * .9f) {
                anchors += FocusStop(1f - hyperfocalDistance / m, hyperfocalDistance, FocusAnchor.Hyperfocal)
            }
            return FocusLadder(m, anchors.sortedBy { it.normalized })
        }

        private const val MIN_HYPERFOCAL_DIOPTERS = .05f
    }
}
