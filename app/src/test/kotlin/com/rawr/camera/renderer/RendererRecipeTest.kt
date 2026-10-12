package com.rawr.camera.renderer

import com.rawr.camera.settings.model.*
import kotlin.test.*

class RendererRecipeTest {
    @Test fun srgbPhotoRecipeRetainsItsProfileAndTone() {
        val values = SettingsCatalog.initialState().values.copy(
            colorRenderProfile = ColorRenderProfile.SRgb,
            srgbTone = ProfileTone(contrast = 21f, renderExposure = -.5f))
        val decoded = RendererRecipe.decode(RendererRecipe.encode(values), values)
        assertEquals(ColorRenderProfile.SRgb, decoded.colorRenderProfile)
        assertEquals(values.srgbTone, decoded.activeProfileTone())
        assertEquals("photo", decoded.captureModeId)
    }

    @Test fun agxRecipeRetainsPresetControlsAndTone() {
        val values = SettingsCatalog.initialState().values.copy(
            colorRenderProfile = ColorRenderProfile.AgX,
            agxPreset = AgxPreset.Sigmoid,
            agxValues = mapOf(AgxPreset.Sigmoid to AgxPreset.Sigmoid.defaults
                .with(AgxParameter.WhiteEv, 8.5f).with(AgxParameter.MasterOutset, 0.4f)
                .with(AgxParameter.BlueRotation, -0.2f).with(AgxParameter.TargetBlack, 0.01f)),
            agxTone = ProfileTone(renderExposure = 0.7f))
        val library = SettingsCatalog.initialState().values
        val decoded = RendererRecipe.decode(RendererRecipe.encode(values), library)
        assertEquals(ColorRenderProfile.AgX, decoded.colorRenderProfile)
        assertEquals(AgxPreset.Sigmoid, decoded.agxPreset)
        assertEquals(values.activeAgxValues(), decoded.activeAgxValues())
        assertEquals(values.agxTone, decoded.activeProfileTone())
    }
}
