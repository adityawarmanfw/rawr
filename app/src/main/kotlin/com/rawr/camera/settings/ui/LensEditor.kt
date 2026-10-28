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
                    draft = draft.image { copy(dcp = profile) }
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

    val canSave = !importingDcp && problems.isEmpty() && (dirty || original == null)
    val saveAndLeave = {
        dispatch.invoke(SetLensProfiles(lenses.upsertLens(original?.name, draft)))
        dispatch.invoke(NavigateBack)
    }
    val requestBack = {
        if (dirty || importingDcp) confirmDiscard = true else dispatch.invoke(NavigateBack)
    }
    BackHandler(onBack = requestBack)

    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("LensEditor")) {
        // Sections follow the profile: lens and camera, streams, session, requests and keys, image.
        SettingsGroup(title = "Lens and camera") {
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
                testTag = tag("camera"),
                onClick = { pickCamera = true }
            )
        }
        RawStreamGroup(draft, onPickStream = { pickStream = true }) { draft = it }
        CaptureStreamGroup(draft) { draft = it }
        CompanionGroup(draft) { draft = it }
        AuxJpegGroup(draft) { draft = it }
        SessionGroup(draft) { draft = it }
        RequestsAndKeysGroup(draft, onEditKey = { editKey = it ?: -1 }) { draft = it }
        SettingsGroup(title = "Image") {
            DropdownChoiceRow(
                title = "Mirror",
                selected = draft.image.mirror,
                values = LensMirror.entries,
                label = { it.label },
                supportingText = "Flips photos, video and the viewfinder, e.g. for a lens behind a mirror or prism. " +
                    "DNG files keep the sensor pixels and store the flip in their orientation.",
                onSelect = { draft = draft.image { copy(mirror = it) } }
            )
            SettingDivider()
            SettingsRow(
                title = if (importingDcp) "Importing profile…" else "Import DCP file",
                value = draft.image.dcp?.name ?: "Camera default",
                supportingText = "Applies the profile’s colour and tone before your selected look; embeds it in DNG files",
                enabled = !importingDcp,
                testTag = tag("dcp"),
                onClick = { dcpPicker.launchPicker(context, arrayOf("*/*")) }
            )
            if (draft.image.dcp != null) {
                SettingDivider()
                SettingsRow(
                    title = "Use camera default",
                    enabled = !importingDcp,
                    testTag = tag("removeDcp"),
                    onClick = { draft = draft.image { copy(dcp = null) }; dcpError = null }
                )
            }
            dcpError?.let {
                Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(horizontal = SettingsRowHorizontalPadding, vertical = 12.dp))
            }
        }
        LevelsGroup(
            title = "Black & white levels",
            description = "Dynamic uses the camera's own levels. Static overrides them on every frame, " +
                "e.g. for vendor sensor modes with a different bit depth.",
            levels = draft.image.levels,
            camera = camera
        ) { draft = draft.image { copy(levels = it) } }
        SensorLayoutGroup(draft.image.cfa) { draft = draft.image { copy(cfa = it) } }
        ExtendedExposureGroup(draft.image.exposure) { draft = draft.image { copy(exposure = it) } }
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
                modifier = Modifier.testTag(tag("save")),
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
private fun LensNameField(name: String, onChange: (String) -> Unit) {
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

@Composable
private fun LevelsGroup(
    title: String,
    description: String?,
    levels: LensLevels,
    camera: ProbedCamera?,
    onChange: (LensLevels) -> Unit
) {
    val cameraLevels = camera?.let { c ->
        c.blackLevels.takeIf { it.size == 4 && c.whiteLevel > 0 }?.let { black ->
            LensLevels(true, black.map { it.toFloat() }, c.whiteLevel.toFloat())
        }
    }
    SettingsGroup(title = title, description = description) {
        CompactChoiceRow(listOf(false, true), levels.isStatic, { if (it) "Static" else "Dynamic" }) { static ->
            onChange(
                when {
                    !static -> levels.copy(isStatic = false)
                    levels.white > 0f -> levels.copy(isStatic = true)
                    else -> cameraLevels ?: levels.copy(isStatic = true)
                }
            )
        }
        if (levels.isStatic) {
            var perChannel by remember { mutableStateOf(levels.blackRggb.distinct().size > 1) }
            SettingDivider()
            SettingsSwitchRow(title = "Per-channel black levels", checked = perChannel) { on ->
                perChannel = on
                if (!on) onChange(levels.copy(blackRggb = List(4) { levels.blackRggb.first() }))
            }
            if (perChannel) {
                SettingsRow(
                    title = "Black",
                    supportingText = "Per channel: R, Gr, Gb, B",
                    trailing = {
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            listOf("R", "Gr", "Gb", "B").forEachIndexed { channel, label ->
                                LevelInput(levels.blackRggb[channel], width = 60.dp, placeholder = label) { v ->
                                    onChange(levels.copy(blackRggb = levels.blackRggb.mapIndexed { i, b -> if (i == channel) v else b }))
                                }
                            }
                        }
                    }
                )
            } else {
                LevelField("Black", levels.blackRggb.first()) { v -> onChange(levels.copy(blackRggb = List(4) { v })) }
            }
            LevelField("White", levels.white) { v -> onChange(levels.copy(white = v)) }
            if (cameraLevels != null && cameraLevels != levels) {
                TextButton(
                    onClick = {
                        perChannel = cameraLevels.blackRggb.distinct().size > 1
                        onChange(cameraLevels)
                    },
                    modifier = Modifier.padding(horizontal = 8.dp)
                ) {
                    Text("Use camera values (black ${cameraLevels.blackRggb.joinToString("/") { formatLevel(it) }}, white ${formatLevel(cameraLevels.white)})")
                }
            }
        }
    }
}

