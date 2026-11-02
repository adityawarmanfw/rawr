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

// Lens editor: request defaults and the vendor keys, with their editing dialogs.

/** What every request carries: the app's defaults and the lens's vendor keys. */
@Composable
internal fun RequestsAndKeysGroup(lens: LensProfile, onEditKey: (Int?) -> Unit, onChange: (LensProfile) -> Unit) {
    val keys = lens.vendorKeys
    SettingsGroup(
        title = "Requests and keys",
        description = "Session keys are set when each capture session starts; request keys on every request " +
            "(preview, stills, multiframe and video). If an enabled key can't be applied the lens fails to open."
    ) {
        DropdownChoiceRow(
            title = "Request defaults",
            selected = lens.request.defaults,
            values = REQUEST_DEFAULTS,
            label = { if (it == "minimal") "Minimal" else "App defaults" },
            supportingText = "Minimal leaves distortion and hot-pixel modes, face detection and the AE priority tag to the camera",
            onSelect = { onChange(lens.copy(request = lens.request.copy(defaults = it))) }
        )
        keys.forEachIndexed { index, key ->
            SettingDivider()
            SettingsRow(
                title = key.tag,
                value = "${key.scope.label} · ${key.type.label} · ${formatVendorKeyValues(key.values, key.type)}",
                onClick = { onEditKey(index) },
                trailing = {
                    CompactSwitch(
                        checked = key.enabled,
                        onCheckedChange = { on ->
                            onChange(lens.copy(vendorKeys = keys.mapIndexed { i, k -> if (i == index) k.copy(enabled = on) else k }))
                        }
                    )
                }
            )
        }
        SettingDivider()
        SettingsRow(title = "Add key", leadingIcon = Icons.Rounded.Add, onClick = { onEditKey(null) })
    }
}

