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
import android.os.Build
import com.rawr.camera.settings.model.CompanionSettings
import com.rawr.camera.settings.model.BufferSettings
import com.rawr.camera.settings.model.resolvedCameraApi


// Lens editor: the Capture section (resolution, high-res modes, vendor gate) and the stream overrides.

/** How a lens fills its photos; derived from the streams, not stored. */
internal enum class LensResolution(val label: String, val detail: String) {
    Normal("Normal", "A RAW size the camera lists"),
    HighResLive("High-res live", "A vendor mode the camera doesn't list, at its exact size, for the viewfinder and every photo"),
    HighResPhotos("High-res photos", "The viewfinder stays at a normal size; photos come from a second, full-size stream")
}

internal fun LensProfile.resolution(): LensResolution = when {
    streams.photo != null -> LensResolution.HighResPhotos
    streams.raw.choice.highResLive -> LensResolution.HighResLive
    else -> LensResolution.Normal
}

/**
 * The lens switched to [next], using what each mode last had: [normal] for the Normal and viewfinder stream,
 * [live] for High-res live, [photoStream] for the High-res photo stream. Normal drops the vendor gate; a high-res
 * mode on [vivo] gets vivo's gate when none is set.
 */
internal fun LensProfile.withResolution(
    next: LensResolution,
    normal: RawStreamChoice,
    live: RawStreamChoice,
    photoStream: PhotoStreamSettings,
    vivo: Boolean
): LensProfile {
    val switched = when (next) {
        LensResolution.Normal -> streams { copy(raw = raw.copy(choice = normal.copy(highResLive = false)), photo = null) }
        LensResolution.HighResLive -> streams { copy(raw = raw.copy(choice = live.copy(highResLive = true)), photo = null) }
        LensResolution.HighResPhotos -> streams { copy(raw = raw.copy(choice = normal.copy(highResLive = false)), photo = photoStream) }
    }
    return when {
        next == LensResolution.Normal -> switched.session { copy(gate = "", gateHold = null) }
        vivo && session.gate.isEmpty() -> switched.session { copy(gate = "vivoFullSize") }
        else -> switched
    }
}

/** vivo needs its vendor gate for every stream above the normal binned size. */
internal val isVivoDevice: Boolean get() = Build.MANUFACTURER.equals("vivo", ignoreCase = true)

internal fun megapixels(width: Int, height: Int): String? =
    if (width > 0 && height > 0) String.format(Locale.ROOT, "%.1f MP", width.toDouble() * height / 1_000_000) else null

/** A short explanation or warning line inside a group. */
@Composable
internal fun EditorNote(text: String, warning: Boolean = false, testTag: String? = null) {
    Text(
        text,
        style = MaterialTheme.typography.bodySmall,
        color = if (warning) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier
            .padding(horizontal = SettingsRowHorizontalPadding, vertical = 8.dp)
            .then(if (testTag != null) Modifier.testTag(testTag) else Modifier)
    )
}

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

/**
 * Capture: one resolution choice that brings what each mode needs. A setting that is in use is never hidden,
 * even when the chosen mode doesn't need it, so an imported profile shows everything it sets.
 */