@Composable
private fun LevelInput(value: Float, modifier: Modifier = Modifier, width: Dp = 104.dp, placeholder: String = "0", onValue: (Float) -> Unit) {
    var text by remember { mutableStateOf(formatLevel(value)) }
    LaunchedEffect(value) { if (text.toFloatOrNull() != value) text = formatLevel(value) }
    CompactInput(text, { t ->
        text = t
        t.toFloatOrNull()?.takeIf { it.isFinite() }?.let(onValue)
    }, modifier, width = width, placeholder = placeholder, isError = text.toFloatOrNull() == null, keyboard = KeyboardType.Decimal)
}

@Composable
private fun LevelField(label: String, value: Float, hint: String? = null, onValue: (Float) -> Unit) {
    SettingsRow(title = label, supportingText = hint, trailing = { LevelInput(value, onValue = onValue) })
}

private fun formatLevel(value: Float): String =
    if (value == Math.round(value).toFloat()) Math.round(value).toString() else value.toString()

@Composable
private fun SensorLayoutGroup(cfa: LensCfa, onChange: (LensCfa) -> Unit) {
    SettingsGroup(
        title = "Sensor layout",
        description = "For RAW that comes out as coloured blocks or with wrong colours: some sensor modes " +
            "(often in-sensor crops) deliver the quad or 4x4 mosaic while reporting Bayer. Quad and 4x4 are " +
            "remosaiced to Bayer at full resolution. Colour order fixes swapped red/blue or magenta-green " +
            "casts; for quad and 4x4 it is the order of the blocks."
    ) {
        CompactChoiceRow(CFA_BLOCKS, cfa.block, ::cfaBlockLabel) { onChange(cfa.copy(block = it)) }
        SettingDivider()
        DropdownChoiceRow(
            title = "Colour order",
            selected = cfa.order,
            values = listOf<String?>(null) + CFA_ORDERS,
            label = { it ?: "Camera's" },
            onSelect = { onChange(cfa.copy(order = it)) }
        )
        if (cfa.block > 1) {
            SettingDivider()
            DropdownChoiceRow(
                title = "Live remosaic",
                selected = cfa.live,
                values = CFA_LIVE_QUALITIES,
                label = ::cfaQualityLabel,
                supportingText = "Viewfinder, video and merged photos (their frames come from the live stream). " +
                    "Fast skips green interpolation: about 20% cheaper, softer colour detail.",
                onSelect = { onChange(cfa.copy(live = it)) }
            )
            SettingDivider()
            DropdownChoiceRow(
                title = "Photo remosaic",
                selected = cfa.photo,
                values = CFA_PHOTO_QUALITIES,
                label = ::cfaQualityLabel,
                supportingText = "Single photos. High searches wider for clean edges; adds ~0.15 s per photo.",
                onSelect = { onChange(cfa.copy(photo = it)) }
            )
        }
    }
}

