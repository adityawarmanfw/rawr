package com.rawr.camera.settings.ui

import androidx.activity.compose.BackHandler
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
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
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
import com.rawr.camera.settings.model.EXTENDED_ISO_MAX
import com.rawr.camera.settings.model.EXTENDED_SHUTTER_MAX_NS
import com.rawr.camera.settings.model.LensExposure
import com.rawr.camera.settings.model.LensLevels
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.RawStreamChoice
import com.rawr.camera.settings.model.RawStreamFormat
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
    val camera = inventory?.find(draft.cameraId, draft.physicalCameraId)
    val others = lenses.filterNot { original != null && it.name.equals(original.name, ignoreCase = true) }
    val problems = validateLensProfile(draft, others)
    val dirty = draft != initial

    var pickCamera by remember { mutableStateOf(false) }
    var pickStream by remember { mutableStateOf(false) }
    // Index of the vendor key being edited; -1 adds a new key; null = closed.
    var editKey by remember { mutableStateOf<Int?>(null) }
    var confirmDiscard by remember { mutableStateOf(false) }
    var confirmDelete by remember { mutableStateOf(false) }

    BackHandler(enabled = dirty) { confirmDiscard = true }

    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("LensEditor")) {
        SettingsGroup(title = "Lens") {
            LensNameField(draft.name) { draft = draft.copy(name = it) }
            SettingDivider()
            SettingsSwitchRow(
                title = "Enabled",
                checked = draft.enabled,
                supportingText = "Shown on the capture screen"
            ) { draft = draft.copy(enabled = it) }
        }
        SettingsGroup(title = "Camera") {
            SettingsRow(
                title = "Camera",
                value = cameraLabel(draft.cameraId, draft.physicalCameraId),
                supportingText = camera?.let(::cameraDetail),
                testTag = SettingsTestTags.row("LensEditor", "camera"),
                onClick = { pickCamera = true }
            )
            SettingDivider()
            SettingsRow(
                title = "RAW stream",
                value = draft.stream.label,
                supportingText = "Falls back to the largest usable stream if this one isn't offered",
                enabled = draft.cameraId.isNotBlank(),
                onClick = { pickStream = true }
            )
        }
        LevelsGroup(
            title = "Black & white levels",
            description = "Dynamic uses the camera's own levels. Static overrides them on every frame, " +
                "e.g. for vendor sensor modes with a different bit depth.",
            levels = draft.levels,
            camera = camera
        ) { draft = draft.copy(levels = it) }
        ExtendedExposureGroup(draft.exposure) { draft = draft.copy(exposure = it) }
        VendorKeysGroup(
            title = "Vendor keys",
            description = "Session keys are set when each capture session starts; request keys on every request " +
                "(preview, stills, multiframe and video). If an enabled key can't be applied the lens fails to open.",
            keys = draft.vendorKeys,
            onChange = { draft = draft.copy(vendorKeys = it) },
            onEdit = { editKey = it ?: -1 }
        )
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
            OutlinedButton(onClick = { dispatch.invoke(NavigateBack) }) { Text("Cancel") }
            Button(
                enabled = problems.isEmpty() && (dirty || original == null),
                modifier = Modifier.testTag(SettingsTestTags.row("LensEditor", "save")),
                onClick = {
                    dispatch.invoke(SetLensProfiles(lenses.upsertLens(original?.name, draft)))
                    dispatch.invoke(NavigateBack)
                }
            ) { Text("Save") }
        }
    }

    if (pickCamera) {
        CameraPickerDialog(
            inventory = inventory,
            selectedCameraId = draft.cameraId,
            selectedPhysicalId = draft.physicalCameraId,
            onDismiss = { pickCamera = false },
            onPick = { picked ->
                draft = draft.copy(
                    cameraId = picked.openId,
                    physicalCameraId = picked.physicalId,
                    stream = streamForCamera(draft.stream, picked)
                )
                pickCamera = false
            }
        )
    }
    if (pickStream) {
        StreamPickerDialog(
            camera = camera,
            selected = draft.stream,
            onDismiss = { pickStream = false },
            onPick = {
                draft = draft.copy(stream = it)
                pickStream = false
            }
        )
    }
    editKey?.let { index ->
        val keys = draft.vendorKeys
        val existing = keys.getOrNull(index)
        VendorKeyDialog(
            initial = existing,
            cameraId = draft.cameraId,
            physicalCameraId = draft.physicalCameraId,
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
            title = { Text("Discard changes?") },
            text = { Text("Your edits to this lens are not saved.") },
            confirmButton = {
                TextButton(onClick = {
                    confirmDiscard = false
                    dispatch.invoke(NavigateBack)
                }) { Text("Discard") }
            },
            dismissButton = { TextButton(onClick = { confirmDiscard = false }) { Text("Keep editing") } }
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
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = SettingsRowHorizontalPadding, vertical = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    listOf("R", "Gr", "Gb", "B").forEachIndexed { channel, label ->
                        LevelField(label, levels.blackRggb[channel], Modifier.weight(1f)) { v ->
                            onChange(levels.copy(blackRggb = levels.blackRggb.mapIndexed { i, b -> if (i == channel) v else b }))
                        }
                    }
                }
            } else {
                LevelField("Black", levels.blackRggb.first(), Modifier.fillMaxWidth().padding(horizontal = SettingsRowHorizontalPadding, vertical = 4.dp)) { v ->
                    onChange(levels.copy(blackRggb = List(4) { v }))
                }
            }
            LevelField("White", levels.white, Modifier.fillMaxWidth().padding(horizontal = SettingsRowHorizontalPadding, vertical = 4.dp)) { v ->
                onChange(levels.copy(white = v))
            }
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
private fun LevelField(label: String, value: Float, modifier: Modifier, onValue: (Float) -> Unit) {
    var text by remember { mutableStateOf(formatLevel(value)) }
    LaunchedEffect(value) { if (text.toFloatOrNull() != value) text = formatLevel(value) }
    OutlinedTextField(
        value = text,
        onValueChange = { t ->
            text = t
            t.toFloatOrNull()?.takeIf { it.isFinite() }?.let(onValue)
        },
        label = { Text(label) },
        singleLine = true,
        isError = text.toFloatOrNull() == null,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
        modifier = modifier
    )
}