@Composable
internal fun CaptureGroup(lens: LensProfile, camera: ProbedCamera?, onPickStream: () -> Unit, onChange: (LensProfile) -> Unit) {
    val resolution = lens.resolution()
    val raw = lens.streams.raw
    val session = lens.session
    // What each mode had, so switching back and forth keeps it.
    var normalChoice by remember { mutableStateOf(if (raw.choice.highResLive) RawStreamChoice(raw.choice.format) else raw.choice) }
    var liveChoice by remember { mutableStateOf(if (raw.choice.highResLive) raw.choice else RawStreamChoice(raw.choice.format, highResLive = true)) }
    var photoStream by remember { mutableStateOf(lens.streams.photo ?: PhotoStreamSettings(RawStreamChoice())) }

    SettingsGroup(title = "Capture") {
        DropdownChoiceRow(
            title = "Resolution",
            selected = resolution,
            values = LensResolution.entries,
            label = { it.label },
            supportingText = resolution.detail,
            optionText = { it.detail },
            onSelect = { next ->
                if (next == resolution) return@DropdownChoiceRow
                when (resolution) {
                    LensResolution.Normal -> normalChoice = raw.choice
                    LensResolution.HighResLive -> liveChoice = raw.choice
                    LensResolution.HighResPhotos -> { photoStream = lens.streams.photo ?: photoStream; normalChoice = raw.choice }
                }
                onChange(lens.withResolution(next, normalChoice, liveChoice, photoStream, isVivoDevice))
            }
        )
        SettingDivider()
        when (resolution) {
            LensResolution.Normal -> SettingsRow(
                title = "RAW stream",
                value = raw.choice.label,
                supportingText = "Falls back to the largest usable stream if this one isn't offered",
                enabled = lens.camera.id.isNotBlank(),
                testTag = lensEditorTag("raw_stream"),
                onClick = onPickStream
            )
            LensResolution.HighResLive -> {
                DropdownChoiceRow(
                    title = "Format",
                    selected = raw.choice.format,
                    values = RawStreamFormat.entries,
                    label = { it.label },
                    onSelect = { onChange(lens.raw { copy(choice = choice.copy(format = it)) }) }
                )
                SizeFields(
                    listOfNotNull("Size", megapixels(raw.choice.width, raw.choice.height)).joinToString(" · "),
                    LensOutputSize(raw.choice.width, raw.choice.height), "live",
                    "The vendor mode's exact size; the camera doesn't list it"
                ) { onChange(lens.raw { copy(choice = choice.copy(width = it.width, height = it.height)) }) }
                if (raw.choice.isLargest) EditorNote("Enter the mode's width and height", warning = true)
            }
            LensResolution.HighResPhotos -> {
                val capture = lens.streams.photo!!
                val update = { change: PhotoStreamSettings.() -> PhotoStreamSettings ->
                    onChange(lens.streams { copy(photo = capture.change()) })
                }
                SettingsRow(
                    title = "Viewfinder stream",
                    value = raw.choice.label,
                    supportingText = "The viewfinder, video and merged photos",
                    enabled = lens.camera.id.isNotBlank(),
                    testTag = lensEditorTag("raw_stream"),
                    onClick = onPickStream
                )
                SettingDivider()
                DropdownChoiceRow(
                    title = "Photo stream format",
                    selected = capture.choice.format,
                    values = RawStreamFormat.entries,
                    label = { it.label },
                    onSelect = { update { copy(choice = choice.copy(format = it)) } }
                )
                SizeFields(
                    listOfNotNull("Photo stream size", megapixels(capture.choice.width, capture.choice.height)).joinToString(" · "),
                    LensOutputSize(capture.choice.width, capture.choice.height), "capture",
                    "The full-size mode's dimensions, including sizes the camera doesn't list"
                ) { update { copy(choice = choice.copy(width = it.width, height = it.height)) } }
                SettingsSwitchRow(
                    title = "Include viewfinder in photo requests",
                    checked = capture.includePreview,
                    supportingText = "Also send each full-size photo request to the viewfinder RAW stream. " +
                        "Required by some vivo modes; keep off for OPPO full-size modes.",
                    testTag = lensEditorTag("photo_include_preview")
                ) { update { copy(includePreview = it) } }
                CaptureNumberField("Merged photo frame limit", capture.mergeFrameLimit, Modifier.testTag(lensEditorTag("merge_frame_limit")),
                    "A merged (multiframe) photo on this lens takes at most this many full-size frames; Multiframe → " +
                        "Maximum Frames sets how many you want. 0 = no lens limit · 1 = single photos only") {
                    update { copy(mergeFrameLimit = it) }
                }
                if (capture.mergeFrameLimit != 1) {
                    DropdownChoiceRow(
                        title = "Merged frames are requested",
                        selected = capture.mergeFramesOneByOne,
                        values = listOf(false, true),
                        label = { if (it) "One by one" else "Together" },
                        supportingText = if (capture.mergeFramesOneByOne) "Each frame after the previous one arrives: slower, " +
                            "for cameras that fail when several full-size frames are queued"
                        else "All frames in one go, the camera sets the timing: fastest",
                        onSelect = { update { copy(mergeFramesOneByOne = it) } }
                    )
                }
                SettingDivider()
                DropdownChoiceRow(
                    title = "Photo levels",
                    selected = capture.levels != null,
                    values = listOf(false, true),
                    label = { if (it) "Own" else "Same as lens" },
                    supportingText = "Full-size modes can have another black and white level than the viewfinder",
                    onSelect = { own -> update { copy(levels = if (own) levels ?: lens.image.levels else null) } }
                )
                capture.levels?.let { levels -> LevelsRows(levels, camera, "photo_levels") { update { copy(levels = it) } } }
            }
        }
        if (resolution != LensResolution.Normal || session.gate.isNotEmpty()) {
            SettingDivider()
            DropdownChoiceRow(
                title = "Vendor gate",
                selected = session.gate,
                values = (listOf("", "vivoFullSize", "vivoFullSizeVcf") + session.gate).distinct(),
                label = { when (it) { "" -> "None"; "vivoFullSize" -> "vivo full-size"; "vivoFullSizeVcf" -> "vivo full-size + VCF bypass"; else -> it } },
                supportingText = "Vendor state held while the camera opens; vivo needs it for every high-res stream",
                onSelect = { onChange(lens.session { copy(gate = it, gateHold = if (it.isEmpty()) null else gateHold) }) }
            )
            if (isVivoDevice && resolution != LensResolution.Normal && session.gate.isEmpty()) {
                EditorNote("vivo opens high-res streams only with a vendor gate", warning = true, testTag = lensEditorTag("gate_warning"))
            }
            if (session.gate.isNotEmpty()) {
                SettingDivider()
                DropdownChoiceRow(
                    title = "Hold the gate",
                    selected = session.gateHold.orEmpty(),
                    values = listOf("") + GATE_HOLDS,
                    label = {
                        when (it) {
                            "" -> "Automatic"
                            "configured" -> "Until the session is configured"
                            "closed" -> "Until the camera closes"
                            else -> it
                        }
                    },
                    supportingText = "Automatic: until the camera closes with high-res photos, otherwise until configured",
                    onSelect = { onChange(lens.session { copy(gateHold = it.ifEmpty { null }) }) }
                )
            }
        }
        if (resolution == LensResolution.HighResPhotos) {
            EditorNote("The screen freezes from the press until the full-size photo is in.")
        }
    }
}

