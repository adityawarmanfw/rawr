package com.rawr.camera.settings.ui

import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.border
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.rawr.camera.integration.CameraInventory
import com.rawr.camera.integration.CameraKeyCatalog
import com.rawr.camera.integration.CameraKeyInfo
import com.rawr.camera.integration.ProbedCamera
import com.rawr.camera.settings.architecture.NavigateBack
import com.rawr.camera.settings.architecture.SetLensProfiles
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.settings.model.LENS_NAME_MAX_LENGTH
import com.rawr.camera.settings.model.SENSOR_LAYOUT_BLOCKS
import com.rawr.camera.settings.model.LIVE_REMOSAIC_QUALITIES
import com.rawr.camera.settings.model.COLOUR_ORDERS
import com.rawr.camera.settings.model.PHOTO_REMOSAIC_QUALITIES
import com.rawr.camera.settings.model.CAMERA_API_CHOICES
import com.rawr.camera.settings.model.CAMERA_WORKAROUNDS
import com.rawr.camera.settings.model.COMPANION_FALLBACKS
import com.rawr.camera.settings.model.AuxJpegSettings
import com.rawr.camera.settings.model.PhotoStreamSettings
import com.rawr.camera.settings.model.CompanionMode
import com.rawr.camera.settings.model.LensCamera
import com.rawr.camera.settings.model.LensSensorLayout
import com.rawr.camera.settings.model.LensOutputSize
import com.rawr.camera.settings.model.LensSession
import com.rawr.camera.settings.model.StillSequence
import com.rawr.camera.settings.model.stillSequence
import com.rawr.camera.settings.model.isBufferUsage
import com.rawr.camera.settings.model.EXTENDED_ISO_MAX
import com.rawr.camera.settings.model.EXTENDED_SHUTTER_MAX_NS
import com.rawr.camera.settings.model.GATE_HOLDS
import com.rawr.camera.settings.model.LensExposure
import com.rawr.camera.settings.model.LensLevels
import com.rawr.camera.settings.model.LensMirror
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.RawStreamChoice
import com.rawr.camera.settings.model.RawStreamFormat
import com.rawr.camera.settings.model.REQUEST_DEFAULTS
import com.rawr.camera.settings.model.SESSION_PARAMS_POLICIES
import com.rawr.camera.settings.model.SettingsUiState
import com.rawr.camera.settings.model.VendorKey
import com.rawr.camera.settings.model.VendorKeyScope
import com.rawr.camera.settings.model.VendorKeyType
import com.rawr.camera.settings.model.effectiveLensProfiles
import com.rawr.camera.settings.model.formatVendorKeyValues
import com.rawr.camera.settings.model.newLensName
import com.rawr.camera.settings.model.parseVendorKeyValues
import com.rawr.camera.settings.model.removeLens
import com.rawr.camera.settings.model.upsertLens
import com.rawr.camera.settings.model.validateLensProfile
import com.rawr.camera.settings.model.validateVendorKey
import com.rawr.camera.settings.preferences.DcpFileStore
import com.rawr.camera.storage.PickFile
import com.rawr.camera.storage.launchPicker
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.Add
import com.rawr.camera.ui.icons.rounded.Check
import com.rawr.camera.ui.icons.rounded.Close
import java.util.Locale
import com.rawr.camera.settings.model.needsJavaCameraApi
import com.rawr.camera.settings.model.resolvedCameraApi


// Lens editor: the Advanced page's session settings and camera workarounds.

/** How the session is opened and configured, past the app's automatic choices. */
@Composable
internal fun SessionAdvancedGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    val session = lens.session
    val update = { change: LensSession.() -> LensSession -> onChange(lens.session(change)) }
    SettingsGroup(title = "Session") {
        DropdownChoiceRow(
            title = "Camera API",
            selected = session.cameraApi,
            values = CAMERA_API_CHOICES.filter { it != "native" || !lens.needsJavaCameraApi() || session.cameraApi == "native" },
            label = { when (it) { "auto" -> "Automatic"; "native" -> "Native"; "java" -> "Java Camera2"; else -> it } },
            supportingText = if (lens.needsJavaCameraApi()) "Automatic runs this lens on Java Camera2: it has an operating mode or a stream use case"
            else "Automatic runs this lens on the native API",
            onSelect = { update { copy(cameraApi = it) } }
        )
        SettingDivider()
        DropdownChoiceRow(
            title = "Session parameters",
            selected = session.params,
            values = SESSION_PARAMS_POLICIES,
            label = {
                when (it) {
                    "cadence" -> "Frame rate and session keys"
                    "none" -> "None"
                    "full" -> "The whole viewfinder request"
                    else -> it
                }
            },
            supportingText = "Sent once when the session is configured. None cannot carry session keys",
            onSelect = { update { copy(params = it) } }
        )
        SettingDivider()
        DropdownChoiceRow(
            title = "Request template",
            selected = session.stillTemplate,
            values = listOf(null, true, false),
            label = { when (it) { true -> "Still capture"; false -> "Preview"; null -> "Automatic" } },
            supportingText = "Automatic: preview for the viewfinder, still capture for full-size photos. " +
                "The others use one template for the session, the viewfinder and photos",
            onSelect = { update { copy(stillTemplate = it) } }
        )
        SettingDivider()
        DropdownChoiceRow(
            title = "Request defaults",
            selected = lens.request.defaults,
            values = REQUEST_DEFAULTS,
            label = { if (it == "minimal") "Minimal" else "App defaults" },
            supportingText = "Minimal leaves distortion and hot-pixel modes, face detection and the AE priority tag to the camera",
            onSelect = { onChange(lens.copy(request = lens.request.copy(defaults = it))) }
        )
        if (lens.streams.photo != null || lens.session.stopViewfinderForPhotos) {
            SettingDivider()
            StreamDuringPhotosRow(lens, onChange)
        }
        FieldColumn {
            CaptureNumberField("Startup delay (ms)", session.startupDelayMs, Modifier.testTag(lensEditorTag("startup_delay")),
                "Wait after the session is configured before the first request, up to 5000") { update { copy(startupDelayMs = it) } }
        }
    }
}

