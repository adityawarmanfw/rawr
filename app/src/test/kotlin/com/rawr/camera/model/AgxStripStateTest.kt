package com.rawr.camera.model

import com.rawr.camera.settings.model.*
import kotlin.test.*

class AgxStripStateTest {
    private val preset = AgxPreset.Default
    private fun state(values: AgxValues = preset.defaults, tenths: Int = 0) =
        agxStripState(AgxQuickState(values, preset.defaults), tenths, preset.label)

    @Test fun sectionsFollowDarktableAndCoverEveryControlOnce() {
        val quick = state()
        assertEquals(listOf("RANGE", "CURVE", "LOOK", "PRIM"), quick.sections.map { it.label })
        assertEquals(listOf("BEFORE", "AFTER"), quick.sections.last().subsections.map { it.label })
        val keys = quick.sections.flatMap { it.paramKeys }
        assertEquals(AGX_STRIP_EXPOSURE_KEY, keys.first())
        assertEquals(listOf(AGX_STRIP_EXPOSURE_KEY) + AgxCatalog.all.map { it.jsonKey }, keys)
        assertTrue(keys.all { it in quick.params })
        assertFalse(quick.modified)
    }

    @Test fun readoutsUseDisplayUnits() {
        val values = preset.defaults.with(AgxParameter.BlueRotation, -0.0349066f)
        val quick = state(values, tenths = 3)
        assertEquals("18.0%", quick.params.getValue("pivotOutput").displayValue)
        assertEquals("-2.0°", quick.params.getValue("blueRotation").displayValue)
        assertEquals("6.5", quick.params.getValue("whiteEv").displayValue)
        assertEquals("+0.3", quick.params.getValue(AGX_STRIP_EXPOSURE_KEY).displayValue)
        assertFalse(quick.params.getValue("blueRotation").isDefault)
        assertTrue(quick.modified)
    }
}