/** The companion: a small output beside RAW that some HALs need before they deliver RAW. */
@Composable
internal fun CompanionGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    val companion = lens.streams.companion
    SettingsGroup(
        title = "Companion stream",
        description = "A small extra output next to RAW. Some cameras deliver no RAW without one. Automatic adds it on " +
            "phones without a profile and with high-res photos, drops it after the first frame, and the start ladder " +
            "retries without it when a lens doesn't start."
    ) {
        DropdownChoiceRow(
            title = "Mode",
            selected = companion.mode,
            values = CompanionMode.entries,
            label = { it.label },
            supportingText = when (companion.mode) {
                CompanionMode.Auto -> if (lens.streams.raw.choice.highResLive) "Off automatically with a high-res live stream"
                else "Added where the app needs one, sized to the RAW aspect"
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
                label = { when (it) { true -> "Yes"; false -> "No"; null -> "Automatic" } },
                supportingText = "Automatic: an automatic companion stops after the first RAW frame with its metadata; a fixed one keeps going",
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
                CaptureNumberField("Buffer count", companion.bufferCount, Modifier.testTag(lensEditorTag("companion_buffer_count")),
                    "Buffers its reader holds, 2 to 16. 0 = 2") { onChange(lens.companion { copy(bufferCount = it) }) }
                UsageField("Buffer usage", companion.bufferUsage, Modifier.testTag(lensEditorTag("companion_buffer_usage")),
                    "Empty: GPU sampling for PRIVATE, the reader's own for YUV. Or gpuDataBuffer, none, or a 0x hex value") {
                    onChange(lens.companion { copy(bufferUsage = it) })
                }
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
                UsageField("Buffer usage", aux.bufferUsage, Modifier.testTag(lensEditorTag("aux_jpeg_buffer_usage"))) { update { copy(bufferUsage = it) } }
            }
        }
    }
}

