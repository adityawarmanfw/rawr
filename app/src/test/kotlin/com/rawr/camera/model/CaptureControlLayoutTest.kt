package com.rawr.camera.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CaptureControlLayoutTest {
    @Test
    fun buttonStripLayoutsAreEveryLayoutButClassic() {
        assertTrue(CaptureControlLayout.Compact.usesButtonStrip)
        assertTrue(CaptureControlLayout.Pro.usesButtonStrip)
        assertFalse(CaptureControlLayout.Classic.usesButtonStrip)
    }

    @Test
    fun persistedNamesStillResolve() {
        assertEquals(CaptureControlLayout.Classic, CaptureControlLayout.valueOf("Classic"))
        assertEquals(CaptureControlLayout.Compact, CaptureControlLayout.valueOf("Compact"))
        assertEquals(CaptureControlLayout.Pro, CaptureControlLayout.valueOf("Pro"))
    }
}
