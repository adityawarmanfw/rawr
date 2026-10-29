package com.rawr.camera.settings.ui

import android.Manifest
import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivityResultRegistryOwner
import androidx.activity.result.ActivityResultRegistry
import androidx.activity.result.ActivityResultRegistryOwner
import androidx.activity.result.contract.ActivityResultContract
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.core.app.ActivityOptionsCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.rawr.camera.settings.architecture.SetLocationTagging
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.settings.model.SettingsCatalog
import com.rawr.camera.ui.CaptureTheme
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], qualifiers = "w412dp-h892dp")
class CaptureSettingsTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun permissionResultsAndSettingsResumeReflectPermissionWithoutErasingIntent() {
        val app = RuntimeEnvironment.getApplication()
        val fine = Manifest.permission.ACCESS_FINE_LOCATION
        val coarse = Manifest.permission.ACCESS_COARSE_LOCATION
        shadowOf(app).denyPermissions(fine, coarse)
        val initial = SettingsCatalog.initialState()
        val state = mutableStateOf(initial.copy(values = initial.values.copy(locationTagging = true)))
        val owner = object : LifecycleOwner {
            override val lifecycle = LifecycleRegistry.createUnsafe(this)
        }
        owner.lifecycle.currentState = Lifecycle.State.RESUMED
        var requests = 0
        var pendingRequestCode = 0
        val registry = object : ActivityResultRegistry() {
            override fun <Input, Output> onLaunch(requestCode: Int, contract: ActivityResultContract<Input, Output>,
                                                  input: Input, options: ActivityOptionsCompat?) {
                assertEquals(listOf(coarse, fine), (input as Array<*>).toList())
                requests++
                pendingRequestCode = requestCode
            }
        }
        val registryOwner = object : ActivityResultRegistryOwner {
            override val activityResultRegistry = registry
        }
        compose.setContent {
            CompositionLocalProvider(LocalLifecycleOwner provides owner,
                LocalActivityResultRegistryOwner provides registryOwner) {
                CaptureTheme {
                    CaptureSettings(state.value, SettingsDispatch { action ->
                        if (action is SetLocationTagging) {
                            state.value = state.value.copy(values = state.value.values.copy(locationTagging = action.enabled))
                        }
                    })
                }
            }
        }
        val tag = SettingsTestTags.row("Capture", "location_tagging")
        val row = compose.onNodeWithTag(tag)
        val toggle = compose.onNode(isToggleable() and hasAnyAncestor(hasTestTag(tag)), useUnmergedTree = true)
        toggle.assertIsOff()
        row.performClick()
        compose.runOnIdle { registry.dispatchResult(pendingRequestCode, mapOf(coarse to false, fine to false)) }
        toggle.assertIsOff()
        assertTrue("Denial preserves the saved intent", state.value.values.locationTagging)
        row.performClick()
        compose.runOnIdle {
            shadowOf(app).grantPermissions(coarse)
            registry.dispatchResult(pendingRequestCode, mapOf(coarse to true, fine to false))
        }
        toggle.assertIsOn()
        assertEquals(2, requests)
        for (access in listOf("none", "fine", "coarse")) {
            compose.runOnIdle {
                owner.lifecycle.currentState = Lifecycle.State.CREATED
                shadowOf(app).denyPermissions(fine, coarse)
                if (access == "fine") shadowOf(app).grantPermissions(fine, coarse)
                if (access == "coarse") shadowOf(app).grantPermissions(coarse)
                owner.lifecycle.currentState = Lifecycle.State.RESUMED
            }
            if (access == "none") toggle.assertIsOff() else toggle.assertIsOn()
            assertTrue(state.value.values.locationTagging)
        }
        assertEquals(2, requests)
        row.performClick()
        toggle.assertIsOff()
        assertFalse(state.value.values.locationTagging)
        row.performClick()
        toggle.assertIsOn()
        assertEquals(2, requests)
    }
}