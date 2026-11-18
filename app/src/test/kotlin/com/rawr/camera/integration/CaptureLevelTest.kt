package com.rawr.camera.integration

import com.rawr.camera.ui.LevelReading
import com.rawr.camera.ui.LevelTracker
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin
import kotlin.test.*

class CaptureLevelTest {
    private fun settled(x: Float, y: Float, z: Float): LevelReading {
        val tracker = LevelTracker()
        var r: LevelReading? = null
        repeat(80) { r = tracker.update(x, y, z) }
        return r!!
    }

    private fun deg(d: Double) = d * PI / 180.0

    @Test fun clockwiseTurnIsRecordedAsPositiveRoll() {
        // Phone turned 10 degrees clockwise: the display's roll is -10, the file records +10.
        val c = settled(-sin(deg(10.0)).toFloat(), cos(deg(10.0)).toFloat(), 0f).toCaptureLevel()
        assertEquals(10f, c.rollDeg!!, 0.05f)
        assertEquals(0f, c.pitchDeg, 0.05f)
    }

    @Test fun lensAboveTheHorizonIsPositivePitch() {
        val c = settled(0f, cos(deg(12.0)).toFloat(), -sin(deg(12.0)).toFloat()).toCaptureLevel()
        assertEquals(12f, c.pitchDeg, 0.05f)
        val down = settled(0f, cos(deg(12.0)).toFloat(), sin(deg(12.0)).toFloat()).toCaptureLevel()
        assertEquals(-12f, down.pitchDeg, 0.05f)
    }

    @Test fun rollIsDroppedWhenTheLensPointsNearVertical() {
        val c = settled(0.02f, 0.01f, -1f).toCaptureLevel()
        assertNull(c.rollDeg)
        assertEquals(90f, c.pitchDeg, 2f)
    }

    @Test fun nativeFormCarriesNaNForAbsentRoll() {
        val full = CaptureLevel(3.2f, 1.5f).toNative()
        assertEquals(listOf(3.2f, 1.5f), full.toList())
        val flat = CaptureLevel(88f, null).toNative()
        assertEquals(88f, flat[0])
        assertTrue(flat[1].isNaN())
    }
}