/** RAW readers: the app picks GPU buffers first and falls back to CPU when the camera refuses them. */
@Composable
internal fun BuffersGroup(lens: LensProfile, camera: ProbedCamera?, onChange: (LensProfile) -> Unit) {
    val raw = lens.streams.raw
    val capture = lens.streams.photo
    SettingsGroup(
        title = "Buffers",
        description = "Automatic: GPU buffers first, CPU buffers when the camera refuses them or no frame arrives."
    ) {
        FieldColumn {
            UsageField("RAW buffer usage", raw.buffers.usage, Modifier.testTag(lensEditorTag("raw_buffer_usage")),
                "Empty: automatic. Or gpuDataBuffer, none, or a 0x hex value") { onChange(lens.raw { copy(buffers = buffers.copy(usage = it)) }) }
            CaptureNumberField("RAW buffer count", raw.buffers.count, Modifier.testTag(lensEditorTag("raw_buffer_count")),
                "Frames the reader holds, 2 to 16 (below 6 the preview can drop frames). 0 = 6") {
                onChange(lens.raw { copy(buffers = buffers.copy(count = it)) })
            }
        }
        if (raw.buffers.usage != null || raw.buffers.testFirst) {
            SettingsSwitchRow(
                title = "Test the buffer usage first",
                checked = raw.buffers.testFirst,
                supportingText = "Ask the allocator before using it, and use CPU buffers if it refuses",
                testTag = lensEditorTag("raw_buffer_test_first")
            ) { onChange(lens.raw { copy(buffers = buffers.copy(testFirst = it)) }) }
        }
        if (raw.buffers.usage != null || raw.buffers.fallBack) {
            SettingDivider()
            SettingsSwitchRow(
                title = "Fall back if no frames arrive",
                checked = raw.buffers.fallBack,
                supportingText = "When no frame arrives, retry with the automatic choice",
                testTag = lensEditorTag("raw_buffer_fall_back")
            ) { onChange(lens.raw { copy(buffers = buffers.copy(fallBack = it)) }) }
        }
        if (capture != null) {
            UsageField("Photo stream buffer usage", capture.bufferUsage, Modifier.testTag(lensEditorTag("photo_buffer_usage")),
                "Empty: GPU data buffer. 0x3: CPU-readable, needed for OPPO RAW10 photos; none: no usage flags") {
                onChange(lens.streams { copy(photo = capture.copy(bufferUsage = it)) })
            }
            CaptureNumberField("Photo stream buffer count", capture.bufferCount,
                Modifier.testTag(lensEditorTag("photo_buffer_count")),
                "Frames the photo reader holds, 2 to 16. 0 = 6") {
                onChange(lens.streams { copy(photo = capture.copy(bufferCount = it)) })
            }
        }
        SettingDivider()
        DropdownChoiceRow(
            title = "Viewfinder levels",
            selected = raw.levels != null,
            values = listOf(false, true),
            label = { if (it) "Own" else "Same as lens" },
            supportingText = "Rarely needed: the viewfinder stream's own black and white levels",
            onSelect = { own -> onChange(lens.raw { copy(levels = if (own) levels ?: lens.image.levels else null) }) }
        )
        raw.levels?.let { levels -> LevelsRows(levels, camera, "viewfinder_levels") { onChange(lens.raw { copy(levels = it) }) } }
    }
}

