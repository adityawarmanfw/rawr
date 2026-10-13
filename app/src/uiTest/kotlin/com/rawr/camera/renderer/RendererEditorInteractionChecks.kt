package com.rawr.camera.renderer

import android.graphics.Bitmap
import androidx.compose.runtime.mutableStateOf
import androidx.compose.foundation.layout.Column
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.activity.ComponentActivity
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.settings.model.SettingsCatalog
import com.rawr.camera.ui.CaptureTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** Same pointer tests run on the host and Android. No RAW files, export, or global settings writer. */
abstract class RendererEditorInteractionChecks {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    protected open val landscape = false
    private var dispatched = 0
    private var lastViewport = RendererViewport()
    private val preview = mutableStateOf(RendererPreviewState())

    private fun open() {
        if (landscape) {
            compose.activity.requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
            compose.waitForIdle()
        }
        val job = RenderJob("ui-test", "UI test", width = 16000, height = 12000)
        preview.value = RendererPreviewState(frame = RendererPreviewFrame(job.previewKey() to 0L,
            Bitmap.createBitmap(16, 12, Bitmap.Config.ARGB_8888), null), activity = "BINNING 200 → 50 MP")
        compose.setContent {
            CaptureTheme {
                RendererEditorContent(job, SettingsCatalog.initialState().values,
                    SettingsDispatch { dispatched++ }, preview.value, false, {}, {},
                    onViewport = { lastViewport = it }, onWaveform = {}, onSaveResolution = {}, onImportLut = {}, metadata = {})
            }
        }
        compose.waitForIdle()
    }
    private fun tab(stage: String) {
        compose.onNodeWithTag("renderer_stage_$stage").performTouchInput { click() }
        compose.onNodeWithTag("renderer_stage_$stage").assertIsSelected()
        compose.onNodeWithTag("renderer_controls_$stage").assertIsDisplayed()
    }

    @Test fun tabsReplaceControlsDuringPreviewWorkWithoutResizeOrBackgrounding() {
        open()
        repeat(3) {
            tab("RAW")
            compose.onNodeWithText("Demosaic").assertIsDisplayed()
            tab("OUTPUT")
            compose.onNodeWithTag("renderer_controls_RAW").assertDoesNotExist()
            tab("LINEAR")
            tab("TONEMAP")
        }
        assertEquals(0, dispatched)
    }

    @Test fun switchingTabsReplacesPageNodesAndRestoresScroll() {
        open(); tab("LINEAR")
        val linear = compose.onNodeWithTag("renderer_controls_LINEAR")
        linear.performTouchInput { swipe(Offset(width - 20f, height - 20f), Offset(width - 20f, 20f), 500) }
        val previousNode = linear.fetchSemanticsNode()
        val previousScroll = previousNode.config[SemanticsProperties.VerticalScrollAxisRange].value()
        assertTrue(previousScroll > 0f)
        tab("RAW")
        val rawNode = compose.onNodeWithTag("renderer_controls_RAW").fetchSemanticsNode()
        assertTrue("Different stages must not reuse the same page LayoutNode",
            previousNode.layoutInfo !== rawNode.layoutInfo)
        tab("LINEAR")
        val restoredNode = linear.fetchSemanticsNode()
        assertTrue("Returning to a stage must create a fresh page LayoutNode",
            previousNode.layoutInfo !== restoredNode.layoutInfo)
        assertEquals(previousScroll, restoredNode.config[SemanticsProperties.VerticalScrollAxisRange].value(), 1f)
    }

    @Test fun rightGutterScrollsAndTabsStillWorkAfterScrolling() {
        open(); tab("LINEAR")
        val controls = compose.onNodeWithTag("renderer_controls_LINEAR")
        controls.performTouchInput { swipe(Offset(width - 20f, height - 20f), Offset(width - 20f, 20f), 500) }
        val scroll = controls.fetchSemanticsNode().config[SemanticsProperties.VerticalScrollAxisRange]
        assertTrue("The right gutter must scroll the controls", scroll.value() > 0f)
        assertEquals("Scrolling the gutter must not edit a setting", 0, dispatched)
        tab("RAW"); tab("OUTPUT"); tab("LINEAR")
    }

