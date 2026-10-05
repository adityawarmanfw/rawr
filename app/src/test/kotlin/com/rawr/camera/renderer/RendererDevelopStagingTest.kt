package com.rawr.camera.renderer

import com.rawr.camera.settings.model.DemosaicAlgorithm
import com.rawr.camera.settings.model.SettingsCatalog
import org.junit.Assert.*
import org.junit.Test

class RendererDevelopStagingTest {
    private fun base() = SettingsCatalog.initialState().values

    @Test fun toneOnlyChangeIsLive() {
        val before = base()
        val after = before.copy(rawrBaseTone = before.rawrBaseTone.copy(shadows = 10f))
        assertFalse(RendererDevelopStaging.isStagedChange(before, after))
    }

    @Test fun numericPostSlidersStayLive() {
        val before = base()
        assertFalse(RendererDevelopStaging.isStagedChange(before, before.copy(photoDefringeStrength = 0.5f)))
        assertFalse(RendererDevelopStaging.isStagedChange(before, before.copy(photoHighlightThreshold = 1.5f)))
        assertFalse(RendererDevelopStaging.isStagedChange(before, before.copy(photoHighlightCompression = 100f)))
        assertFalse(RendererDevelopStaging.isStagedChange(before, before.copy(filmSimEnabled = true)))
    }

    @Test fun cacheBustingSwitchesAreStaged() {
        val before = base()
        assertTrue(RendererDevelopStaging.isStagedChange(before, before.copy(demosaicAlgorithm = DemosaicAlgorithm.Vng4)))
        assertTrue(RendererDevelopStaging.isStagedChange(before, before.copy(dualAutoContrast = false)))
        assertTrue(RendererDevelopStaging.isStagedChange(before, before.copy(quadfixEnabled = true)))
        assertTrue(
            RendererDevelopStaging.isStagedChange(
                before, before.copy(photoLensShadingEnabled = !before.photoLensShadingEnabled)
            )
        )
    }

    @Test fun exactRenderSwitchesAreStaged() {
        val before = base()
        assertTrue(RendererDevelopStaging.isStagedChange(before, before.copy(photoFccSteps = 4)))
        assertTrue(RendererDevelopStaging.isStagedChange(before, before.copy(photoDefringeEnabled = !before.photoDefringeEnabled)))
        assertTrue(
            RendererDevelopStaging.isStagedChange(
                before, before.copy(distortionCorrectionEnabled = !before.distortionCorrectionEnabled)
            )
        )
        assertTrue(RendererDevelopStaging.isStagedChange(before, before.copy(photoHighlightMethod = 1 - before.photoHighlightMethod)))
    }

    @Test fun labelsNameChangedGroups() {
        val before = base()
        val after = before.copy(
            demosaicAlgorithm = DemosaicAlgorithm.DualRcdVng4,
            photoLensShadingEnabled = !before.photoLensShadingEnabled
        )
        assertEquals(listOf("Demosaic", "Shading"), RendererDevelopStaging.stagedLabels(before, after))
    }

    @Test fun toneRemainsLiveWhileDevelopIsStaged() {
        val applied = base()
        val editing = applied.copy(
            demosaicAlgorithm = DemosaicAlgorithm.Vng4,
            photoLensShadingEnabled = !applied.photoLensShadingEnabled,
            rawrBaseTone = applied.rawrBaseTone.copy(shadows = 12f)
        )
        val preview = RendererDevelopStaging.appliedPreview(applied, editing)
        assertEquals(applied.demosaicAlgorithm, preview.demosaicAlgorithm)
        assertEquals(applied.photoLensShadingEnabled, preview.photoLensShadingEnabled)
        assertEquals(12f, preview.rawrBaseTone.shadows)
    }
}
