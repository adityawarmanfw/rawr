package com.rawr.camera.renderer

import com.rawr.camera.settings.model.*
import kotlin.test.*

class RendererRecipeTest {
    @Test fun localTuningSurvivesCaptureEditingAndLegacyDefaults() {
        val library = SettingsCatalog.initialState().values
        val tuned = library.copy(localToneTuning = LocalToneTuning(1.2f, 3.7f, 8, true))
        val original = RendererRecipe.encode(tuned)
        assertEquals(tuned.localToneTuning, RendererRecipe.decode(original, library).localToneTuning)
        val edited = RendererRecipe.rebuiltDraft(tuned.copy(photoFccSteps = 4), original, original)
        assertEquals(tuned.localToneTuning, RendererRecipe.decode(edited, library).localToneTuning)
        val legacy = org.json.JSONObject(original).apply { remove("localToneTuning") }.toString()
        assertEquals(LocalToneTuning(), RendererRecipe.decode(legacy, tuned).localToneTuning)
        val resolved = org.json.JSONObject(legacy).put("localTone", org.json.JSONObject()
            .put("version", 3).put("shadowsEv", 2).put("highlightsEv", 1).put("sigma", 4)
            .put("coarsestMip", 7).put("boostLocalContrast", true)).toString()
        assertEquals(LocalToneTuning(1f, 4f, 7, true), RendererRecipe.decode(resolved, library).localToneTuning)
    }

    @Test fun localTuningBoundsRejectNonFiniteAndOutOfRangeSettings() {
        assertEquals(LocalToneTuning(2f, 5f, 6, false), LocalToneTuning(Float.NaN, Float.POSITIVE_INFINITY, -1).sanitized())
        assertEquals(LocalToneTuning(.5f, 5f, 8, true), LocalToneTuning(-4f, 100f, 99, true).sanitized())
    }

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

    @Test fun rec709PhotoRecipeRetainsItsProfileAndTone() {
        val values = SettingsCatalog.initialState().values.copy(
            colorRenderProfile = ColorRenderProfile.Rec709,
            rec709Tone = ProfileTone(contrast = 14f, shadows = 9f))
        val library = SettingsCatalog.initialState().values
        val decoded = RendererRecipe.decode(RendererRecipe.encode(values), library)
        assertEquals(ColorRenderProfile.Rec709, decoded.colorRenderProfile)
        assertEquals(values.rec709Tone, decoded.activeProfileTone())
        val captured = RendererRecipe.decode(com.rawr.camera.integration.CaptureRecipe.encode(values), library)
        assertEquals(ColorRenderProfile.Rec709, captured.colorRenderProfile)
        assertEquals(values.rec709Tone, captured.activeProfileTone())
    }

    @Test fun rawrDngImportsReplayTheirRecipe() {
        assertTrue(RendererRecipe.replaysRecipe(capture = "RAWR_20261014_153111_095", embeddedRecipe = null))
        assertTrue(RendererRecipe.replaysRecipe(capture = "", embeddedRecipe = "{\"version\":1}"))
        assertFalse(RendererRecipe.replaysRecipe(capture = "", embeddedRecipe = null))
        assertFalse(RendererRecipe.replaysRecipe(capture = "", embeddedRecipe = ""))
    }

    @Test fun shadowLiftModeOverridesTheCaptureAndSurvivesSettingsEdits() {
        val values = SettingsCatalog.initialState().values
        val original = org.json.JSONObject(RendererRecipe.encode(values))
            .put("capturedAutoKeyExposure", "Natural")
            .put("adaptiveTone", org.json.JSONObject().put("liftEv", 0.55).put("keyEv", -3.69).put("highlightEv", -1.47))
            .toString()
        val captured = RendererRecipe.rebuiltDraft(values, previousDraft = "{}", original = original)
        assertEquals(AutoKeyExposure.Natural, RendererRecipe.adaptiveToneMode(captured))
        assertFalse(org.json.JSONObject(captured).has("adaptiveToneMode"))
        val strong = RendererRecipe.withAdaptiveToneMode(captured, AutoKeyExposure.Strong)
        assertEquals(AutoKeyExposure.Strong, RendererRecipe.adaptiveToneMode(strong))
        // Another settings edit keeps the override; the captured tone stays the source's.
        val edited = RendererRecipe.rebuiltDraft(values.copy(photoFccSteps = 4), strong, original)
        assertEquals(AutoKeyExposure.Strong, RendererRecipe.adaptiveToneMode(edited))
        assertEquals(0.55f, RendererRecipe.adaptiveLift(edited))
        // Choosing the captured mode again clears the override: exact replay.
        val back = RendererRecipe.withAdaptiveToneMode(edited, AutoKeyExposure.Natural)
        assertFalse(org.json.JSONObject(back).has("adaptiveToneMode"))
        assertEquals(AutoKeyExposure.Natural, RendererRecipe.adaptiveToneMode(back))
    }

    @Test fun shadowLiftModeForSourcesWithoutARecordedMode() {
        val values = SettingsCatalog.initialState().values
        // Other cameras and Off captures: no lift, shown as Off; any lift mode is an override.
        val plain = RendererRecipe.encode(values)
        assertEquals(AutoKeyExposure.Off, RendererRecipe.adaptiveToneMode(plain))
        val medium = RendererRecipe.withAdaptiveToneMode(plain, AutoKeyExposure.Medium)
        assertEquals("Medium", org.json.JSONObject(medium).getString("adaptiveToneMode"))
        assertFalse(org.json.JSONObject(RendererRecipe.withAdaptiveToneMode(medium, AutoKeyExposure.Off)).has("adaptiveToneMode"))
        // Older Rawr captures with a lift but no recorded mode read "as captured" until changed.
        val legacy = org.json.JSONObject(plain).put("adaptiveTone",
            org.json.JSONObject().put("liftEv", 0.4).put("keyEv", -3.0).put("highlightEv", -1.0)).toString()
        assertNull(RendererRecipe.adaptiveToneMode(legacy))
        assertEquals("Off", org.json.JSONObject(RendererRecipe.withAdaptiveToneMode(legacy, AutoKeyExposure.Off))
            .getString("adaptiveToneMode"))
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
