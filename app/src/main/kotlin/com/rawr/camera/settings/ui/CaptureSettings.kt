package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
    SettingsPageContainer {
        SettingsGroup(title = "Anti-flicker") {
            CompactChoiceRow(
                AntiFlicker.entries,
                state.values.antiFlicker,
                { it.label }
            ) { dispatch.invoke(SetAntiFlicker(it)) }
        }
        SettingsGroup(
            title = "Highlight Protection",
            description = "Soft-knee width past Maximum Post-RAW Gain. Narrower protects highlights; wider keeps shadows brighter."
        ) {
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
        SettingsGroup(
            description = "Locking shutter or ISO enters full Manual instead of shutter/ISO priority, " +
                "even when the camera supports priority modes."
        ) {
            SettingsSwitchRow(
                title = "Full Manual Instead of Priority",
                checked = state.values.aePriorityDisabled
            ) { dispatch.invoke(SetAePriorityDisabled(it)) }
        }
        SettingsGroup(title = "Auto Exposure Limits") {
            SettingsRow(
                "Maximum Post-RAW Gain",
                maxPostGain,
                onClick = { dispatch.invoke(OpenSelector(ChoiceSelectorKind.MaxPostGain)) }
            )
            SettingDivider()
            SettingsRow(
                "Auto Minimum FPS",
                autoMinFps,
                onClick = { dispatch.invoke(OpenSelector(ChoiceSelectorKind.AutoMinFps)) }
            )
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
