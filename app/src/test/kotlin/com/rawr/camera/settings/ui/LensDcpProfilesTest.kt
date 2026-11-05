package com.rawr.camera.settings.ui

import com.rawr.camera.registerComposeTestActivity

import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import com.rawr.camera.settings.model.LensDcp
import com.rawr.camera.settings.model.LensImage
import com.rawr.camera.ui.CaptureTheme
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], qualifiers = "w360dp-h800dp")
class LensDcpProfilesTest {
    init { registerComposeTestActivity() }

    @get:Rule val compose = createComposeRule()

    @Test fun summaryOpensPickerWithSelectionRemovalAndImport() {
        val first = LensDcp("0".repeat(32) + ".dcp", "Daylight")
        val second = LensDcp("1".repeat(32) + ".dcp", "A long tungsten camera profile name")
        val image = mutableStateOf(LensImage().importDcp(first).importDcp(second))
        var imports = 0
        compose.setContent {
            CaptureTheme {
                LensDcpProfiles(image.value, false, null,
                    { image.value = image.value.selectDcp(it) },
                    { image.value = image.value.removeDcp(it) }, { imports++ })
            }
        }
        compose.onNodeWithTag(lensEditorTag("dcp_${first.file}")).assertDoesNotExist()
        compose.onNodeWithTag(lensEditorTag("dcp")).performClick()
        compose.onNodeWithTag(lensEditorTag("dcp_${second.file}")).assertIsSelected()
        compose.onNodeWithTag(lensEditorTag("dcp_${first.file}")).performClick().assertIsSelected()
        compose.onNodeWithTag(lensEditorTag("removeDcp_${second.file}")).performClick()
        compose.onNodeWithTag(lensEditorTag("dcp_${second.file}")).assertDoesNotExist()
        compose.onNodeWithTag(lensEditorTag("dcp_${first.file}")).assertIsSelected()
        compose.onNodeWithTag(lensEditorTag("removeDcp_${first.file}")).performClick()
        compose.onNodeWithTag(lensEditorTag("dcpDefault")).assertIsSelected()
        compose.onNodeWithTag(lensEditorTag("dcpDone")).performClick()
        compose.onNodeWithTag(lensEditorTag("dcpPicker")).assertDoesNotExist()
        compose.onNodeWithTag(lensEditorTag("dcp")).performClick()
        compose.onNodeWithTag(lensEditorTag("dcpImport")).performClick()
        compose.runOnIdle { assertEquals(1, imports) }
        compose.onNodeWithTag(lensEditorTag("dcpPicker")).assertDoesNotExist()
    }
    @OptIn(ExperimentalTestApi::class)
    @Test fun sortingAndDraggingUpdateThePickerOrderWithoutChangingSelection() {
        val zulu = LensDcp("2".repeat(32) + ".dcp", "Zulu")
        val alpha = LensDcp("3".repeat(32) + ".dcp", "alpha")
        val image = mutableStateOf(LensImage().importDcp(zulu).importDcp(alpha))
        compose.setContent {
            CaptureTheme {
                LensDcpProfiles(image.value, false, null,
                    onSelect = { image.value = image.value.selectDcp(it) },
                    onRemove = { image.value = image.value.removeDcp(it) }, onImport = {},
                    onMove = { from, to -> image.value = image.value.moveDcp(from, to) },
                    onSort = { image.value = image.value.sortDcps(it) })
            }
        }
        compose.onNodeWithTag(lensEditorTag("dcp")).performClick()
        compose.onNodeWithTag(lensEditorTag("dcpSort")).performClick()
        compose.onNodeWithTag(lensEditorTag("dcpSortAscending")).performClick()
        compose.runOnIdle { assertEquals(listOf(alpha, zulu), image.value.availableDcps) }
        compose.onNodeWithTag(lensEditorTag("dcpSort")).performClick()
        compose.onNodeWithTag(lensEditorTag("dcpSortDescending")).performClick()
        compose.runOnIdle { assertEquals(listOf(zulu, alpha), image.value.availableDcps) }
        compose.onNodeWithTag(lensEditorTag("dcp_${zulu.file}"))
            .performCustomAccessibilityActionWithLabel("Move down")
        compose.runOnIdle { assertEquals(listOf(alpha, zulu), image.value.availableDcps) }
        val distance = compose.onNodeWithTag(lensEditorTag("dcp_${zulu.file}")).fetchSemanticsNode().boundsInRoot.center.y -
            compose.onNodeWithTag(lensEditorTag("dcp_${alpha.file}")).fetchSemanticsNode().boundsInRoot.center.y
        compose.onNodeWithTag(lensEditorTag("moveDcp_${alpha.file}"))
            .performTouchInput { swipe(center, center + Offset(0f, distance), durationMillis = 600) }
        compose.runOnIdle {
            assertEquals(listOf(zulu, alpha), image.value.availableDcps)
            assertEquals(alpha, image.value.dcp)
        }
        compose.onNodeWithTag(lensEditorTag("dcp_${alpha.file}")).assertIsSelected()
        val defaultTop = compose.onNodeWithTag(lensEditorTag("dcpDefault")).fetchSemanticsNode().boundsInRoot.top
        val firstTop = compose.onNodeWithTag(lensEditorTag("dcp_${zulu.file}")).fetchSemanticsNode().boundsInRoot.top
        assertTrue(defaultTop < firstTop)
    }

}
