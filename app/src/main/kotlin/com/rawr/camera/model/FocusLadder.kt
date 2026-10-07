package com.rawr.camera.model

import kotlin.math.abs
import kotlin.math.max

enum class FocusAnchor { Near, Hyperfocal, Infinity }

/** One scrub stop. [normalized] uses the native MF scale: 0 = closest, 1 = ∞ (0 D). */
data class FocusStop(val normalized: Float, val diopters: Float, val anchor: FocusAnchor? = null)

/**
 * Discrete manual-focus scrub positions. Every stop costs the same drag
 * distance, so the diopter gap between stops *is* the sensitivity: fine
 * through the macro end, coarse across the middle, medium in the far zone,
 * with exact magnetic anchors at NEAR, hyperfocal and ∞ (exactly 0 D).
 */
class FocusLadder private constructor(val stops: List<FocusStop>) {
    val lastIndex: Int get() = stops.lastIndex

    fun isAnchor(index: Int): Boolean = stops.getOrNull(index)?.anchor != null

    fun indexNearest(normalized: Float): Int {
        var best = 0
        var bestDistance = Float.MAX_VALUE
        stops.forEachIndexed { i, stop ->
            val d = abs(stop.normalized - normalized)
            if (d < bestDistance) {
                best = i
                bestDistance = d
            }
        }
        return best
    }

    fun anchorNear(normalized: Float, radius: Float): FocusStop? =
        stops.filter { it.anchor != null && abs(it.normalized - normalized) <= radius }
            .minByOrNull { abs(it.normalized - normalized) }

    companion object {
        /** Beyond 2 m (diopters below this) is the far zone. */
        const val FAR_ZONE_DIOPTERS = .5f
        const val FAR_STEP_DIOPTERS = .05f
        /** Top share of the diopter range treated as macro. */
        const val MACRO_ZONE_FRACTION = .4f
        const val MACRO_STEP_FRACTION = .01f
        const val MACRO_STEP_MIN_DIOPTERS = .05f
        const val MID_STEP_FRACTION = .04f
        const val MID_STEP_MIN_DIOPTERS = .2f

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
            val macroStart = m * (1f - MACRO_ZONE_FRACTION)
            fun stepAt(d: Float): Float = when {
                d <= FAR_ZONE_DIOPTERS -> FAR_STEP_DIOPTERS
                d >= macroStart -> max(MACRO_STEP_MIN_DIOPTERS, m * MACRO_STEP_FRACTION)
                else -> max(MID_STEP_MIN_DIOPTERS, m * MID_STEP_FRACTION)
            }

            val anchors = mutableListOf(m to FocusAnchor.Near, 0f to FocusAnchor.Infinity)
            // A hyperfocal within one far step of ∞ (common on mains) is ∞ in practice.
            if (hyperfocalDistance.isFinite() && hyperfocalDistance > FAR_STEP_DIOPTERS &&
                hyperfocalDistance < m - stepAt(m)
            ) {
                anchors += hyperfocalDistance to FocusAnchor.Hyperfocal
            }

            val diopters = mutableListOf<Float>()
            var d = m - stepAt(m)
            while (d > 1e-4f) {
                diopters += d
                d -= stepAt(d)
            }
            // Ladder stops crowding an anchor would make it feel unreachable.
            val free = diopters.filter { v -> anchors.none { (a, _) -> abs(v - a) < stepAt(v) * .5f } }
            val stops = free.map { FocusStop(1f - it / m, it) } +
                anchors.map { (a, kind) -> FocusStop(if (kind == FocusAnchor.Infinity) 1f else 1f - a / m, a, kind) }
            return FocusLadder(stops.sortedBy { it.normalized })
        }
    }
}