internal fun cfaQualityLabel(quality: String): String = when (quality) {
    "fast" -> "Fast"
    "high" -> "High"
    else -> "Standard"
}

internal fun cfaBlockLabel(block: Int): String = when (block) {
    2 -> "Quad 2×2"
    4 -> "4×4"
    else -> "Bayer"
}

@Composable
private fun ExtendedExposureGroup(exposure: LensExposure, onChange: (LensExposure) -> Unit) {
    SettingsGroup(
        title = "Exposure range",
        description = "Allow ISO and shutter requests beyond what the camera advertises. Some lenses honor them; " +
            "others clamp silently (a long exposure the camera shortened is flagged after capture). " +
            "Shutters longer than 1/5 s run only when you press the shutter. 0 keeps the camera's limit."
    ) {
        SettingsSwitchRow(title = "Allow requests outside range", checked = exposure.extended) {
            onChange(exposure.copy(extended = it))
        }
        if (exposure.extended) {
            LevelField("Highest ISO", exposure.isoMax.toFloat(), "0 keeps the camera's limit") { v ->
                onChange(exposure.copy(isoMax = v.toInt().coerceIn(0, EXTENDED_ISO_MAX)))
            }
            LevelField("Longest shutter (s)", exposure.shutterMaxNs / 1e9f, "Up to 25 s. 0 keeps the camera's limit") { v ->
                onChange(exposure.copy(shutterMaxNs = (v.toDouble() * 1e9).toLong().coerceIn(0L, EXTENDED_SHUTTER_MAX_NS)))
            }
        }
    }
}



