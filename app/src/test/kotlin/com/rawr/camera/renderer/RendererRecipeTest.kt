package com.rawr.camera.renderer

import com.rawr.camera.settings.model.*
import kotlin.test.*

class RendererRecipeTest {
    @Test fun legacyRecipeRetainsUserDefaultsForInactiveAgxPresets() {
        val library = SettingsCatalog.initialState().values.copy(
            agxValues = mapOf(AgxPreset.Smooth to AgxPreset.Smooth.defaults.with(AgxParameter.WhiteEv, 8f)))
        val legacy = org.json.JSONObject(RendererRecipe.encode(SettingsCatalog.initialState().values))
            .apply { remove("editorAgx") }.toString()
        val restored = RendererRecipe.decode(legacy, library)
        assertEquals(library.agxValues[AgxPreset.Smooth], restored.agxValues[AgxPreset.Smooth])
    }

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
    @Test fun inactiveToneModesSurviveReopeningWithDifferentGlobalSettings() {
        val values = SettingsCatalog.initialState().values.copy(
            rawrBaseTone=ProfileTone(contrast=27f),srgbTone=ProfileTone(shadows=18f),
            agxTone=ProfileTone(renderExposure=.7f),
            agxValues=mapOf(AgxPreset.Smooth to AgxPreset.Smooth.defaults.with(AgxParameter.WhiteEv,8f)),
            filmSimEnabled=true)
        val restored=RendererRecipe.decode(RendererRecipe.encode(values),SettingsCatalog.initialState().values)
        assertEquals(values.rawrBaseTone,restored.rawrBaseTone)
        assertEquals(values.srgbTone,restored.srgbTone)
        assertEquals(values.agxTone,restored.agxTone)
        assertEquals(values.agxValues[AgxPreset.Smooth],restored.agxValues[AgxPreset.Smooth])
    }

    @Test fun importsUseCurrentSettingsButResumedCapturesUseTheirRecipe() {
        val defaults=SettingsCatalog.initialState().values
        val capture=defaults.copy(rawrBaseTone=ProfileTone(contrast=12f))
        val current=defaults.copy(rawrBaseTone=ProfileTone(contrast=35f),filmSimEnabled=true)
        val source=RendererRecipe.encode(capture)
        val imported=RendererRecipe.decode(RendererRecipe.startingDraft(source,current,false),defaults)
        val resumed=RendererRecipe.decode(RendererRecipe.startingDraft(source,current,true),defaults)
        assertEquals(35f,imported.rawrBaseTone.contrast)
        assertTrue(imported.filmSimEnabled)
        assertEquals(12f,resumed.rawrBaseTone.contrast)
        assertFalse(resumed.filmSimEnabled)
        val reopened=RendererRecipe.decode(RendererRecipe.encode(imported),capture)
        assertEquals(imported.rawrBaseTone,reopened.rawrBaseTone)
        assertTrue(reopened.filmSimEnabled)
    }

}