/**
 * Whether the camera's viewfinder stream stops for a full-size photo (stopRepeating, then restored). The screen
 * freezes for every full-size photo either way; this is a camera workaround. A decision key always stops it.
 */
@Composable
private fun StreamDuringPhotosRow(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    val stops = lens.session.stopViewfinderForPhotos
    val java = lens.resolvedCameraApi() == "java"
    if (lens.stillSequence() == StillSequence.DecisionFrame) {
        SettingsRow(
            title = "Viewfinder stream for photos",
            value = "Stops (decision key)",
            supportingText = "The decision key sends one viewfinder frame so the camera switches to full size, then the " +
                "stream stops for the photos and comes back",
            testTag = lensEditorTag("photo_sequence")
        )
        return
    }
    DropdownChoiceRow(
        title = "Viewfinder stream for photos",
        selected = stops,
        values = if (java && !stops) listOf(false) else listOf(false, true),
        label = { if (it) "Stops" else "Keeps running" },
        supportingText = when {
            java && stops -> "Java Camera2 can't stop it; choose Keeps running"
            java -> "Java Camera2 keeps the stream running"
            else -> "Stops: the camera's stream stops before a full-size photo and restarts after it, for cameras that " +
                "only switch to full size with it stopped. The screen freezes for full-size photos either way"
        },
        onSelect = { onChange(lens.session { copy(stopViewfinderForPhotos = it) }) }
    )
}

/** Workarounds the lens's own settings already imply (native effectiveQuirks), with the reason. */
internal fun impliedWorkarounds(lens: LensProfile): Map<String, String> = buildMap {
    if (lens.session.gate.isNotEmpty()) put("holdTapFocus", "on with a vendor gate")
    if (lens.streams.raw.choice.highResLive) {
        put("appPriorityAe", "on with high-res live")
        put("alwaysSendFrameRate", "on with high-res live")
        put("noAutoCompanion", "on with high-res live")
    }
}

@Composable
internal fun WorkaroundsGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    val session = lens.session
    val implied = impliedWorkarounds(lens)
    SettingsGroup(
        title = "Camera workarounds",
        description = "Behaviours a camera crashes or misbehaves on. The device profile can turn some on for every lens."
    ) {
        CAMERA_WORKAROUNDS.forEachIndexed { index, workaround ->
            if (index > 0) SettingDivider()
            val reason = implied[workaround]
            SettingsSwitchRow(
                title = workaroundTitle(workaround),
                checked = workaround in session.workarounds || reason != null,
                value = reason?.let { "Automatic" },
                supportingText = reason?.let { "${workaroundDescription(workaround)}. Automatically $it" } ?: workaroundDescription(workaround),
                enabled = reason == null,
                testTag = lensEditorTag("workaround_$workaround")
            ) { on -> onChange(lens.session { copy(workarounds = if (on) (workarounds + workaround).distinct() else workarounds - workaround) }) }
        }
    }
}

internal fun workaroundTitle(workaround: String) = when (workaround) {
    "holdTapFocus" -> "Keep tap focus until the next tap"
    "appPriorityAe" -> "App AE for shutter and ISO priority"
    "noTapFocus" -> "No tap focus"
    "alwaysSendFrameRate" -> "Always send an fps range"
    "noAutoCompanion" -> "No automatic companion"
    "keepTemplateRegions" -> "Keep the template's focus regions"
    else -> workaround
}

internal fun workaroundDescription(workaround: String) = when (workaround) {
    "holdTapFocus" -> "A tap's focus holds until the next tap; returning to continuous focus crashes this camera"
    "appPriorityAe" -> "The camera's own shutter and ISO priority modes crash it; the app's AE handles them"
    "noTapFocus" -> "Changing the focus region crashes this camera, so tap focus and regions are off"
    "alwaysSendFrameRate" -> "Every request carries an AE fps range, even with AE off"
    "noAutoCompanion" -> "Do not add the small extra stream that unverified devices get"
    "keepTemplateRegions" -> "Without a tap, requests keep the camera's own focus and exposure regions instead of an empty one, which crashes this camera's focus code"
    else -> ""
}
