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

// Lens editor: full-screen camera and stream pickers.

@Composable
internal fun CameraPickerDialog(
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

internal fun cameraDetail(c: ProbedCamera): String = buildList {
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

@Composable
internal fun StreamPickerDialog(
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
internal fun FullScreenPicker(title: String, onDismiss: () -> Unit, content: @Composable () -> Unit) {
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
internal fun PickerHeader(text: String) {
    Text(
        text.uppercase(),
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp)
    )
}

@Composable
internal fun PickerRow(title: String, detail: String?, selected: Boolean, enabled: Boolean = true, onClick: () -> Unit) {
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
internal fun PickerLoading() {
    Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
}

@Composable
internal fun PickerMessage(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(16.dp)
    )
}
