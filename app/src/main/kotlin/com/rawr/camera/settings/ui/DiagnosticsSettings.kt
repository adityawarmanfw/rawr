package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.rawr.camera.integration.GpuPlatform
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.Download
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun ExperimentalSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("Experimental")) {
        SettingsGroup {
            ToggleSubmenuRow(
                title = "Zero Copy",
                checked = state.values.experimentalZeroCopyEnabled,
                testTag = SettingsTestTags.row("Experimental", "zero_copy"),
                onOpen = { dispatch.invoke(OpenSection(SettingsSection.ZeroCopy)) },
                onCheckedChange = { dispatch.invoke(SetExperimentalZeroCopyEnabled(it)) }
            )
            SettingDivider()
            SettingsSwitchRow(
                title = "Persistent Engine",
                checked = state.values.persistentEngineEnabled
            ) { dispatch.invoke(SetPersistentEngineEnabled(it)) }
            if (GpuPlatform.supportsCustomDriver) {
                SettingDivider()
                ToggleSubmenuRow(
                    title = "GPU Driver",
                    value = if (state.values.customGpuDriverEnabled) state.values.customGpuDriverName ?: "Custom" else "System",
                    checked = state.values.customGpuDriverEnabled,
                    testTag = SettingsTestTags.row("Experimental", "gpu_driver"),
                    onOpen = { dispatch.invoke(OpenSection(SettingsSection.GpuDriver)) },
                    onCheckedChange = { dispatch.invoke(SetCustomGpuDriverEnabled(it)) }
                )
            }
        }
    }
}

@Composable
internal fun GpuDriverSettings(state: SettingsUiState, dispatch: SettingsDispatch, onImportGpuDriver: () -> Unit) {
    SettingsPageContainer {
        SettingsGroup(
            description = "Use a custom Vulkan driver for Rawr. Changes take effect after returning to the camera."
        ) {
            SettingsSwitchRow(
                title = "Custom GPU Driver",
                checked = state.values.customGpuDriverEnabled,
                enabled = state.values.customGpuDriverName != null
            ) { dispatch.invoke(SetCustomGpuDriverEnabled(it)) }
            SettingDivider()
            SettingsRow(
                title = "Load GPU Driver",
                onClick = onImportGpuDriver
            )
        }
    }
}

@Composable
internal fun ZeroCopySettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsPageContainer {
        SettingsGroup(
            description = "Experimental RAW preview ingress. This can render corrupted RAW on unsupported camera/driver combinations."
        ) {
            SettingsSwitchRow(
                title = "Zero Copy",
                checked = state.values.experimentalZeroCopyEnabled
            ) { dispatch.invoke(SetExperimentalZeroCopyEnabled(it)) }
        }
    }
}

@Composable
internal fun DebugSettings(
    state: SettingsUiState,
    dispatch: SettingsDispatch,
    onRequestSaveNotificationPermission: () -> Unit,
    onExportDiagnosticsBundle: () -> Unit
) {
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("Debug")) {
        SettingsGroup(
            title = "Report a Problem",
            description = "Rawr keeps recent logs in the background. After a problem, export them here and attach the zip to your report."
        ) {
            SettingsRow(
                title = "Export Diagnostic Bundle",
                leadingIcon = Icons.Rounded.Download,
                testTag = SettingsTestTags.row("Debug", "export_diagnostics"),
                onClick = onExportDiagnosticsBundle
            )
        }
        SettingsGroup(
            title = "Developer",
            description = "Developer diagnostics are expensive and should stay off for normal photography."
        ) {
            SettingsSwitchRow(
                title = "Pipeline Diagnostics",
                checked = state.values.pipelineDiagnosticsEnabled
            ) { dispatch.invoke(SetPipelineDiagnosticsEnabled(it)) }
            SettingsSwitchRow(
                title = "Dump RZSL on Shutter",
                checked = state.values.persistZslRingOnShutterEnabled,
                enabled = state.values.experimentalMultiframeEnabled
            ) { enabled ->
                dispatch.invoke(SetPersistZslRingOnShutterEnabled(enabled))
                if (enabled) onRequestSaveNotificationPermission()
            }
        }
    }
}
