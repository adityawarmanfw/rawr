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

@Composable
internal fun LensEditorScreen(state: SettingsUiState, lensName: String?, dispatch: SettingsDispatch) {
    val hardware = LocalLensHardware.current
    val lenses = state.values.effectiveLensProfiles(hardware.deviceDefaults)
    val original = lensName?.let { name -> lenses.firstOrNull { it.name.equals(name, ignoreCase = true) } }
    val initial = remember(lensName) { original ?: LensProfile(name = lenses.newLensName(), cameraId = "") }
    var draft by remember(lensName) { mutableStateOf(initial) }
    val inventory by produceState<CameraInventory?>(null) { value = hardware.cameras() }
    val camera = inventory?.find(draft.camera.id, draft.camera.physical)
    val others = lenses.filterNot { original != null && it.name.equals(original.name, ignoreCase = true) }
    val problems = validateLensProfile(draft, others)
    val dirty = draft != initial
    val context = LocalContext.current
    val dcpFiles = remember(context) { DcpFileStore(context.applicationContext) }
    val scope = rememberCoroutineScope()
    var importingDcp by remember { mutableStateOf(false) }
    var dcpError by remember { mutableStateOf<String?>(null) }
    val dcpPicker = rememberLauncherForActivityResult(PickFile()) { uri ->
        if (uri != null) {
            importingDcp = true
            dcpError = null
            scope.launch {
                try {
                    val profile = withContext(Dispatchers.IO) { dcpFiles.import(uri) }
                    draft = draft.image { importDcp(profile) }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    dcpError = e.message ?: "Unable to import camera profile"
                } finally {
                    importingDcp = false
                }
            }
        }
    }

    var pickCamera by remember { mutableStateOf(false) }
    var pickStream by remember { mutableStateOf(false) }
    // Index of the vendor key being edited; -1 adds a new key; null = closed.
    var editKey by remember { mutableStateOf<Int?>(null) }
    var confirmDiscard by remember { mutableStateOf(false) }
    var confirmDelete by remember { mutableStateOf(false) }
    // The Advanced page opens inside the editor so it edits the same unsaved draft.
    var showAdvanced by remember { mutableStateOf(false) }

    val canSave = !importingDcp && problems.isEmpty() && (dirty || original == null)
    val saveAndLeave = {
        dispatch.invoke(SetLensProfiles(lenses.upsertLens(original?.name, draft)))
        dispatch.invoke(NavigateBack)
    }
    val requestBack = {
        if (dirty || importingDcp) confirmDiscard = true else dispatch.invoke(NavigateBack)
    }
    BackHandler(onBack = requestBack)
    BackHandler(enabled = showAdvanced) { showAdvanced = false }

    if (showAdvanced) {
        SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("LensAdvanced")) {
            SettingsGroup(
                title = "Advanced · ${draft.name}",
                description = "The app sets these by itself. Change them only to work around a camera; " +
                    "Save on the lens page keeps them."
            ) {
                SettingsRow(
                    title = "Reset to automatic",
                    enabled = draft.advancedSummary().isNotEmpty(),
                    testTag = lensEditorTag("advanced_reset"),
                    onClick = { draft = draft.withAutomaticAdvanced() }
                )
            }
            CompanionGroup(draft) { draft = it }
            AuxJpegGroup(draft) { draft = it }
            BuffersGroup(draft, camera) { draft = it }
            UseCasesGroup(draft) { draft = it }
            SessionAdvancedGroup(draft) { draft = it }
            WorkaroundsGroup(draft) { draft = it }
            Row(
                Modifier.fillMaxWidth().padding(horizontal = SettingsRowHorizontalPadding),
                horizontalArrangement = Arrangement.End
            ) {
                Button(onClick = { showAdvanced = false }, modifier = Modifier.testTag(lensEditorTag("advanced_done"))) {
                    Text("Done")
                }
            }
        }
    } else SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("LensEditor")) {
        // Most used first; what the app sets by itself is on the Advanced page.
        SettingsGroup(title = "Lens") {
            LensNameField(draft.name) { draft = draft.copy(name = it) }
            SettingDivider()
            SettingsSwitchRow(
                title = "Enabled",
                checked = draft.enabled,
                supportingText = "Shown on the capture screen"
            ) { draft = draft.copy(enabled = it) }
            SettingDivider()
            SettingsRow(
                title = "Camera",
                value = cameraLabel(draft.camera.id, draft.camera.physical),
                supportingText = camera?.let(::cameraDetail),
                testTag = lensEditorTag("camera"),
                onClick = { pickCamera = true }
            )
            SettingDivider()
            DropdownChoiceRow(
                title = "Mirror",
                selected = draft.image.mirror,
                values = LensMirror.entries,
                label = { it.label },
                supportingText = "Flips photos, video and the viewfinder, e.g. for a lens behind a mirror or prism. " +
                    "DNG files keep the sensor pixels and store the flip in their orientation.",
                onSelect = { draft = draft.image { copy(mirror = it) } }
            )
        }
        CaptureGroup(draft, camera, onPickStream = { pickStream = true }) { draft = it }
        ExtendedExposureGroup(draft.image.exposure) { draft = draft.image { copy(exposure = it) } }
        VendorModeGroup(draft, camera, onEditKey = { editKey = it ?: -1 }) { draft = it }
        LevelsGroup(
            title = "Black & white levels",
            description = "Dynamic uses the camera's own levels. Static overrides them on every frame, " +
                "e.g. for vendor sensor modes with a different bit depth.",
            levels = draft.image.levels,
            camera = camera
        ) { draft = draft.image { copy(levels = it) } }
        SensorLayoutGroup(draft.image.sensorLayout) { draft = draft.image { copy(sensorLayout = it) } }
        LensDcpProfiles(
            image = draft.image,
            importing = importingDcp,
            error = dcpError,
            onSelect = { file -> draft = draft.image { selectDcp(file) }; dcpError = null },
            onRemove = { file -> draft = draft.image { removeDcp(file) }; dcpError = null },
            onImport = { dcpPicker.launchPicker(context, arrayOf("*/*")) },
            onMove = { from, to -> draft = draft.image { moveDcp(from, to) } },
            onSort = { descending -> draft = draft.image { sortDcps(descending) } }
        )
        SettingsGroup(title = "Advanced") {
            val changed = draft.advancedSummary()
            SettingsRow(
                title = "Advanced settings",
                value = if (changed.isEmpty()) "All automatic" else changed.joinToString(" · "),
                supportingText = "What the app sets by itself: extra streams, buffers, use cases, session and camera " +
                    "workarounds. Change them only to work around a camera",
                testTag = lensEditorTag("advanced"),
                onClick = { showAdvanced = true }
            )
        }
        if (problems.isNotEmpty()) {
            Text(
                problems.joinToString("\n"),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(horizontal = SettingsRowHorizontalPadding)
            )
        }
        Row(
            Modifier.fillMaxWidth().padding(horizontal = SettingsRowHorizontalPadding),
            horizontalArrangement = Arrangement.spacedBy(12.dp, Alignment.End),
            verticalAlignment = Alignment.CenterVertically
        ) {
            if (original != null) {
                TextButton(onClick = { confirmDelete = true }) { Text("Delete") }
                Box(Modifier.weight(1f))
            }
            OutlinedButton(onClick = requestBack) { Text("Cancel") }
            Button(
                enabled = canSave,
                modifier = Modifier.testTag(lensEditorTag("save")),
                onClick = saveAndLeave
            ) { Text("Save") }
        }
    }

    if (pickCamera) {
        CameraPickerDialog(
            inventory = inventory,
            selectedCameraId = draft.camera.id,
            selectedPhysicalId = draft.camera.physical,
            onDismiss = { pickCamera = false },
            onPick = { picked ->
                draft = draft.copy(camera = LensCamera(picked.openId, picked.physicalId))
                    .raw { copy(choice = streamForCamera(choice, picked)) }
                pickCamera = false
            }
        )
    }
    if (pickStream) {
        StreamPickerDialog(
            camera = camera,
            selected = draft.streams.raw.choice,
            onDismiss = { pickStream = false },
            onPick = {
                draft = draft.raw { copy(choice = it) }
                pickStream = false
            }
        )
    }
    editKey?.let { index ->
        val keys = draft.vendorKeys
        val existing = keys.getOrNull(index)
        VendorKeyDialog(
            initial = existing,
            cameraId = draft.camera.id,
            physicalCameraId = draft.camera.physical,
            scopes = draft.vendorKeyScopes(),
            onDismiss = { editKey = null },
            onDelete = existing?.let {
                {
                    draft = draft.copy(vendorKeys = keys.filterIndexed { i, _ -> i != index })
                    editKey = null
                }
            },
            onSave = { key ->
                draft = draft.copy(
                    vendorKeys = if (existing == null) keys + key else keys.mapIndexed { i, k -> if (i == index) key else k }
                )
                editKey = null
            }
        )
    }
    if (confirmDiscard) {
        AlertDialog(
            onDismissRequest = { confirmDiscard = false },
            title = { Text("Save changes?") },
            text = {
                Text(when {
                    importingDcp -> "Wait for the profile import to finish before saving, or discard your edits."
                    problems.isNotEmpty() -> "Fix the lens settings before saving, or discard your edits."
                    else -> "Your edits to this lens are not saved."
                })
            },
            confirmButton = {
                TextButton(enabled = canSave, onClick = {
                    confirmDiscard = false
                    saveAndLeave()
                }) { Text("Save") }
            },
            dismissButton = {
                Row {
                    TextButton(onClick = { confirmDiscard = false }) { Text("Keep editing") }
                    TextButton(onClick = {
                        confirmDiscard = false
                        dispatch.invoke(NavigateBack)
                    }) { Text("Discard") }
                }
            }
        )
    }
    if (confirmDelete && original != null) {
        val lastEnabled = original.enabled && lenses.count { it.enabled } <= 1
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text("Delete lens ${original.name}?") },
            text = {
                Text(if (lastEnabled) "This is the only enabled lens. Enable another lens first." else "The lens and its keys are removed.")
            },
            confirmButton = {
                TextButton(enabled = !lastEnabled, onClick = {
                    confirmDelete = false
                    dispatch.invoke(SetLensProfiles(lenses.removeLens(original.name)))
                    dispatch.invoke(NavigateBack)
                }) { Text("Delete") }
            },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("Cancel") } }
        )
    }
}

@Composable
internal fun LensNameField(name: String, onChange: (String) -> Unit) {
    OutlinedTextField(
        value = name,
        onValueChange = { onChange(it.replace("\n", "").take(LENS_NAME_MAX_LENGTH)) },
        label = { Text("Name") },
        supportingText = { Text("Capture-screen label, up to $LENS_NAME_MAX_LENGTH characters (e.g. 35, UW, 3x)") },
        singleLine = true,
        modifier =
            Modifier
                .fillMaxWidth()
                .padding(horizontal = SettingsRowHorizontalPadding, vertical = 12.dp)
                .testTag(SettingsTestTags.row("LensEditor", "name"))
    )
}
