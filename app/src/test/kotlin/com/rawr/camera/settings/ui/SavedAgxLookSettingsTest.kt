package com.rawr.camera.settings.ui

import androidx.compose.runtime.collectAsState
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import com.rawr.camera.model.RenderProfileSelection
import com.rawr.camera.registerComposeTestActivity
import com.rawr.camera.settings.architecture.PersistentSettingsController
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.settings.architecture.withAgxValue
import com.rawr.camera.settings.model.*
import com.rawr.camera.ui.CaptureTheme
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.LooperMode

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], qualifiers = "w412dp-h892dp")
@LooperMode(LooperMode.Mode.PAUSED)
class SavedAgxLookSettingsTest {
    init { registerComposeTestActivity() }
    @get:Rule val compose = createComposeRule()

    @Test fun doubleTapResetUsesSavedLookAndKeepsItsSelection() {
        val base = AgxPreset.Rawr
        val look = AgxLook("custom", "My saved look", base, base.defaults.with(AgxParameter.Contrast, 2.2f))
        val values = SettingsCatalog.initialState().values.copy(agxLooks = listOf(look))
            .withRenderProfile(RenderProfileSelection.SavedAgx(look.id))
            .withAgxValue(AgxParameter.Contrast, 4.32f)
        val controller = PersistentSettingsController(SettingsCatalog.initialState().copy(values = values))
        compose.setContent {
            CaptureTheme {
                ImageToneSettings(controller.state.collectAsState().value, SettingsDispatch(controller::dispatch))
            }
        }
        compose.onNode(hasScrollAction()).performScrollToNode(hasText("Contrast"))
        compose.onNodeWithText("Contrast").performTouchInput { doubleClick() }
        compose.runOnIdle {
            val reset = controller.state.value.values
            assertEquals(look, reset.selectedAgxLook())
            assertEquals(2.2f, reset.activeAgxValues().valueOf(AgxParameter.Contrast))
            assertFalse(reset.isAgxModified())
        }
    }
}
