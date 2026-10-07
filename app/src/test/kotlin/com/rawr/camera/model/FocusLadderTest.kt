package com.rawr.camera.model

import com.rawr.camera.fixtures.CaptureFixtures
import com.rawr.camera.ui.detentScrub
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class FocusLadderTest {
    private fun drag(ladder: FocusLadder, from: FocusScrubPosition, totalDp: Float, eventDp: Float = 2f): FocusScrubPosition {
        var p = from
        var left = totalDp
        while (left != 0f) {
            val d = if (kotlin.math.abs(left) < kotlin.math.abs(eventDp)) left else kotlin.math.sign(left) * kotlin.math.abs(eventDp)
            p = ladder.scrub(p, d).first
            left -= d
        }
        return p
    }

    @Test
    fun anchorsAreExactNearAndInfinity() {
        val ladder = FocusLadder.build(5f)
        assertEquals(listOf(FocusAnchor.Near, FocusAnchor.Infinity), ladder.anchors.map { it.anchor })
        assertEquals(1f, ladder.normalizedOf(0f))
        assertEquals(0f, ladder.anchors.last().diopters)
    }

    @Test
    fun dragStartsExactlyAtLensPosition() {
        val ladder = FocusLadder.build(5f)
        val start = ladder.start(1.2345f)
        assertEquals(1.2345f, start.diopters)
        assertNull(start.heldAnchor)
        val (moved, _) = ladder.scrub(start, 1f)
        assertTrue(kotlin.math.abs(moved.diopters - start.diopters) <= .02f, "one dp moves at most 0.02 D")
    }

    @Test
    fun macroIsFinerThanMidRange() {
        val ladder = FocusLadder.build(5f)
        assertTrue(ladder.dioptersPerDp(4f) < ladder.dioptersPerDp(1.5f))
        assertTrue(ladder.dioptersPerDp(.2f) < ladder.dioptersPerDp(1.5f))
    }

    @Test
    fun scrubSnapsToInfinityHoldsAndReleases() {
        val ladder = FocusLadder.build(5f)
        val atInfinity = drag(ladder, ladder.start(.3f), 1000f)
        assertEquals(0f, atInfinity.diopters)
        assertEquals(FocusAnchor.Infinity, atInfinity.heldAnchor?.anchor)
        // Pushing past ∞ banks nothing; a small pull back stays held.
        val stillHeld = drag(ladder, drag(ladder, atInfinity, 200f), -FocusLadder.ANCHOR_RELEASE_DP + 2f)
        assertEquals(0f, stillHeld.diopters)
        val released = drag(ladder, stillHeld, -10f)
        assertTrue(released.diopters > 0f)
        assertNull(released.heldAnchor)
    }

    @Test
    fun fullTraverseIsSeveralHundredDp() {
        val ladder = FocusLadder.build(5f)
        var p = ladder.start(5f)
        var dp = 0f
        while (p.heldAnchor?.anchor != FocusAnchor.Infinity) {
            p = ladder.scrub(p, 1f).first
            dp += 1f
        }
        assertTrue(dp in 350f..600f, "traverse $dp dp")
    }

    @Test
    fun hyperfocalAnchorOnlyWhenMeaningful() {
        // V2562 main reports 0.019 D (52 m): effectively ∞, so no separate anchor.
        assertNull(FocusLadder.build(5f, .019f).anchors.firstOrNull { it.anchor == FocusAnchor.Hyperfocal })
        val wide = FocusLadder.build(16.67f, .3f)
        val hyperfocal = wide.anchors.single { it.anchor == FocusAnchor.Hyperfocal }
        assertEquals(.3f, hyperfocal.diopters)
        // Dragging farther from 1 m catches the hyperfocal stop before ∞.
        var p = wide.start(1f)
        var firstLanding: FocusAnchor? = null
        while (firstLanding == null) {
            val (next, landed) = wide.scrub(p, 2f)
            if (landed) firstLanding = next.heldAnchor?.anchor
            p = next
        }
        assertEquals(FocusAnchor.Hyperfocal, firstLanding)
    }

    @Test
    fun detentScrubKeepsExposureStepping() {
        var steps = 0
        val r = detentScrub(0, 0f, 10f, 5, 3f, 6.75f, magnetic = false, isAnchor = { false }) { _, detent ->
            assertTrue(detent)
            steps++
        }
        assertEquals(3, steps)
        assertEquals(3, r.index)
        assertEquals(1f, r.trackOffsetPx)
        val end = detentScrub(5, 0f, 4f, 5, 3f, 6.75f, magnetic = true, isAnchor = { it == 5 }) { _, _ -> }
        assertEquals(5, end.index)
        assertFalse(end.trackOffsetPx > 1f)
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