@Composable
private fun VendorKeyDialog(
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

private fun keyDetectionText(info: CameraKeyInfo?, catalog: CameraKeyCatalog?, tag: String): String = when {
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
private fun KeyPickerDialog(catalog: CameraKeyCatalog?, onDismiss: () -> Unit, onPick: (CameraKeyInfo) -> Unit) {
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

private fun formatDefault(value: Double): String =
    if (value == Math.rint(value)) value.toLong().toString() else String.format(Locale.US, "%.4g", value)

@Composable
private fun CameraPickerDialog(
    inventory: CameraInventory?,
    selectedCameraId: String,
    selectedPhysicalId: String,
    onDismiss: () -> Unit,
    onPick: (ProbedCamera) -> Unit
) {
    FullScreenPicker(title = "Camera", onDismiss = onDismiss) {
        when {
            inventory == null -> PickerLoading()
            inventory.cameras.isEmpty() -> PickerMessage(inventory.error ?: "No cameras found")
            else -> LazyColumn(Modifier.fillMaxSize()) {
                inventory.grouped().forEach { (facing, cameras) ->
                    item(key = "header:${facing.name}") { PickerHeader(facing.label) }
                    items(cameras, key = { "${it.parentId}:${it.id}" }) { c ->
                        PickerRow(
                            title = cameraLabel(c.openId, c.physicalId),
                            detail = cameraDetail(c),
                            selected = c.openId == selectedCameraId && c.physicalId == selectedPhysicalId,
                            onClick = { onPick(c) }
                        )
                    }
                }
            }
        }
    }
}

private fun cameraDetail(c: ProbedCamera): String = buildList {
    if (c.equivalentFocalMm > 0f) add("${Math.round(c.equivalentFocalMm)} mm eq.")
    c.focalLengthsMm.firstOrNull()?.let { add("f ${String.format(Locale.US, "%.2f", it)} mm") }
    when {
        c.parentId != null -> add("physical")
        c.logical -> add("logical (${c.physicalIds.joinToString(", ")})")
    }
    if (c.parentId == null && !c.enumerated) add("hidden id")
    val usable = c.rawStreams.filter { it.supported }
    add(if (usable.isEmpty()) "no usable RAW" else usable.maxBy { it.width.toLong() * it.height }.label)
}.joinToString(" · ")

/** Keeps the stream when the new camera offers it; otherwise the largest of a format it supports. */
private fun streamForCamera(current: RawStreamChoice, camera: ProbedCamera): RawStreamChoice {
    val usable = camera.rawStreams.filter { it.supported }
    if (current.isLargest && usable.any { it.format == current.format }) return current
    if (usable.any { it.format == current.format && it.width == current.width && it.height == current.height }) return current
    val format = usable.firstOrNull { it.format == RawStreamFormat.Raw16 }?.format ?: usable.firstOrNull()?.format ?: current.format
    return RawStreamChoice(format)
}



private fun quirkTitle(quirk: String) = when (quirk) {
    "afTapHold" -> "Keep tap focus until the next tap"
    "noHalPriorityAe" -> "App AE for shutter and ISO priority"
    "noTapAf" -> "No tap focus"
    "requireFpsRange" -> "Always send an fps range"
    "noAutoCompanion" -> "No automatic companion"
    else -> quirk
}

private fun quirkDescription(quirk: String) = when (quirk) {
    "afTapHold" -> "A tap's focus holds until the next tap; returning to continuous focus crashes this camera"
    "noHalPriorityAe" -> "The camera's own shutter and ISO priority modes crash it; the app's AE handles them"
    "noTapAf" -> "Changing the focus region crashes this camera, so tap focus and regions are off"
    "requireFpsRange" -> "Every request carries an AE fps range, even with AE off"
    "noAutoCompanion" -> "Do not add the small extra stream that unverified devices get"
    else -> ""
}

private fun tag(slug: String) = SettingsTestTags.row("LensEditor", slug)

/** Input rows stacked inside a group. */
@Composable
private fun FieldColumn(content: @Composable () -> Unit) {
    Column(Modifier.fillMaxWidth()) { content() }
}

@Composable
private fun SizeFields(
    title: String,
    size: LensOutputSize,
    tagPrefix: String,
    hint: String? = null,
    onChange: (LensOutputSize) -> Unit
) {
    var width by remember { mutableStateOf(if (size.width == 0) "" else size.width.toString()) }
    var height by remember { mutableStateOf(if (size.height == 0) "" else size.height.toString()) }
    fun number(text: String) = if (text.isEmpty()) 0 else text.toIntOrNull() ?: -1
    SettingsRow(
        title = title,
        supportingText = hint,
        trailing = {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                CompactInput(width, { t -> if (t.all { it in '0'..'9' }) { width = t; onChange(size.copy(width = number(t))) } },
                    Modifier.testTag(tag("${tagPrefix}_width")), width = 80.dp, placeholder = "W")
                Text("×", style = MaterialTheme.typography.bodyMedium)
                CompactInput(height, { t -> if (t.all { it in '0'..'9' }) { height = t; onChange(size.copy(height = number(t))) } },
                    Modifier.testTag(tag("${tagPrefix}_height")), width = 80.dp, placeholder = "H")
            }
        }
    )
}

/** The viewfinder RAW stream and its reader. */
@Composable
private fun RawStreamGroup(lens: LensProfile, onPickStream: () -> Unit, onChange: (LensProfile) -> Unit) {
    val raw = lens.streams.raw
    SettingsGroup(title = "RAW stream", description = "The stream the viewfinder, video and normal photos read.") {
        SettingsRow(
            title = if (lens.streams.capture != null) "Preview stream" else "Stream",
            value = raw.choice.label,
            supportingText = "Falls back to the largest usable stream if this one isn't offered",
            enabled = lens.camera.id.isNotBlank(),
            onClick = onPickStream
        )
        FieldColumn {
            CaptureNumberField("Stream use case", raw.useCase, Modifier.testTag(tag("use_case_raw")),
                "Java Camera2 only. 0 = not set; standard values are 1 to 6, anything else is the vendor's") {
                onChange(lens.raw { copy(useCase = it) })
            }
            UsageField("Buffer usage", raw.reader.usage, Modifier.testTag(tag("reader_usage")),
                "Empty: the app chooses (GPU first, CPU if the camera refuses). Or gpuDataBuffer, none, or a 0x hex value") { onChange(lens.raw { copy(reader = reader.copy(usage = it)) }) }
            CaptureNumberField("Buffers", raw.reader.maxImages, Modifier.testTag(tag("reader_images")),
                "RAW frames the reader holds, 6 to 16. 0 = 6") { onChange(lens.raw { copy(reader = reader.copy(maxImages = it)) }) }
        }
        SettingsSwitchRow(
            title = "Test the buffer usage first",
            checked = raw.reader.probe,
            supportingText = "With a buffer usage set: ask the allocator before using it, and use CPU buffers if it refuses",
            enabled = raw.reader.usage != null,
            testTag = tag("reader_probe")
        ) { onChange(lens.raw { copy(reader = reader.copy(probe = it)) }) }
        SettingDivider()
        SettingsSwitchRow(
            title = "Fall back if no frames arrive",
            checked = raw.reader.demote,
            supportingText = "With a buffer usage set: when no frame arrives, retry with the app's own choice",
            enabled = raw.reader.usage != null,
            testTag = tag("reader_demote")
        ) { onChange(lens.raw { copy(reader = reader.copy(demote = it)) }) }
    }
}

/** A second RAW stream that only still requests fill (vendor full-size modes). */
@Composable
private fun CaptureStreamGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    var retained by remember { mutableStateOf(lens.streams.capture ?: CaptureStreamSettings(RawStreamChoice())) }
    val capture = lens.streams.capture
    val update = { change: CaptureStreamSettings.() -> CaptureStreamSettings ->
        capture?.let { onChange(lens.streams { copy(capture = it.change()) }) }
    }
    SettingsGroup(
        title = "Full-size capture stream",
        description = "A second RAW stream that only photos fill, for vendor full-resolution modes (vivo 50 and 200 MP)."
    ) {
        SettingsSwitchRow(
            title = "Capture stream",
            checked = capture != null,
            supportingText = "Off: photos come from the RAW stream above",
            testTag = tag("capture_stream")
        ) { enabled ->
            capture?.let { retained = it }
            onChange(lens.streams { copy(capture = if (enabled) retained else null) })
        }
        if (capture != null) {
            SettingDivider()
            DropdownChoiceRow(
                title = "Format",
                selected = capture.choice.format,
                values = RawStreamFormat.entries,
                label = { it.label },
                onSelect = { update { copy(choice = choice.copy(format = it)) } }
            )
            FieldColumn {
                SizeFields(
                    if (capture.choice.width > 0 && capture.choice.height > 0)
                        String.format(Locale.ROOT, "Size · %.1f MP", capture.choice.width.toDouble() * capture.choice.height / 1_000_000)
                    else "Size",
                    LensOutputSize(capture.choice.width, capture.choice.height), "capture",
                    "The sensor mode's full-size dimensions, including sizes the camera doesn't advertise"
                ) { update { copy(choice = choice.copy(width = it.width, height = it.height)) } }
                CaptureNumberField("Stream use case", capture.useCase, Modifier.testTag(tag("use_case_capture")),
                    "Java Camera2 only. 0 = not set") { update { copy(useCase = it) } }
                UsageField("Buffer usage", capture.readerUsage, Modifier.testTag(tag("capture_reader_usage")),
                    "Empty: GPU data buffer, the one vivo full-size modes accept") { update { copy(readerUsage = it) } }
                CaptureNumberField("Most frames per photo", capture.burstMax, Modifier.testTag(tag("capture_burst")),
                    "Ceiling for this lens. Choose the requested count in Multiframe → Maximum Frames. " +
                        "0 = app limit (30) · 1 = single frame") { update { copy(burstMax = it) } }
            }
            DropdownChoiceRow(
                title = "Send a multi-frame photo",
                selected = capture.burstSequential,
                values = listOf(false, true),
                label = { if (it) "One frame at a time" else "All frames at once" },
                supportingText = if (capture.burstSequential) "Each frame is requested after the previous one arrives"
                else "All frames are requested together; the camera sets the timing",
                onSelect = { update { copy(burstSequential = it) } }
            )
        }
    }
}

/** The companion: a small output beside RAW that some HALs need before they deliver RAW. */
@Composable
private fun CompanionGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    val companion = lens.streams.companion
    SettingsGroup(
        title = "Companion stream",
        description = "A small extra output next to RAW. Some cameras deliver no RAW without one."
    ) {
        DropdownChoiceRow(
            title = "Mode",
            selected = companion.mode,
            values = CompanionMode.entries,
            label = { it.label },
            supportingText = when (companion.mode) {
                CompanionMode.Auto -> "Added on unverified devices and lenses with a capture stream, sized to the RAW aspect"
                CompanionMode.Fixed -> "Always configured, at the size below: part of this lens's mode"
                CompanionMode.Off -> "Never configured, even where the app would add one"
            },
            onSelect = { mode ->
                onChange(lens.companion { copy(mode = mode, fallback = if (mode == CompanionMode.Fixed) "base" else fallback) })
            }
        )
        if (companion.mode != CompanionMode.Off) {
            if (companion.mode == CompanionMode.Fixed) {
                FieldColumn {
                    SizeFields("Size", companion.size, "companion") { onChange(lens.companion { copy(size = it) }) }
                }
            } else {
                SettingDivider()
                DropdownChoiceRow(
                    title = "If no size matches the RAW aspect",
                    selected = companion.fallback,
                    values = COMPANION_FALLBACKS,
                    label = { when (it) { "nearest" -> "Nearest aspect"; "none" -> "No companion"; else -> "Default size" } },
                    onSelect = { onChange(lens.companion { copy(fallback = it) }) }
                )
            }
            SettingDivider()
            DropdownChoiceRow(
                title = "Format",
                selected = companion.format,
                values = listOf("yuv", "private"),
                label = { if (it == "private") "PRIVATE (GPU)" else "YUV 420" },
                onSelect = { onChange(lens.companion { copy(format = it) }) }
            )
            SettingDivider()
            SettingsSwitchRow(
                title = "Fill on every frame",
                checked = companion.targeted,
                supportingText = "Off: configured with the session, but no request fills it",
                testTag = tag("companion_targeted")
            ) { onChange(lens.companion { copy(targeted = it) }) }
            SettingDivider()
            DropdownChoiceRow(
                title = "Stop filling once RAW arrives",
                selected = companion.detach,
                values = listOf(null, true, false),
                label = { when (it) { true -> "Yes"; false -> "No"; null -> "Default" } },
                supportingText = "Default: an automatic companion stops after the first RAW frame with its metadata; a fixed one keeps going",
                onSelect = { onChange(lens.companion { copy(detach = it) }) }
            )
            SettingDivider()
            DropdownChoiceRow(
                title = "On the physical camera",
                selected = companion.physical,
                values = listOf(null, true, false),
                label = { when (it) { true -> "Yes"; false -> "No, the logical camera"; null -> "Same as the lens" } },
                supportingText = "Only matters for a lens on a physical camera inside a logical one",
                onSelect = { onChange(lens.companion { copy(physical = it) }) }
            )
            FieldColumn {
                CaptureNumberField("Buffers", companion.maxImages, Modifier.testTag(tag("companion_images")),
                    "Frames its reader holds, 2 to 16. 0 = 2") { onChange(lens.companion { copy(maxImages = it) }) }
                UsageField("Buffer usage", companion.usage, Modifier.testTag(tag("companion_usage")),
                    "Empty: GPU sampling for PRIVATE, the reader's own for YUV. Or gpuDataBuffer, none, or a 0x hex value") {
                    onChange(lens.companion { copy(usage = it) })
                }
                CaptureNumberField("Stream use case", companion.useCase, Modifier.testTag(tag("use_case_companion")),
                    "Java Camera2 only. 0 = not set") { onChange(lens.companion { copy(useCase = it) }) }
            }
        }
    }
}

