package com.rawr.camera.model

import com.rawr.camera.architecture.CaptureReducer
import com.rawr.camera.architecture.ToggleVideoLog
import com.rawr.camera.fixtures.CaptureFixtures
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*
import com.rawr.camera.settings.preferences.migrateV33RenderProfiles
import kotlin.test.*

class RenderProfileSelectionTest {
    private val defaults = SettingsCatalog.initialState().values
    private fun builtIn(profile: ColorRenderProfile) = RenderProfileSelection.BuiltIn(profile)

    @Test fun catalogsAndSelectionsAreIndependent() {
        val photo = defaults.withRenderProfile(builtIn(ColorRenderProfile.SRgb))
        assertEquals(listOf("RAWR NTRL", "AgX", "AgX Punchy", "AgX Sigmoid", "AgX Smooth", "sRGB"), photo.toRenderProfileQuickState().options.map { it.label })
        val video = photo.copy(captureModeId = "video").withRenderProfile(builtIn(ColorRenderProfile.Rec709))
        assertEquals(listOf("RAWR NTRL", "AgX", "AgX Punchy", "AgX Sigmoid", "AgX Smooth", "Rec.709"), video.toRenderProfileQuickState().options.map { it.label })
        assertEquals(ColorRenderProfile.SRgb, video.copy(captureModeId = "photo").regularRenderProfile())
        assertEquals(video, video.withRenderProfile(builtIn(ColorRenderProfile.SRgb)))
        assertEquals(photo, photo.withRenderProfile(builtIn(ColorRenderProfile.Rec709)))
    }

    @Test fun directToneAndLogBypassPreserveStoredAdjustments() {
        val photo = defaults.withRenderProfile(builtIn(ColorRenderProfile.SRgb))
            .editActiveProfileTone { it.copy(contrast = 20f) }
        val video = photo.copy(captureModeId = "video").withRenderProfile(builtIn(ColorRenderProfile.Rec709))
            .editActiveProfileTone { it.copy(contrast = -30f) }
        assertEquals(20f, video.srgbTone.contrast)
        assertEquals(-30f, video.activeProfileTone().contrast)
        assertEquals(ProfileTone.Neutral, video.rawrBaseTone)
        val log = video.copy(videoLogEnabled = true)
        assertEquals(ProfileTone.Neutral, log.activeProfileTone())
        assertEquals(log, log.editActiveProfileTone { it.copy(contrast = 99f) })
        assertEquals(-30f, log.copy(videoLogEnabled = false).activeProfileTone().contrast)
    }

    @Test fun agxPresetsAreSeparateSelectionsWithOwnControls() {
        val photo = defaults.withRenderProfile(RenderProfileSelection.Agx(AgxPreset.Smooth))
        assertEquals(ColorRenderProfile.AgX, photo.colorRenderProfile)
        assertEquals(RenderProfileSelection.Agx(AgxPreset.Smooth), photo.toRenderProfileQuickState().selectedId)
        assertEquals(AgxPreset.Smooth.defaults, photo.toRenderProfileQuickState().agx?.values)
        assertEquals("AgX Smooth", photo.activeProfileLabel())
        // Stills write sRGB (native 9); video writes Rec.709 (native 10).
        assertEquals(9, photo.effectiveRenderProfileId())
        val video = photo.copy(captureModeId = "video").withRenderProfile(RenderProfileSelection.Agx(AgxPreset.BlenderPunchy))
        assertEquals(10, video.effectiveRenderProfileId())
        assertEquals(AgxPreset.Smooth, video.agxPreset)
        // Controls are per preset, clamped, and reset to the preset default.
        val edited = photo.withAgxValue(AgxParameter.WhiteEv, 99f).withAgxValue(AgxParameter.Purity, 0.5f)
        assertEquals(AgxCatalog.whiteEv.maximum, edited.activeAgxValues().whiteEv)
        assertEquals(AgxPreset.Default.defaults, edited.agxValuesFor(AgxPreset.Default))
        assertTrue(edited.toRenderProfileQuickState().agx!!.modified)
        assertEquals(0f, edited.withAgxDefault(AgxParameter.Purity).activeAgxValues().purity)
        // AgX keeps its own TONE set; edits are ignored on other profiles.
        val toned = edited.editActiveProfileTone { it.copy(contrast = 12f) }
        assertEquals(12f, toned.agxTone.contrast)
        assertEquals(ProfileTone.Neutral, toned.rawrBaseTone)
        val ntrl = toned.withRenderProfile(builtIn(ColorRenderProfile.RawrBase))
        assertNull(ntrl.toRenderProfileQuickState().agx)
        assertEquals(ntrl, ntrl.withAgxValue(AgxParameter.ToePower, 2f))
    }

