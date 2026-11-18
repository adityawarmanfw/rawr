package com.rawr.camera.ui

import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin
import kotlin.test.*

class LevelTrackerTest {
    private fun settled(x: Float, y: Float, z: Float, tracker: LevelTracker = LevelTracker()): LevelReading {
        var r: LevelReading? = null
        repeat(80) { r = tracker.update(x, y, z) }
        return r!!
    }

    private fun deg(d: Double) = d * PI / 180.0

    @Test fun uprightPortraitIsLevelAtTheHorizon() {
        val r = settled(0f, 1f, 0f)
        assertEquals(0f, r.rollDeg, 0.01f)
        assertEquals(0f, r.elevationDeg, 0.01f)
        assertEquals(0, r.quadrant)
        assertTrue(r.rollLevel && r.pitchLevel && !r.flat)
    }

    @Test fun clockwiseTurnGivesNegativeRoll() {
        // Phone turned 10 degrees clockwise: world-up leans toward the device's left (-x).
        val r = settled(-sin(deg(10.0)).toFloat(), cos(deg(10.0)).toFloat(), 0f)
        assertEquals(-10f, r.rollDeg, 0.05f)
        assertFalse(r.rollLevel)
    }

    @Test fun landscapeKeepsSmallDeviationAndReportsQuadrant() {
        val r = settled(cos(deg(2.0)).toFloat(), sin(deg(2.0)).toFloat(), 0f)
        assertEquals(90, r.quadrant)
        assertEquals(-2f, r.rollDeg, 0.05f)
    }

    @Test fun lensAtSkyIsPositiveElevationAndSwitchesToBubble() {
        val r = settled(0f, 0f, -1f)
        assertEquals(90f, r.elevationDeg, 0.01f)
        assertTrue(r.flat)
        assertEquals(1f, r.flatBlend, 1e-6f)
        assertTrue(r.tiltLevel)
    }

    @Test fun bubbleMovesTowardTheHighSide() {
        // Lens at the ground, top edge of the phone raised: world-up leans toward +y.
        val r = settled(0f, sin(deg(5.0)).toFloat(), cos(deg(5.0)).toFloat())
        assertTrue(r.flat)
        assertTrue(r.bubbleYDeg < -4f, "dot rides toward the top of the screen")
        assertEquals(0f, r.bubbleXDeg, 0.01f)
    }

    @Test fun lockBandHasHysteresis() {
        val tracker = LevelTracker()
        assertTrue(settled(0f, 1f, 0f, tracker).rollLevel)
        // 1.2 degrees: past the 0.8 enter band but inside the 1.5 exit band, stays locked.
        assertTrue(settled(-sin(deg(1.2)).toFloat(), cos(deg(1.2)).toFloat(), 0f, tracker).rollLevel)
        assertFalse(settled(-sin(deg(2.0)).toFloat(), cos(deg(2.0)).toFloat(), 0f, tracker).rollLevel)
        // Coming back it must reach 0.8 before it locks again.
        assertFalse(settled(-sin(deg(1.2)).toFloat(), cos(deg(1.2)).toFloat(), 0f, tracker).rollLevel)
        assertTrue(settled(-sin(deg(0.5)).toFloat(), cos(deg(0.5)).toFloat(), 0f, tracker).rollLevel)
    }

    @Test fun pitchLocksAtTheHorizonAndReportsSkyAndGround() {
        assertTrue(settled(0f, 1f, 0f).pitchLevel)
        // Lens tipped 12 degrees up: back of the phone leans away from the viewer.
        val up = settled(0f, cos(deg(12.0)).toFloat(), -sin(deg(12.0)).toFloat())
        assertEquals(12f, up.elevationDeg, 0.05f)
        assertFalse(up.pitchLevel)
        val down = settled(0f, cos(deg(12.0)).toFloat(), sin(deg(12.0)).toFloat())
        assertEquals(-12f, down.elevationDeg, 0.05f)
    }

    @Test fun invalidSampleIsIgnored() {
        assertNull(LevelTracker().update(0f, 0f, 0f))
    }

    @Test fun displayEasesFromHorizonToSkyOverSixtyToSeventyFiveDegrees() {
        fun blendAt(elev: Double) = settled(0f, cos(deg(elev)).toFloat(), -sin(deg(elev)).toFloat()).flatBlend
        assertEquals(0f, blendAt(0.0), 1e-6f)
        assertEquals(0f, blendAt(60.0), 1e-3f)
        val mid = blendAt(67.5)
        assertEquals(0.5f, mid, 0.01f)
        assertEquals(1f, blendAt(75.0), 1e-3f)
        assertEquals(1f, blendAt(90.0), 1e-6f)
        var prev = 0f
        for (e in 0..90) {
            val b = blendAt(e.toDouble())
            assertTrue(b >= prev - 1e-6f, "blend must never decrease with elevation ($e deg)")
            assertTrue(b - prev < 0.11f, "blend must not jump ($e deg)")
            prev = b
        }
    }

    @Test fun groundEasesTheSameWayAsSky() {
        val sky = settled(0f, cos(deg(70.0)).toFloat(), -sin(deg(70.0)).toFloat()).flatBlend
        val ground = settled(0f, cos(deg(70.0)).toFloat(), sin(deg(70.0)).toFloat()).flatBlend
        assertEquals(sky, ground, 1e-6f)
    }
}
