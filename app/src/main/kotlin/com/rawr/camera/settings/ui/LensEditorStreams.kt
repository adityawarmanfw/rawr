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

// Lens editor: the RAW, capture, companion and aux JPEG streams.

@Composable
internal fun SizeFields(
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
                    Modifier.testTag(lensEditorTag("${tagPrefix}_width")), width = 80.dp, placeholder = "W")
                Text("×", style = MaterialTheme.typography.bodyMedium)
                CompactInput(height, { t -> if (t.all { it in '0'..'9' }) { height = t; onChange(size.copy(height = number(t))) } },
                    Modifier.testTag(lensEditorTag("${tagPrefix}_height")), width = 80.dp, placeholder = "H")
            }
        }
    )
}

/** The viewfinder RAW stream and its reader. */
@Composable
internal fun RawStreamGroup(lens: LensProfile, onPickStream: () -> Unit, onChange: (LensProfile) -> Unit) {
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
            CaptureNumberField("Stream use case", raw.useCase, Modifier.testTag(lensEditorTag("use_case_raw")),
                "Java Camera2 only. 0 = not set; standard values are 1 to 6, anything else is the vendor's") {
                onChange(lens.raw { copy(useCase = it) })
            }
            UsageField("Buffer usage", raw.reader.usage, Modifier.testTag(lensEditorTag("reader_usage")),
                "Empty: the app chooses (GPU first, CPU if the camera refuses). Or gpuDataBuffer, none, or a 0x hex value") { onChange(lens.raw { copy(reader = reader.copy(usage = it)) }) }
            CaptureNumberField("Buffers", raw.reader.maxImages, Modifier.testTag(lensEditorTag("reader_images")),
                "RAW frames the reader holds, 6 to 16. 0 = 6") { onChange(lens.raw { copy(reader = reader.copy(maxImages = it)) }) }
        }
        SettingsSwitchRow(
            title = "Test the buffer usage first",
            checked = raw.reader.probe,
            supportingText = "With a buffer usage set: ask the allocator before using it, and use CPU buffers if it refuses",
            enabled = raw.reader.usage != null,
            testTag = lensEditorTag("reader_probe")
        ) { onChange(lens.raw { copy(reader = reader.copy(probe = it)) }) }
        SettingDivider()
        SettingsSwitchRow(
            title = "Fall back if no frames arrive",
            checked = raw.reader.demote,
            supportingText = "With a buffer usage set: when no frame arrives, retry with the app's own choice",
            enabled = raw.reader.usage != null,
            testTag = lensEditorTag("reader_demote")
        ) { onChange(lens.raw { copy(reader = reader.copy(demote = it)) }) }
    }
}

/** A second RAW stream that only still requests fill (vendor full-size modes). */
@Composable
internal fun CaptureStreamGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
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
            testTag = lensEditorTag("capture_stream")
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
                CaptureNumberField("Stream use case", capture.useCase, Modifier.testTag(lensEditorTag("use_case_capture")),
                    "Java Camera2 only. 0 = not set") { update { copy(useCase = it) } }
                UsageField("Buffer usage", capture.readerUsage, Modifier.testTag(lensEditorTag("capture_reader_usage")),
                    "Empty: GPU data buffer, the one vivo full-size modes accept") { update { copy(readerUsage = it) } }
                CaptureNumberField("Most frames per photo", capture.burstMax, Modifier.testTag(lensEditorTag("capture_burst")),
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
internal fun CompanionGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
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
                testTag = lensEditorTag("companion_targeted")
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
                CaptureNumberField("Buffers", companion.maxImages, Modifier.testTag(lensEditorTag("companion_images")),
                    "Frames its reader holds, 2 to 16. 0 = 2") { onChange(lens.companion { copy(maxImages = it) }) }
                UsageField("Buffer usage", companion.usage, Modifier.testTag(lensEditorTag("companion_usage")),
                    "Empty: GPU sampling for PRIVATE, the reader's own for YUV. Or gpuDataBuffer, none, or a 0x hex value") {
                    onChange(lens.companion { copy(usage = it) })
                }
                CaptureNumberField("Stream use case", companion.useCase, Modifier.testTag(lensEditorTag("use_case_companion")),
                    "Java Camera2 only. 0 = not set") { onChange(lens.companion { copy(useCase = it) }) }
            }
        }
    }
}

@Composable
internal fun AuxJpegGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    val aux = lens.streams.auxJpeg
    val update = { change: AuxJpegSettings.() -> AuxJpegSettings ->
        aux?.let { onChange(lens.streams { copy(auxJpeg = it.change()) }) }
    }
    SettingsGroup(title = "Aux JPEG stream") {
        SettingsSwitchRow(
            title = "Add a JPEG stream",
            checked = aux != null,
            supportingText = "Configured with the session but never filled. Some vendor modes only start with one",
            testTag = lensEditorTag("aux_jpeg")
        ) { onChange(lens.streams { copy(auxJpeg = if (it) auxJpeg ?: AuxJpegSettings(LensOutputSize(4096, 3072)) else null) }) }
        if (aux != null) {
            FieldColumn {
                SizeFields("Size", aux.size, "aux_jpeg") { update { copy(size = it) } }
                UsageField("Buffer usage", aux.usage, Modifier.testTag(lensEditorTag("aux_jpeg_usage"))) { update { copy(usage = it) } }
                CaptureNumberField("Stream use case", aux.useCase, Modifier.testTag(lensEditorTag("use_case_aux_jpeg")),
                    "Java Camera2 only. 0 = not set") { update { copy(useCase = it) } }
            }
        }
    }
}

/** Keeps the stream when the new camera offers it; otherwise the largest of a format it supports. */
internal fun streamForCamera(current: RawStreamChoice, camera: ProbedCamera): RawStreamChoice {
    val usable = camera.rawStreams.filter { it.supported }
    if (current.isLargest && usable.any { it.format == current.format }) return current
    if (usable.any { it.format == current.format && it.width == current.width && it.height == current.height }) return current
    val format = usable.firstOrNull { it.format == RawStreamFormat.Raw16 }?.format ?: usable.firstOrNull()?.format ?: current.format
    return RawStreamChoice(format)
}
