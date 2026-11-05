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

// Lens editor: levels, sensor layout and exposure range.

@Composable
internal fun LevelsGroup(
    title: String,
    description: String?,
    levels: LensLevels,
    camera: ProbedCamera?,
    onChange: (LensLevels) -> Unit
) {
    SettingsGroup(title = title, description = description) { LevelsRows(levels, camera, "levels", onChange) }
}

/** Dynamic/static choice and the static values; shared by the lens, photo-stream and viewfinder levels. */
@Composable
internal fun LevelsRows(levels: LensLevels, camera: ProbedCamera?, tagPrefix: String, onChange: (LensLevels) -> Unit) {
    val cameraLevels = camera?.let { c ->
        c.blackLevels.takeIf { it.size == 4 && c.whiteLevel > 0 }?.let { black ->
            LensLevels(true, black.map { it.toFloat() }, c.whiteLevel.toFloat())
        }
    }
    Column(Modifier.fillMaxWidth().testTag(lensEditorTag(tagPrefix))) {
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
internal fun LevelInput(value: Float, modifier: Modifier = Modifier, width: Dp = 104.dp, placeholder: String = "0", onValue: (Float) -> Unit) {
    var text by remember { mutableStateOf(formatLevel(value)) }
    LaunchedEffect(value) { if (text.toFloatOrNull() != value) text = formatLevel(value) }
    CompactInput(text, { t ->
        text = t
        t.toFloatOrNull()?.takeIf { it.isFinite() }?.let(onValue)
    }, modifier, width = width, placeholder = placeholder, isError = text.toFloatOrNull() == null, keyboard = KeyboardType.Decimal)
}

@Composable
internal fun LevelField(label: String, value: Float, hint: String? = null, onValue: (Float) -> Unit) {
    SettingsRow(title = label, supportingText = hint, trailing = { LevelInput(value, onValue = onValue) })
}

internal fun formatLevel(value: Float): String =
    if (value == Math.round(value).toFloat()) Math.round(value).toString() else value.toString()

@Composable
internal fun SensorLayoutGroup(layout: LensSensorLayout, onChange: (LensSensorLayout) -> Unit) {
    SettingsGroup(
        title = "Sensor layout",
        description = "For RAW that comes out as coloured blocks or with wrong colours: some sensor modes " +
            "(often in-sensor crops) deliver the quad or 4x4 mosaic while reporting Bayer. Quad and 4x4 are " +
            "remosaiced to Bayer at full resolution. Colour order fixes swapped red/blue or magenta-green " +
            "casts; for quad and 4x4 it is the order of the blocks."
    ) {
        CompactChoiceRow(SENSOR_LAYOUT_BLOCKS, layout.block, ::sensorLayoutBlockLabel) { onChange(layout.copy(block = it)) }
        SettingDivider()
        DropdownChoiceRow(
            title = "Colour order",
            selected = layout.colourOrder,
            values = listOf<String?>(null) + COLOUR_ORDERS,
            label = { it ?: "Camera's" },
            onSelect = { onChange(layout.copy(colourOrder = it)) }
        )
        if (layout.block > 1) {
            SettingDivider()
            DropdownChoiceRow(
                title = "Live remosaic",
                selected = layout.liveRemosaic,
                values = LIVE_REMOSAIC_QUALITIES,
                label = ::remosaicQualityLabel,
                supportingText = "Viewfinder, video and merged photos (their frames come from the live stream). " +
                    "Fast skips green interpolation: about 20% cheaper, softer colour detail.",
                onSelect = { onChange(layout.copy(liveRemosaic = it)) }
            )
            SettingDivider()
            DropdownChoiceRow(
                title = "Photo remosaic",
                selected = layout.photoRemosaic,
                values = PHOTO_REMOSAIC_QUALITIES,
                label = ::remosaicQualityLabel,
                supportingText = "Single photos. High searches wider for clean edges; adds ~0.15 s per photo.",
                onSelect = { onChange(layout.copy(photoRemosaic = it)) }
            )
        }
    }
}

internal fun remosaicQualityLabel(quality: String): String = when (quality) {
    "fast" -> "Fast"
    "high" -> "High"
    else -> "Standard"
}

internal fun sensorLayoutBlockLabel(block: Int): String = when (block) {
    2 -> "Quad 2×2"
    4 -> "4×4"
    else -> "Bayer"
}

@Composable
internal fun ExtendedExposureGroup(exposure: LensExposure, onChange: (LensExposure) -> Unit) {
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
