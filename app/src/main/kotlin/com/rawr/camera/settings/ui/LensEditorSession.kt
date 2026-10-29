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
import com.rawr.camera.settings.model.CFA_BLOCKS
import com.rawr.camera.settings.model.CFA_LIVE_QUALITIES
import com.rawr.camera.settings.model.CFA_ORDERS
import com.rawr.camera.settings.model.CFA_PHOTO_QUALITIES
import com.rawr.camera.settings.model.BACKEND_CHOICES
import com.rawr.camera.settings.model.CAMERA_QUIRKS
import com.rawr.camera.settings.model.COMPANION_FALLBACKS
import com.rawr.camera.settings.model.AuxJpegSettings
import com.rawr.camera.settings.model.CaptureStreamSettings
import com.rawr.camera.settings.model.CompanionMode
import com.rawr.camera.settings.model.LensCamera
import com.rawr.camera.settings.model.LensCfa
import com.rawr.camera.settings.model.LensOutputSize
import com.rawr.camera.settings.model.LensSession
import com.rawr.camera.settings.model.StillSequence
import com.rawr.camera.settings.model.stillSequence
import com.rawr.camera.settings.model.isReaderUsage
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

// Lens editor: how the session is opened and configured, and the camera workarounds.

/** How the session is opened and configured. */
@Composable
internal fun SessionGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    val session = lens.session
    val update = { change: LensSession.() -> LensSession -> onChange(lens.session(change)) }
    SettingsGroup(
        title = "Session",
        description = "How this lens opens the camera and configures the session. The defaults match the app's normal behaviour."
    ) {
        DropdownChoiceRow(
            title = "Camera API",
            selected = session.backend,
            values = BACKEND_CHOICES,
            label = { when (it) { "auto" -> "Automatic"; "ndk" -> "Native (NDK)"; "java" -> "Java Camera2"; else -> it } },
            supportingText = "Automatic: Java Camera2 when an operating mode or a stream use case is set (the NDK " +
                "cannot send them), Native otherwise",
            onSelect = { update { copy(backend = it) } }
        )
        FieldColumn {
            CaptureNumberField("Operating mode", session.opMode, Modifier.testTag(lensEditorTag("op_mode")),
                "Java Camera2 only. 0 = normal. Vendor modes are usually 32768 (0x8000) and above, in decimal. 1 and 2 are refused") {
                update { copy(opMode = it) }
            }
        }
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
            label = { when (it) { true -> "Still capture"; false -> "Preview"; null -> "Default" } },
            supportingText = "Default: preview for the viewfinder, still capture for full-size photos. " +
                "The others use one template for the session, the viewfinder and photos",
            onSelect = { update { copy(stillTemplate = it) } }
        )
        FieldColumn {
            CaptureNumberField("Fixed frame rate", session.fixedFps, Modifier.testTag(lensEditorTag("fixed_fps")),
                "One AE fps range on the session and every request. 0 = the app varies it") { update { copy(fixedFps = it) } }
            CaptureNumberField("Startup delay (ms)", session.startupDelayMs, Modifier.testTag(lensEditorTag("startup_delay")),
                "Wait after the session is configured before the first request, up to 5000") { update { copy(startupDelayMs = it) } }
        }
        DropdownChoiceRow(
            title = "Vendor gate",
            selected = session.gate,
            values = (listOf("", "vivoFullSize", "vivoFullSizeVcf") + session.gate).distinct(),
            label = { when (it) { "" -> "None"; "vivoFullSize" -> "vivo full-size"; "vivoFullSizeVcf" -> "vivo full-size + VCF bypass"; else -> it } },
            supportingText = "Vendor state the app sets while it opens the camera; vivo needs it for 50 and 200 MP",
            onSelect = { update { copy(gate = it, gateHold = if (it.isEmpty()) null else gateHold) } }
        )
        if (session.gate.isNotEmpty()) {
            SettingDivider()
            DropdownChoiceRow(
                title = "Keep the gate",
                selected = session.gateHold.orEmpty(),
                values = listOf("") + GATE_HOLDS,
                label = {
                    when (it) {
                        "" -> "Default"
                        "configured" -> "Until the session is configured"
                        "closed" -> "Until the camera closes"
                        else -> it
                    }
                },
                supportingText = "Default: until the camera closes on a lens with a capture stream, otherwise until configured",
                onSelect = { update { copy(gateHold = it.ifEmpty { null }) } }
            )
        }
        SettingDivider()
        val sequence = lens.stillSequence()
        SettingsRow(
            title = "Photo sequence",
            value = when (sequence) {
                StillSequence.DecisionFrame -> "Decision frame, then pause"
                StillSequence.PauseViewfinder -> "Pause, then photo"
                StillSequence.Direct -> "Beside the viewfinder"
            },
            supportingText = when (sequence) {
                StillSequence.DecisionFrame -> "Set by the decision key with the capture stream and companion: one frame lets the " +
                    "camera switch to full size, the viewfinder pauses for the photos and comes back"
                StillSequence.PauseViewfinder -> "The viewfinder pauses for the photo and comes back"
                StillSequence.Direct -> "Photos are taken while the viewfinder keeps running"
            },
            testTag = lensEditorTag("photo_sequence")
        )
        SettingDivider()
        SettingsSwitchRow(
            title = "Always pause the viewfinder for photos",
            checked = session.stopRepeatingForStill,
            supportingText = "Pause, take the photo, restore. A lens with a decision key already pauses for full-size photos",
            testTag = lensEditorTag("stop_repeating")
        ) { update { copy(stopRepeatingForStill = it) } }
    }
    SettingsGroup(
        title = "Camera workarounds",
        description = "Behaviours this camera crashes or misbehaves on. The device profile may already set some for every lens."
    ) {
        CAMERA_QUIRKS.forEachIndexed { index, quirk ->
            if (index > 0) SettingDivider()
            SettingsSwitchRow(
                title = quirkTitle(quirk),
                checked = quirk in session.quirks,
                supportingText = quirkDescription(quirk),
                testTag = lensEditorTag("quirk_$quirk")
            ) { on -> update { copy(quirks = if (on) (quirks + quirk).distinct() else quirks - quirk) } }
        }
    }
}

internal fun quirkTitle(quirk: String) = when (quirk) {
    "afTapHold" -> "Keep tap focus until the next tap"
    "noHalPriorityAe" -> "App AE for shutter and ISO priority"
    "noTapAf" -> "No tap focus"
    "requireFpsRange" -> "Always send an fps range"
    "noAutoCompanion" -> "No automatic companion"
    else -> quirk
}

internal fun quirkDescription(quirk: String) = when (quirk) {
    "afTapHold" -> "A tap's focus holds until the next tap; returning to continuous focus crashes this camera"
    "noHalPriorityAe" -> "The camera's own shutter and ISO priority modes crash it; the app's AE handles them"
    "noTapAf" -> "Changing the focus region crashes this camera, so tap focus and regions are off"
    "requireFpsRange" -> "Every request carries an AE fps range, even with AE off"
    "noAutoCompanion" -> "Do not add the small extra stream that unverified devices get"
    else -> ""
}
