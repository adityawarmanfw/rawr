package com.rawr.camera.model

import kotlin.test.Test
import kotlin.test.assertEquals

class ExposureModeCycleTest {
    private val all = ExposureMode.entries.toSet()

    @Test
    fun stepsAutoShutterIsoManualAndWraps() {
        assertEquals(ExposureMode.ShutterPriority, nextExposureMode(ExposureMode.Auto, all))
        assertEquals(ExposureMode.IsoPriority, nextExposureMode(ExposureMode.ShutterPriority, all))
        assertEquals(ExposureMode.Manual, nextExposureMode(ExposureMode.IsoPriority, all))
        assertEquals(ExposureMode.Auto, nextExposureMode(ExposureMode.Manual, all))
    }

    @Test
    fun skipsModesTheCameraCannotDo() {
        val autoAndManual = setOf(ExposureMode.Auto, ExposureMode.Manual)
        assertEquals(ExposureMode.Manual, nextExposureMode(ExposureMode.Auto, autoAndManual))
        assertEquals(ExposureMode.Auto, nextExposureMode(ExposureMode.Manual, autoAndManual))
    }

    @Test
    fun autoOnlyCameraStaysInAuto() {
        assertEquals(ExposureMode.Auto, nextExposureMode(ExposureMode.Auto, setOf(ExposureMode.Auto)))
        assertEquals(ExposureMode.Auto, nextExposureMode(ExposureMode.Auto, emptySet()))
    }

    @Test
    fun anUnsupportedCurrentModeStillMovesOn() {
        assertEquals(ExposureMode.Auto, nextExposureMode(ExposureMode.IsoPriority, setOf(ExposureMode.Auto)))
    }

    @Test
    fun chipLabelsAreDistinct() {
        assertEquals(ExposureMode.entries.size, ExposureMode.entries.map { it.chipLabel }.toSet().size)
    }
}
