package com.rawr.camera.model

import com.rawr.camera.fixtures.CaptureFixtures
import com.rawr.camera.ui.detentScrub
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class FocusLadderTest {
    private fun gaps(ladder: FocusLadder, range: ClosedFloatingPointRange<Float>): List<Float> =
        ladder.stops.zipWithNext { a, b -> a.diopters - b.diopters }
            .filterIndexed { i, _ -> ladder.stops[i].diopters in range && ladder.stops[i + 1].diopters in range }

    @Test
    fun ladderRunsNearToInfinityWithExactAnchors() {
        val ladder = FocusLadder.build(5f)
        assertTrue(ladder.stops.zipWithNext().all { (a, b) -> a.normalized < b.normalized })
        assertEquals(FocusStop(0f, 5f, FocusAnchor.Near), ladder.stops.first())
        assertEquals(1f, ladder.stops.last().normalized)
        assertEquals(0f, ladder.stops.last().diopters)
        assertEquals(FocusAnchor.Infinity, ladder.stops.last().anchor)
        assertTrue(ladder.isAnchor(ladder.lastIndex))
    }

    @Test
    fun macroStepsAreFinerThanMidRange() {
        val ladder = FocusLadder.build(5f)
        val macro = gaps(ladder, 3f..5f).max()
        val mid = gaps(ladder, .6f..2.9f).min()
        assertTrue(macro < mid, "macro $macro mid $mid")
    }

    @Test
    fun hyperfocalAnchorOnlyWhenMeaningful() {
        // V2562 main reports 0.019 D: effectively ∞, so no separate anchor.
        assertNull(FocusLadder.build(5f, .019f).stops.firstOrNull { it.anchor == FocusAnchor.Hyperfocal })
        val wide = FocusLadder.build(16.67f, .3f)
        val hyperfocal = wide.stops.single { it.anchor == FocusAnchor.Hyperfocal }
        assertEquals(.3f, hyperfocal.diopters)
        assertEquals(1f - .3f / 16.67f, hyperfocal.normalized, 1e-6f)
    }

    @Test
    fun indexNearestRoundTrips() {
        val ladder = FocusLadder.build(6.67f, .24f)
        ladder.stops.forEachIndexed { i, stop -> assertEquals(i, ladder.indexNearest(stop.normalized)) }
        assertEquals(ladder.lastIndex, ladder.indexNearest(1f))
    }

    @Test
    fun scrubReachesInfinityAndHoldsOnAnchor() {
        val ladder = FocusLadder.build(5f)
        var index = ladder.indexNearest(.9f)
        var offset = 0f
        val visited = mutableListOf<Int>()
        repeat(200) {
            val r = detentScrub(index, offset, 3f, ladder.lastIndex, 3f, 6.75f, true, ladder::isAnchor) { i, _ ->
                visited += i
            }
            index = r.index
            offset = r.trackOffsetPx
        }
        assertEquals(ladder.lastIndex, index)
        assertTrue(offset < 3f, "rubber-banded past ∞")
        // Leaving the ∞ anchor costs the magnetic release, not a fine step.
        val back = detentScrub(index, 0f, -3f, ladder.lastIndex, 3f, 6.75f, true, ladder::isAnchor) { _, _ -> }
        assertEquals(ladder.lastIndex, back.index)
    }

    @Test
    fun focusLabelUsesRequestedInManualAndAppliedInAuto() {
        val caps = CaptureFixtures.baseline().copy(
            manualFocus = ManualFocusCapability(supported = true, distanceReadoutTrustworthy = true, minimumFocusDistance = 5f)
        )
        val base = CaptureUiState(caps)
        val mfInfinity = base.copy(focus = FocusUiState(mode = FocusMode.Mf, mfNormalized = 1f, appliedFocusDiopters = .03f))
        assertEquals("∞", mfInfinity.focusDistanceLabel())
        val mfHalf = base.copy(focus = FocusUiState(mode = FocusMode.Mf, mfNormalized = .5f))
        assertEquals(ExposureFormat.formatFocusDistance(2.5f), mfHalf.focusDistanceLabel())
        val afFar = base.copy(focus = FocusUiState(mode = FocusMode.Af, appliedFocusDiopters = .005f))
        assertEquals("∞", afFar.focusDistanceLabel())
        val untrusted = CaptureUiState(CaptureFixtures.baseline())
            .copy(focus = FocusUiState(mode = FocusMode.Mf, mfNormalized = .25f))
        assertEquals("25%", untrusted.focusDistanceLabel())
    }
}