private fun formatLevel(value: Float): String =
    if (value == Math.round(value).toFloat()) Math.round(value).toString() else value.toString()

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
            LevelField(
                "Highest ISO",
                exposure.isoMax.toFloat(),
                Modifier.fillMaxWidth().padding(horizontal = SettingsRowHorizontalPadding, vertical = 4.dp)
            ) { v -> onChange(exposure.copy(isoMax = v.toInt().coerceIn(0, EXTENDED_ISO_MAX))) }
            LevelField(
                "Longest shutter (s, up to 25)",
                exposure.shutterMaxNs / 1e9f,
                Modifier.fillMaxWidth().padding(horizontal = SettingsRowHorizontalPadding, vertical = 4.dp)
            ) { v ->
                onChange(exposure.copy(shutterMaxNs = (v.toDouble() * 1e9).toLong().coerceIn(0L, EXTENDED_SHUTTER_MAX_NS)))
            }
        }
    }
}

@Composable
private fun VendorKeysGroup(
    title: String,
    description: String?,
    keys: List<VendorKey>,
    onChange: (List<VendorKey>) -> Unit,
    onEdit: (Int?) -> Unit
) {
    SettingsGroup(title = title, description = description) {
        keys.forEachIndexed { index, key ->
            if (index > 0) SettingDivider()
            SettingsRow(
                title = key.tag,
                value = "${key.scope.label} · ${key.type.label} · ${formatVendorKeyValues(key.values, key.type)}",
                onClick = { onEdit(index) },
                trailing = {
                    CompactSwitch(
                        checked = key.enabled,
                        onCheckedChange = { on -> onChange(keys.mapIndexed { i, k -> if (i == index) k.copy(enabled = on) else k }) }
                    )
                }
            )
        }
        if (keys.isNotEmpty()) SettingDivider()
        SettingsRow(title = "Add key", leadingIcon = Icons.Rounded.Add, onClick = { onEdit(null) })
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