@Composable
private fun AuxJpegGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    val aux = lens.streams.auxJpeg
    val update = { change: AuxJpegSettings.() -> AuxJpegSettings ->
        aux?.let { onChange(lens.streams { copy(auxJpeg = it.change()) }) }
    }
    SettingsGroup(title = "Aux JPEG stream") {
        SettingsSwitchRow(
            title = "Add a JPEG stream",
            checked = aux != null,
            supportingText = "Configured with the session but never filled. Some vendor modes only start with one",
            testTag = tag("aux_jpeg")
        ) { onChange(lens.streams { copy(auxJpeg = if (it) auxJpeg ?: AuxJpegSettings(LensOutputSize(4096, 3072)) else null) }) }
        if (aux != null) {
            FieldColumn {
                SizeFields("Size", aux.size, "aux_jpeg") { update { copy(size = it) } }
                UsageField("Buffer usage", aux.usage, Modifier.testTag(tag("aux_jpeg_usage"))) { update { copy(usage = it) } }
                CaptureNumberField("Stream use case", aux.useCase, Modifier.testTag(tag("use_case_aux_jpeg")),
                    "Java Camera2 only. 0 = not set") { update { copy(useCase = it) } }
            }
        }
    }
}

/** How the session is opened and configured. */
@Composable
private fun SessionGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
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
            CaptureNumberField("Operating mode", session.opMode, Modifier.testTag(tag("op_mode")),
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
            CaptureNumberField("Fixed frame rate", session.fixedFps, Modifier.testTag(tag("fixed_fps")),
                "One AE fps range on the session and every request. 0 = the app varies it") { update { copy(fixedFps = it) } }
            CaptureNumberField("Startup delay (ms)", session.startupDelayMs, Modifier.testTag(tag("startup_delay")),
                "Wait after the session is configured before the first request, up to 5000") { update { copy(startupDelayMs = it) } }
        }
        DropdownChoiceRow(
            title = "Vendor gate",
            selected = session.gate,
            values = (listOf("", "vivoFullSize") + session.gate).distinct(),
            label = { when (it) { "" -> "None"; "vivoFullSize" -> "vivo full-size"; else -> it } },
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
            testTag = tag("photo_sequence")
        )
        SettingDivider()
        SettingsSwitchRow(
            title = "Always pause the viewfinder for photos",
            checked = session.stopRepeatingForStill,
            supportingText = "Pause, take the photo, restore. A lens with a decision key already pauses for full-size photos",
            testTag = tag("stop_repeating")
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
                testTag = tag("quirk_$quirk")
            ) { on -> update { copy(quirks = if (on) (quirks + quirk).distinct() else quirks - quirk) } }
        }
    }
}

