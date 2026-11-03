package com.rawr.camera.ui

import com.rawr.camera.registerComposeTestActivity

import androidx.compose.foundation.layout.width
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.unit.dp
import com.rawr.camera.fixtures.CaptureFixtures
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.DcpQuickState
import com.rawr.camera.settings.model.LensDcp
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], qualifiers = "w360dp-h800dp")
class CaptureDcpSelectorTest {
    init { registerComposeTestActivity() }

    @get:Rule val compose = createComposeRule()
    private val first = LensDcp("0".repeat(32) + ".dcp", "A long daylight camera profile name")
    private val second = LensDcp("1".repeat(32) + ".dcp", "Tungsten")

    @Test fun pickerSelectsProfilesAndDefaultAndClosesWhenLocked() {
        val dcp = mutableStateOf(DcpQuickState("wide", listOf(first, second), first.file))
        val enabled = mutableStateOf(true)
        compose.setContent {
            CaptureTheme {
                LensAndDcpSelector(CaptureUiState(capabilities = CaptureFixtures.baseline()),
                    {}, Modifier.width(336.dp), dcp.value,
                    { dcp.value = dcp.value.copy(selectedFile = it) }, enabled.value)
            }
        }
        compose.onNodeWithTag(CaptureTestTags.DCP_PROFILE).assertIsDisplayed().performClick()
        compose.onNodeWithTag(CaptureTestTags.dcpOption(second.file)).performClick()
        compose.runOnIdle { assertEquals(second.file, dcp.value.selectedFile) }
        compose.onNodeWithTag(CaptureTestTags.DCP_PROFILE).performClick()
        compose.onNodeWithTag(CaptureTestTags.dcpOption(null)).performClick()
        compose.runOnIdle { assertEquals(null, dcp.value.selectedFile) }
        compose.onNodeWithTag(CaptureTestTags.DCP_PROFILE).performClick()
        compose.runOnIdle { enabled.value = false }
        compose.onNodeWithTag(CaptureTestTags.DCP_PROFILE).assertIsNotEnabled()
        compose.onNodeWithTag(CaptureTestTags.dcpOption(first.file)).assertDoesNotExist()
    }
    @Test fun switchingBetweenLensesWithAndWithoutDcpsKeepsRowHeightStable() {
        val dcp = mutableStateOf<DcpQuickState?>(null)
        compose.setContent {
            CaptureTheme {
                LensAndDcpSelector(CaptureUiState(capabilities = CaptureFixtures.baseline()),
                    {}, Modifier.width(336.dp).testTag("lens_dcp_row"), dcp.value, {})
            }
        }
        val lensBefore = compose.onNodeWithTag(CaptureTestTags.lens("wide")).fetchSemanticsNode().boundsInRoot
        val rowBefore = compose.onNodeWithTag("lens_dcp_row").fetchSemanticsNode().boundsInRoot
        compose.onNodeWithTag(CaptureTestTags.DCP_PROFILE).assertDoesNotExist()
        compose.runOnIdle { dcp.value = DcpQuickState("wide", listOf(first, second), first.file) }
        compose.onNodeWithTag(CaptureTestTags.DCP_PROFILE).assertIsEnabled()
        assertEquals(lensBefore.top, compose.onNodeWithTag(CaptureTestTags.lens("wide")).fetchSemanticsNode().boundsInRoot.top)
        assertEquals(rowBefore.height, compose.onNodeWithTag("lens_dcp_row").fetchSemanticsNode().boundsInRoot.height)
        compose.runOnIdle { dcp.value = DcpQuickState("main", emptyList(), null) }
        compose.onNodeWithTag(CaptureTestTags.DCP_PROFILE).assertDoesNotExist()
        assertEquals(lensBefore.top, compose.onNodeWithTag(CaptureTestTags.lens("wide")).fetchSemanticsNode().boundsInRoot.top)
        assertEquals(rowBefore.height, compose.onNodeWithTag("lens_dcp_row").fetchSemanticsNode().boundsInRoot.height)
    }

}