@Composable
internal fun VendorKeyDialog(
    initial: VendorKey?,
    cameraId: String,
    physicalCameraId: String,
    onDismiss: () -> Unit,
    onDelete: (() -> Unit)?,
    onSave: (VendorKey) -> Unit
) {
    val hardware = LocalLensHardware.current
    var tag by remember { mutableStateOf(initial?.tag.orEmpty()) }
    var scope by remember { mutableStateOf(initial?.scope ?: VendorKeyScope.Session) }
    var type by remember { mutableStateOf(initial?.type ?: VendorKeyType.Int32) }
    var valuesText by remember { mutableStateOf(initial?.let { formatVendorKeyValues(it.values, it.type) }.orEmpty()) }
    var enabled by remember { mutableStateOf(initial?.enabled ?: true) }
    var browsing by remember { mutableStateOf(false) }
    val catalog by produceState<CameraKeyCatalog?>(null, cameraId, physicalCameraId) {
        value = if (cameraId.isBlank()) CameraKeyCatalog(emptyList(), "Choose a camera to list its keys") else hardware.keys(cameraId, physicalCameraId)
    }
    val info = catalog?.find(tag.trim())
    // Auto-detect the type of a known key whenever the tag changes.
    LaunchedEffect(tag, catalog) { info?.type?.let { type = it } }

    val values = parseVendorKeyValues(valuesText)
    val key = VendorKey(tag.trim(), scope, type, values.orEmpty(), enabled)
    val problem = if (values == null) "Values must be numbers, separated by commas" else validateVendorKey(key)

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (initial == null) "Add key" else "Edit key") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = tag,
                    onValueChange = { tag = it.trim() },
                    label = { Text("Key name or tag") },
                    placeholder = { Text("vendor.section.key or 0x80020000") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
                TextButton(onClick = { browsing = true }) { Text("Browse camera keys") }
                Text(
                    keyDetectionText(info, catalog, tag),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text("Applied", style = MaterialTheme.typography.labelLarge)
                CompactChoiceRow(VendorKeyScope.entries, scope, { it.label }) { scope = it }
                Text(
                    when (scope) {
                        VendorKeyScope.Session -> "Set when each capture session starts (photo or video)."
                        VendorKeyScope.Request -> "Set on every capture request (preview, stills, multiframe, video)."
                        VendorKeyScope.Still -> "Set only on full-size still requests (capture-stream lenses)."
                        VendorKeyScope.Decision ->
                            "Set only on one low-ISO viewfinder frame sent before full-size stills (capture-stream lenses)."
                        VendorKeyScope.SessionAndRequest ->
                            "The same value in the session parameters and on every capture request."
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                DropdownChoiceRow(
                    title = "Type",
                    selected = type,
                    values = VendorKeyType.entries,
                    label = { it.label },
                    supportingText = if (info?.type != null) "Detected from the camera" else null
                ) { type = it }
                OutlinedTextField(
                    value = valuesText,
                    onValueChange = { valuesText = it },
                    label = { Text("Value(s)") },
                    supportingText = { Text("Comma-separated for arrays") },
                    singleLine = true,
                    isError = values == null,
                    modifier = Modifier.fillMaxWidth()
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Enabled", Modifier.weight(1f))
                    Switch(checked = enabled, onCheckedChange = { enabled = it })
                }
                if (problem != null && tag.isNotBlank()) {
                    Text(problem, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
                }
                if (onDelete != null) {
                    TextButton(onClick = onDelete) { Text("Delete key", color = MaterialTheme.colorScheme.error) }
                }
            }
        },
        confirmButton = { TextButton(enabled = problem == null, onClick = { onSave(key) }) { Text("Done") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } }
    )

    if (browsing) {
        KeyPickerDialog(
            catalog = catalog,
            onDismiss = { browsing = false },
            onPick = { picked ->
                tag = picked.reference
                picked.type?.let { type = it }
                scope = if (picked.sessionKey) VendorKeyScope.Session else VendorKeyScope.Request
                if (valuesText.isBlank() && picked.defaults.isNotEmpty()) {
                    valuesText = formatVendorKeyValues(picked.defaults, picked.type ?: type)
                }
                browsing = false
            }
        )
    }
}

internal fun keyDetectionText(info: CameraKeyInfo?, catalog: CameraKeyCatalog?, tag: String): String = when {
    tag.isBlank() -> "Type a key name, or browse the keys this camera reports."
    catalog == null -> "Reading camera keys…"
    info == null && catalog.error != null -> catalog.error
    info == null -> "Not reported by this camera; set the type manually."
    info.type != null ->
        "Detected ${info.type.label}" + (if (info.sessionKey) ", session key" else "") +
            (if (info.defaults.isNotEmpty()) ", default ${formatVendorKeyValues(info.defaults, info.type)}" else "")
    info.unsupportedType != null -> "Reported as ${info.unsupportedType}, which can't be set; choose a type manually."
    else -> "Known key" + (if (info.sessionKey) " (session key)" else "") + "; type not detected, set it manually."
}

@Composable
internal fun KeyPickerDialog(catalog: CameraKeyCatalog?, onDismiss: () -> Unit, onPick: (CameraKeyInfo) -> Unit) {
    var query by remember { mutableStateOf("") }
    FullScreenPicker(title = "Camera keys", onDismiss = onDismiss) {
        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            label = { Text("Search") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp)
        )
        when {
            catalog == null -> PickerLoading()
            catalog.error != null && catalog.keys.isEmpty() -> PickerMessage(catalog.error)
            else -> {
                val results = catalog.search(query)
                LazyColumn(Modifier.fillMaxSize()) {
                    if (results.isEmpty()) item { PickerMessage("No matching keys. Type the name in the key field instead.") }
                    items(results, key = { it.reference }) { info ->
                        PickerRow(
                            title = info.reference,
                            detail = buildList {
                                add(info.type?.label ?: info.unsupportedType ?: "type unknown")
                                if (info.sessionKey) add("session key")
                                if (info.defaults.isNotEmpty()) add("default ${info.defaults.joinToString(", ") { formatDefault(it) }}")
                                if (info.name != null && info.tag != null) add("0x" + java.lang.Long.toHexString(info.tag))
                            }.joinToString(" · "),
                            selected = false,
                            onClick = { onPick(info) }
                        )
                    }
                }
            }
        }
    }
}

internal fun formatDefault(value: Double): String =
    if (value == Math.rint(value)) value.toLong().toString() else String.format(Locale.US, "%.4g", value)