    @Test fun previewGesturesDoNotEditInvisibleControlsAndToolbarRemainsUsable() {
        open(); tab("LINEAR")
        compose.onNodeWithTag("renderer_controls_LINEAR").performTouchInput {
            swipe(Offset(width - 20f, height - 20f), Offset(width - 20f, 20f), 500)
        }
        compose.onNodeWithTag("renderer_preview").performTouchInput { doubleClick() }
        compose.waitForIdle()
        assertTrue("Double tap must zoom the preview", lastViewport.zoom > 1f)
        compose.onNodeWithTag("renderer_preview").performTouchInput {
            swipe(Offset(width * .25f, height * .5f), Offset(width * .7f, height * .5f), 400)
        }
        compose.waitForIdle()
        assertTrue("Preview pan must update the viewport", lastViewport.offsetX != 0f)
        assertEquals("Preview gestures must not reach scrolled sliders", 0, dispatched)
        compose.onNodeWithTag("renderer_fit").performTouchInput { click() }
        compose.waitForIdle()
        assertEquals(0f, lastViewport.offsetX)
        compose.onNodeWithTag("renderer_toggle_pane").performTouchInput { click() }
        compose.onNodeWithTag("renderer_stage_RAW").assertDoesNotExist()
        compose.onNodeWithTag("renderer_toggle_pane").performTouchInput { click() }
        tab("RAW")
    }

    @Test fun filmQuickSwitchesWorkCollapsedAndRestoreEditedValues() {
        val initial = FilmSimLook(dirCouplersAmount = .37f, cameraUvFilterEnabled = true,
            cameraIrFilterEnabled = false, cameraUvCutNm = 435f, printExposureEv = .7f)
        val look = mutableStateOf(initial)
        compose.setContent {
            CaptureTheme {
                Column {
                    listOf(FilmSimSection.DirCouplers, FilmSimSection.Print, FilmSimSection.Filters).forEach { section ->
                        RendererFilmGroup(section, look.value, SettingsDispatch { action ->
                            look.value = when (action) {
                                is SetFilmSimNumericValue -> look.value.withNumeric(action)
                                is SetFilmSimDiscreteValue -> look.value.withDiscrete(action)
                                is SetFilmSimFlag -> look.value.withFlag(action)
                                else -> look.value
                            }
                        }) { }
                    }
                }
            }
        }
        fun toggle(title: String) = compose.onNodeWithTag("renderer_film_toggle_$title").performTouchInput { click() }
        toggle("DIR Couplers")
        compose.runOnIdle { assertEquals(0f, look.value.dirCouplersAmount) }
        toggle("DIR Couplers")
        compose.runOnIdle { assertEquals(.37f, look.value.dirCouplersAmount) }
        toggle("Filters")
        compose.runOnIdle { assertTrue(!look.value.cameraUvFilterEnabled && !look.value.cameraIrFilterEnabled) }
        toggle("Filters")
        compose.runOnIdle {
            assertTrue(look.value.cameraUvFilterEnabled && !look.value.cameraIrFilterEnabled)
            assertEquals(435f, look.value.cameraUvCutNm)
        }
        toggle("Print")
        compose.runOnIdle { assertEquals(1, look.value.process) }
        toggle("Print")
        compose.runOnIdle { assertEquals(initial, look.value) }
    }

    @Test fun inspectionToolsShareToolbarWithoutAnExtraRow() {
        open()
        val fit = compose.onNodeWithTag("renderer_fit").fetchSemanticsNode().boundsInRoot
        val wave = compose.onNodeWithTag("renderer_tool_waveform")
        val metadata = compose.onNodeWithTag("renderer_tool_metadata")
        val waveBounds = wave.fetchSemanticsNode().boundsInRoot
        val metadataBounds = metadata.fetchSemanticsNode().boundsInRoot
        assertEquals(fit.center.y, waveBounds.center.y, 1f)
        assertEquals(fit.center.y, metadataBounds.center.y, 1f)
        assertTrue(waveBounds.right <= metadataBounds.left)
        wave.performTouchInput { click() }
        wave.assertIsSelected()
        tab("RAW"); tab("OUTPUT")
        wave.performTouchInput { click() }
        wave.assertIsNotSelected()
    }

    @Test fun framePublicationAndExpansionDoNotResetSelectedTab() {
        open(); tab("OUTPUT")
        compose.runOnIdle {
            preview.value = preview.value.copy(activity = "APPLYING LOOK", frame = preview.value.frame?.copy(
                bitmap = Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888)))
        }
        compose.onNodeWithTag("renderer_stage_OUTPUT").assertIsSelected()
        compose.onNodeWithTag("renderer_expand_pane").performTouchInput { click() }
        compose.onNodeWithTag("renderer_controls_OUTPUT").assertIsDisplayed()
        tab("RAW"); tab("TONEMAP")
    }
}
