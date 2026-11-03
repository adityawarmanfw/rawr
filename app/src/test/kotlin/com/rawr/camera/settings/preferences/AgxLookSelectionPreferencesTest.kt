package com.rawr.camera.settings.preferences

import android.content.Context
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.test.core.app.ApplicationProvider
import com.rawr.camera.model.RenderProfileSelection
import com.rawr.camera.settings.architecture.withAgxDefault
import com.rawr.camera.settings.architecture.withAgxValue
import com.rawr.camera.settings.model.*
import kotlin.test.*
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class AgxLookSelectionPreferencesTest {
    private val defaults = SettingsCatalog.initialState().values
    private val repository = SettingsPreferencesRepository(ApplicationProvider.getApplicationContext<Context>(), defaults)
    private val photo = AgxLook("photo", "Photo", AgxPreset.Rawr,
        AgxPreset.Rawr.defaults.with(AgxParameter.Contrast, 2.2f))
    private val video = AgxLook("video", "Video", AgxPreset.Smooth,
        AgxPreset.Smooth.defaults.with(AgxParameter.Contrast, 2.4f))

    @Test fun editedSelectionsSurviveStorageWithIndependentPhotoAndVideoOrigins() {
        val selected = defaults.copy(agxLooks = listOf(photo, video))
            .withRenderProfile(RenderProfileSelection.SavedAgx(photo.id))
            .withAgxValue(AgxParameter.Contrast, 2.5f)
            .copy(captureModeId = "video")
            .withRenderProfile(RenderProfileSelection.SavedAgx(video.id))
            .withAgxValue(AgxParameter.Contrast, 2.7f)
        val restored = repository.decodeBackup(repository.encodeForBackup(selected))!!
        for ((mode, look) in listOf("photo" to photo, "video" to video)) {
            val active = restored.copy(captureModeId = mode)
            assertEquals(look, active.selectedAgxLook())
            assertTrue(active.isAgxModified())
            assertEquals(look.values, active.activeAgxDefaults())
            assertFalse(active.withAgxDefault(AgxParameter.Contrast).isAgxModified())
        }
        val builtIn = restored.withRenderProfile(RenderProfileSelection.Agx(video.base))
        val reloaded = repository.decodeBackup(repository.encodeForBackup(builtIn))!!
        assertNull(reloaded.selectedAgxLook())
        assertEquals(video.base.defaults, reloaded.activeAgxDefaults())
        assertEquals(photo.id, reloaded.selectedAgxLookId)
    }

    @Test fun legacyMatchingSelectionIsInferredOnceAndExplicitBuiltInStaysBuiltIn() {
        val selected = defaults.copy(agxLooks = listOf(photo))
            .withRenderProfile(RenderProfileSelection.SavedAgx(photo.id))
        val legacy = repository.encodeForBackup(selected).toMutablePreferences().apply {
            remove(stringPreferencesKey("selected_agx_look_id"))
            remove(stringPreferencesKey("video_agx_look_id"))
        }
        val migrated = repository.decodeBackup(legacy)!!
        assertEquals(photo, migrated.selectedAgxLook())
        val builtIn = migrated.withRenderProfile(RenderProfileSelection.Agx(photo.base))
        assertNull(repository.decodeBackup(repository.encodeForBackup(builtIn))!!.selectedAgxLook())
    }
}