/** Stream use cases: any of them runs the lens on Java Camera2. */
@Composable
internal fun UseCasesGroup(lens: LensProfile, onChange: (LensProfile) -> Unit) {
    val streams = lens.streams
    SettingsGroup(
        title = "Stream use cases",
        description = "0 = not set. Standard values are 1 to 6, anything else is the vendor's. Any use case runs the lens on Java Camera2."
    ) {
        FieldColumn {
            CaptureNumberField("RAW stream", streams.raw.useCase, Modifier.testTag(lensEditorTag("use_case_raw"))) {
                onChange(lens.raw { copy(useCase = it) })
            }
            streams.photo?.let { capture ->
                CaptureNumberField("Photo stream", capture.useCase, Modifier.testTag(lensEditorTag("use_case_photo"))) {
                    onChange(lens.streams { copy(photo = capture.copy(useCase = it)) })
                }
            }
            if (streams.companion.mode != CompanionMode.Off || streams.companion.useCase != 0) {
                CaptureNumberField("Companion", streams.companion.useCase, Modifier.testTag(lensEditorTag("use_case_companion"))) {
                    onChange(lens.companion { copy(useCase = it) })
                }
            }
            streams.auxJpeg?.let { aux ->
                CaptureNumberField("Aux JPEG", aux.useCase, Modifier.testTag(lensEditorTag("use_case_aux_jpeg"))) {
                    onChange(lens.streams { copy(auxJpeg = aux.copy(useCase = it)) })
                }
            }
        }
    }
}

/** Everything the Advanced page can change, back at its automatic value. */
internal fun LensProfile.withAutomaticAdvanced(): LensProfile = copy(
    streams = streams.copy(
        raw = streams.raw.copy(useCase = 0, buffers = BufferSettings(), levels = null),
        photo = streams.photo?.copy(useCase = 0, bufferUsage = null, bufferCount = 0),
        companion = CompanionSettings(),
        auxJpeg = null
    ),
    session = session.copy(cameraApi = "auto", params = "cadence", stillTemplate = null, startupDelayMs = 0,
        stopViewfinderForPhotos = false, workarounds = emptyList()),
    request = request.copy(defaults = "rawr")
)

/** What the Advanced page changes from automatic, for its summary row. */
internal fun LensProfile.advancedSummary(): List<String> = buildList {
    val companion = streams.companion
    when {
        companion.mode == CompanionMode.Fixed -> add("Companion fixed")
        companion.mode == CompanionMode.Off -> add("Companion off")
        companion != CompanionSettings() -> add("Companion tuned")
    }
    if (streams.auxJpeg != null) add("Aux JPEG")
    if (streams.raw.buffers != BufferSettings() || streams.photo?.bufferUsage != null ||
        (streams.photo?.bufferCount ?: 0) != 0) add("Buffers")
    if (streams.raw.levels != null) add("Viewfinder levels")
    if (listOf(streams.raw.useCase, streams.photo?.useCase ?: 0, companion.useCase, streams.auxJpeg?.useCase ?: 0).any { it != 0 })
        add("Use cases")
    when (session.cameraApi) {
        "java" -> add("Java Camera2")
        "native" -> add("Native API")
    }
    if (session.params != "cadence" || session.stillTemplate != null || session.startupDelayMs != 0 || request.defaults != "rawr")
        add("Session")
    if (session.stopViewfinderForPhotos) add("Stream stops for photos")
    if (session.workarounds.isNotEmpty()) add(if (session.workarounds.size == 1) "1 workaround" else "${session.workarounds.size} workarounds")
}

/** Keeps the stream when the new camera offers it; otherwise the largest of a format it supports. */
internal fun streamForCamera(current: RawStreamChoice, camera: ProbedCamera): RawStreamChoice {
    if (current.highResLive) return current
    val usable = camera.rawStreams.filter { it.supported }
    if (current.isLargest && usable.any { it.format == current.format }) return current
    if (usable.any { it.format == current.format && it.width == current.width && it.height == current.height }) return current
    val format = usable.firstOrNull { it.format == RawStreamFormat.Raw16 }?.format ?: usable.firstOrNull()?.format ?: current.format
    return RawStreamChoice(format)
}
