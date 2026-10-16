package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun CaptureSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val lenses = state.values.effectiveLensProfiles(LocalLensHardware.current.deviceDefaults)
    val enabledLenses = lenses.filter { it.enabled }
    SettingsPageContainer {
        SettingsGroup {
            SettingsRow(
                title = "Lens",
                value = enabledLenses.joinToString(" · ") { it.name }.ifEmpty { null },
                testTag = SettingsTestTags.row("Capture", "lens"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.Lens)) }
            )
            SettingDivider()
            ToggleSubmenuRow(
                title = "Multiframe",
                checked = state.values.experimentalMultiframeEnabled,
                testTag = SettingsTestTags.row("Capture", "multiframe"),
                onOpen = { dispatch.invoke(OpenSection(SettingsSection.Multiframe)) },
                onCheckedChange = { dispatch.invoke(SetExperimentalMultiframeEnabled(it)) }
            )
            SettingDivider()
            SettingsSwitchRow(
                title = "OIS",
                checked = state.values.oisEnabledPreference,
                enabled = state.capabilities.oisSupported
            ) { dispatch.invoke(SetOisPreference(it)) }
            SettingDivider()
            SettingsRow(
                title = "Exposure",
                testTag = SettingsTestTags.row("Capture", "exposure"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.Exposure)) }
            )
        }
    }
}

@Composable
internal fun OisSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsPageContainer {
        SettingsGroup {
            SettingsSwitchRow(
                title = "OIS",
                checked = state.values.oisEnabledPreference
            ) { dispatch.invoke(SetOisPreference(it)) }
        }
    }
}

@Composable
internal fun ExposureSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val maxPostGain =
        state.capabilities.maxPostGainChoices
            .firstOrNull { it.id == state.values.maxPostGainId }
            ?.label ?: "Unavailable"
    val autoMinFps =
        state.capabilities.autoMinFpsChoices
            .firstOrNull { it.id == state.values.autoMinFpsId }
            ?.label ?: "Unavailable"
    // Optimistic until probed so a supporting camera never flashes disabled.
    val hardware = LocalLensHardware.current
    val prioritySupported by produceState(true, hardware) { value = hardware.aePrioritySupported() }
    val boostLimited = state.values.maxPostGainId != "gain.100"
    SettingsPageContainer {
        SettingsGroup(
            title = "Auto Exposure",
            description =
                if (state.values.rawrAeEnabled) {
                    "rawr sets shutter and ISO itself in Auto: smooth changes, no flicker banding, " +
                        "real ISO only (extra gain is added cleanly afterwards). Video and tap-to-meter use the camera's AE."
                } else {
                    "Auto uses the camera's own exposure."
                }
        ) {
            SettingsSwitchRow(
                title = "rawr Auto Exposure",
                checked = state.values.rawrAeEnabled
            ) { dispatch.invoke(SetRawrAeEnabled(it)) }
            if (state.values.rawrAeEnabled) {
                SettingDivider()
                Text(
                    "Night Look",
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp)
                )
                CompactChoiceRow(RawrNightLook.entries, state.values.rawrAeNightLook, {
                    it.label
                }) { dispatch.invoke(SetRawrAeNightLook(it)) }
                Text(
                    when (state.values.rawrAeNightLook) {
                        RawrNightLook.Natural -> "Dim scenes stay dark, like they look"
                        RawrNightLook.Bright -> "Dim scenes a little darker than day"
                        RawrNightLook.Off -> "Every scene at day brightness"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                )
            }
        }
        SettingsGroup(
            title = "Brightness Boost",
            description = "Digital gain the camera adds in low light, applied evenly to the whole image."
        ) {
            SettingsRow(
                "Boost Limit",
                maxPostGain,
                onClick = { dispatch.invoke(OpenSelector(ChoiceSelectorKind.MaxPostGain)) }
            )
            if (boostLimited) {
                SettingDivider()
                Text(
                    "Above the Limit",
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp)
                )
                CompactChoiceRow(HighlightProtection.entries, state.values.highlightProtection, {
                    it.label
                }) { dispatch.invoke(SetHighlightProtection(it)) }
                Text(
                    state.values.highlightProtection.kneeSummary,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                )
            }
        }
        SettingsGroup(title = "Anti-flicker") {
            CompactChoiceRow(
                AntiFlicker.entries,
                state.values.antiFlicker,
                { it.label }
            ) { dispatch.invoke(SetAntiFlicker(it)) }
        }
        SettingsGroup {
            SettingsRow(
                "Auto Minimum FPS",
                autoMinFps,
                onClick = { dispatch.invoke(OpenSelector(ChoiceSelectorKind.AutoMinFps)) }
            )
        }
        SettingsGroup(
            description =
                if (prioritySupported) {
                    "Locking shutter or ISO enters full Manual instead of shutter/ISO priority, " +
                        "even when the camera supports priority modes."
                } else {
                    "This camera has no shutter/ISO priority; locking shutter or ISO always enters full Manual."
                }
        ) {
            SettingsSwitchRow(
                title = "Full Manual Instead of Priority",
                checked = state.values.aePriorityDisabled || !prioritySupported,
                enabled = prioritySupported
            ) { dispatch.invoke(SetAePriorityDisabled(it)) }
        }
    }
}

@Composable
internal fun JpegSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val tone = state.values.imageTone
    val subsampling =
        state.capabilities.jpegChromaSubsamplingChoices
            .firstOrNull { it.id == tone.jpegChromaSubsamplingId }
            ?.label ?: "4:2:0"
    SettingsPageContainer {
        SettingsGroup {
            NumericSliderRow(ImageToneSpecs.jpegQuality, tone.jpegQuality) {
                dispatch.invoke(SetNumericValue(ImageToneNumericParameter.JpegQuality, it))
            }
            SettingDivider()
            SettingsRow("Chroma Subsampling", subsampling, onClick = {
                dispatch.invoke(OpenSelector(ChoiceSelectorKind.JpegChromaSubsampling))
            })
            SettingDivider()
            SettingsSwitchRow(
                title = "Ultra HDR",
                checked = state.values.ultraHdrEnabled,
                onCheckedChange = { dispatch.invoke(SetUltraHdrEnabled(it)) }
            )
        }
    }
}

@Composable
internal fun DngSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val compression =
        state.capabilities.dngCompressionChoices
            .firstOrNull { it.id == state.values.dngCompressionId }
            ?.label ?: "Lossless"
    SettingsPageContainer {
        SettingsGroup(
            description = "Lossless writes compressed DNGs at roughly half the size. Uncompressed keeps the legacy byte layout."
        ) {
            SettingsRow("Compression", compression, onClick = {
                dispatch.invoke(OpenSelector(ChoiceSelectorKind.DngCompression))
            })
        }
    }
}