/** What every request carries: the app's defaults and the lens's vendor keys. */
@Composable
private fun RequestsAndKeysGroup(lens: LensProfile, onEditKey: (Int?) -> Unit, onChange: (LensProfile) -> Unit) {
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

/** A reader usage: "gpuDataBuffer", "none" or a 0x hex value; empty means null. */
/** A small boxed text field for the end of a settings row. */
@Composable
private fun CompactInput(
    text: String,
    onText: (String) -> Unit,
    modifier: Modifier = Modifier,
    width: Dp = 104.dp,
    placeholder: String = "0",
    isError: Boolean = false,
    keyboard: KeyboardType = KeyboardType.Number
) {
    val colors = MaterialTheme.colorScheme
    BasicTextField(
        value = text,
        onValueChange = onText,
        singleLine = true,
        textStyle = MaterialTheme.typography.bodyMedium.copy(color = colors.onSurface, textAlign = TextAlign.End),
        keyboardOptions = KeyboardOptions(keyboardType = keyboard),
        cursorBrush = SolidColor(colors.primary),
        modifier = modifier.width(width),
        decorationBox = { inner ->
            Box(
                Modifier
                    .border(1.dp, if (isError) colors.error else colors.outline, RoundedCornerShape(8.dp))
                    .padding(horizontal = 10.dp, vertical = 8.dp),
                contentAlignment = Alignment.CenterEnd
            ) {
                if (text.isEmpty()) {
                    Text(placeholder, style = MaterialTheme.typography.bodyMedium, color = colors.onSurfaceVariant)
                }
                inner()
            }
        }
    )
}

/** A reader usage: "gpuDataBuffer", "none" or a 0x hex value; empty means null (the default). */
@Composable
private fun UsageField(
    label: String,
    value: String?,
    modifier: Modifier = Modifier,
    hint: String = "Empty: the default. Or gpuDataBuffer, none, or a 0x hex value",
    onChange: (String?) -> Unit
) {
    var text by remember { mutableStateOf(value.orEmpty()) }
    val bad = text.isNotEmpty() && !isReaderUsage(text)
    SettingsRow(
        title = label,
        supportingText = if (bad) "Not a usage. $hint" else hint,
        trailing = {
            CompactInput(text, { input ->
                text = input.trim()
                onChange(text.ifEmpty { null })
            }, modifier, width = 160.dp, placeholder = "default", isError = bad, keyboard = KeyboardType.Ascii)
        }
    )
}

@Composable
private fun CaptureNumberField(
    label: String,
    value: Int,
    modifier: Modifier = Modifier,
    hint: String? = null,
    onChange: (Int) -> Unit
) {
    var text by remember { mutableStateOf(if (value == 0) "" else value.toString()) }
    SettingsRow(
        title = label,
        supportingText = hint,
        trailing = {
            CompactInput(text, { input ->
                if (input.all { it in '0'..'9' }) {
                    text = input
                    onChange(if (input.isEmpty()) 0 else input.toIntOrNull() ?: -1)
                }
            }, modifier)
        }
    )
}

@Composable
private fun StreamPickerDialog(
    camera: ProbedCamera?,
    selected: RawStreamChoice,
    onDismiss: () -> Unit,
    onPick: (RawStreamChoice) -> Unit
) {
    FullScreenPicker(title = "RAW stream", onDismiss = onDismiss) {
        LazyColumn(Modifier.fillMaxSize()) {
            if (camera == null) item { PickerMessage("Camera details unavailable; only the largest stream can be chosen.") }
            val formats = camera?.rawStreams?.mapNotNull { it.format }?.distinct() ?: RawStreamFormat.entries
            items(formats, key = { "largest:${it.name}" }) { format ->
                val choice = RawStreamChoice(format)
                PickerRow(
                    title = choice.label,
                    detail = "Largest ${format.label} size this camera offers",
                    selected = selected.isLargest && selected.format == format,
                    onClick = { onPick(choice) }
                )
            }
            if (camera != null) {
                item { PickerHeader("Advertised streams") }
                items(camera.rawStreams, key = { "${it.formatName}:${it.width}x${it.height}" }) { s ->
                    val format = s.format
                    PickerRow(
                        title = s.label,
                        detail = if (s.supported) null else "Not supported by the capture pipeline",
                        selected = format != null && !selected.isLargest && selected.format == format &&
                            selected.width == s.width && selected.height == s.height,
                        enabled = s.supported && format != null,
                        onClick = { if (format != null) onPick(RawStreamChoice(format, s.width, s.height)) }
                    )
                }
            }
        }
    }
}

@Composable
private fun FullScreenPicker(title: String, onDismiss: () -> Unit, content: @Composable () -> Unit) {
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
            Column(Modifier.fillMaxSize()) {
                Row(Modifier.fillMaxWidth().padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = onDismiss) { Icon(Icons.Rounded.Close, contentDescription = "Close") }
                    Text(title, style = MaterialTheme.typography.titleLarge)
                }
                content()
            }
        }
    }
}

@Composable
private fun PickerHeader(text: String) {
    Text(
        text.uppercase(),
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp)
    )
}

@Composable
private fun PickerRow(title: String, detail: String?, selected: Boolean, enabled: Boolean = true, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .then(if (enabled) Modifier.clickable(onClick = onClick) else Modifier)
            .heightIn(min = SettingsRowMinHeight)
            .padding(horizontal = SettingsRowHorizontalPadding, vertical = SettingsRowVerticalPadding),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            val alpha = if (enabled) 1f else .45f
            Text(title, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurface.copy(alpha = alpha))
            if (detail != null) {
                Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = alpha))
            }
        }
        if (selected) Icon(Icons.Rounded.Check, contentDescription = "Selected", tint = MaterialTheme.colorScheme.primary)
    }
}

@Composable
private fun PickerLoading() {
    Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
}

@Composable
private fun PickerMessage(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(16.dp)
    )
}
