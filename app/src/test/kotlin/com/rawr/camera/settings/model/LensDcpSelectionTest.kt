package com.rawr.camera.settings.model

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

class LensDcpSelectionTest {
    private val daylight = LensDcp("0".repeat(32) + ".dcp", "Daylight")
    private val tungsten = LensDcp("1".repeat(32) + ".dcp", "Tungsten")

    @Test fun legacySelectionSurvivesImportAndDefaultSwitch() {
        val image = LensImage(dcp = daylight).importDcp(tungsten).selectDcp(null)
        assertNull(image.dcp)
        assertEquals(listOf(daylight, tungsten), image.availableDcps)
        assertEquals(daylight, image.selectDcp(daylight.file).dcp)
        assertEquals(image, image.selectDcp("missing"))
    }

    @Test fun reimportDoesNotDuplicateAndRemovalKeepsOtherChoices() {
        val image = LensImage().importDcp(daylight).importDcp(tungsten).importDcp(daylight)
        assertEquals(listOf(daylight, tungsten), image.availableDcps)
        assertEquals(daylight, image.removeDcp(tungsten.file).dcp)
        val removed = image.removeDcp(daylight.file)
        assertNull(removed.dcp)
        assertEquals(listOf(tungsten), removed.availableDcps)
    }
    @Test fun reorderingAndSortingKeepTheActiveSelectionAndPersist() {
        val image = LensImage().importDcp(tungsten).importDcp(daylight)
        val moved = image.moveDcp(0, 1)
        assertEquals(listOf(daylight, tungsten), moved.availableDcps)
        assertEquals(daylight, moved.dcp)
        assertEquals(image, image.moveDcp(-1, 0))
        assertEquals(image, image.moveDcp(0, 10))
        assertEquals(listOf(daylight, tungsten), image.sortDcps().availableDcps)
        assertEquals(listOf(tungsten, daylight), image.sortDcps(descending = true).availableDcps)
        assertEquals(daylight, image.sortDcps(descending = true).dcp)
        val lens = LensProfile("35", "3").image { moved }
        val codec = com.rawr.camera.settings.preferences.LensProfileCodec
        assertEquals(listOf(lens), codec.decode(codec.encode(listOf(lens))))
    }

}