    @Test fun logCatalogAndRememberedSelection() {
        val log = defaults.copy(captureModeId = "video", videoLogEnabled = true)
        val quick = log.toRenderProfileQuickState()
        assertTrue(quick.log)
        assertEquals(5, quick.options.size)
        assertEquals(RenderProfileSelection.Log(VideoLogProfile.LogC3), quick.selectedId)
        val selected = log.withRenderProfile(RenderProfileSelection.Log(VideoLogProfile.SLog3))
        assertEquals(VideoLogProfile.SLog3, selected.copy(videoLogEnabled = false).videoLogProfile)
        assertFalse(selected.copy(captureModeId = "photo").toRenderProfileQuickState().log)
        assertEquals(selected, selected.withRenderProfile(builtIn(ColorRenderProfile.RawrBase)))
    }

    @Test fun logForcesDepthWithoutOverwritingPreference() {
        val video = defaults.copy(videoEncoder = VideoEncoderConfig(bitDepth = 8))
        assertEquals(8, video.effectiveVideoBitDepth())
        assertEquals(10, video.copy(videoLogEnabled = true).effectiveVideoBitDepth())
        assertEquals(8, video.copy(videoLogEnabled = true).videoEncoder.bitDepth)
    }

    @Test fun migrationSeedsBothSelectionsAndKeepsExistingTone() {
        val original = defaults.copy(colorRenderProfile = ColorRenderProfile.UserLut,
            selectedUserLutProfileId = "saved", rawrBaseTone = ProfileTone(contrast = 17f))
        val migrated = migrateV33RenderProfiles(original)
        assertEquals(original.colorRenderProfile, migrated.videoColorRenderProfile)
        assertEquals("saved", migrated.videoUserLutProfileId)
        assertEquals(original.rawrBaseTone, migrated.rawrBaseTone)
        assertFalse(migrated.videoLogEnabled)
        assertEquals(VideoLogProfile.LogC3, migrated.videoLogProfile)
    }

    @Test fun toggleIsVideoOnlyAndPersistsSeparately() {
        val photo = CaptureUiState(CaptureFixtures.baseline())
        assertEquals(photo, CaptureReducer.reduce(photo, ToggleVideoLog))
        val video = photo.copy(captureMode = CaptureMode.Video)
        val enabled = CaptureReducer.reduce(video, ToggleVideoLog)
        assertTrue(enabled.videoLogEnabled)
        val saved = defaults.withCapturePreferenceChanges(video, enabled)
        assertTrue(saved.capturePreferences().videoLogEnabled)
    }

    @Test fun settingsSelectOnlyTheCurrentModesProfile() {
        val state = SettingsCatalog.initialState()
        val video = state.copy(values = defaults.copy(captureModeId = "video"))
        val selected = reduceRenderProfileSettings(video, SetColorRenderProfile(ColorRenderProfile.Rec709)).values
        assertEquals(ColorRenderProfile.Rec709, selected.videoColorRenderProfile)
        assertEquals(ColorRenderProfile.RawrBase, selected.colorRenderProfile)
        assertEquals("video", selected.captureModeId)
    }

    @Test fun profileToneEditsKeepResourceSyncKeyStable() {
        val srgb = defaults.withRenderProfile(builtIn(ColorRenderProfile.SRgb))
        assertEquals(srgb.renderProfileSyncKey(), srgb.editActiveProfileTone { it.copy(contrast = 45f) }.renderProfileSyncKey())
        assertNotEquals(srgb.renderProfileSyncKey(), srgb.copy(captureModeId = "video").renderProfileSyncKey())
    }
}
