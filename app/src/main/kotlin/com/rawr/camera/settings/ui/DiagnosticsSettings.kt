package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.rawr.camera.integration.GpuPlatform
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun InternalLoggingSettings(
    state: SettingsUiState,
    dispatch: SettingsDispatch,
    onDumpInternalTrace: () -> Unit,
    onClearInternalTrace: () -> Unit,
    onExportDiagnosticsBundle: () -> Unit
) {
    SettingsPageContainer {
        SettingsGroup(
            description = "Bounded in-memory runtime instrumentation. Oldest rows are overwritten when the selected window is full."
        ) {
            SettingsSwitchRow(
                title = "Internal Log Capture",
                checked = state.values.internalTraceCaptureEnabled
            ) { dispatch.invoke(SetInternalTraceCaptureEnabled(it)) }
            SettingDivider()
            SettingsRow(title = "Retained Rows", value = state.values.internalTraceRetainedRows.toString())
            CompactChoiceRow(
                values = listOf(4096, 8192, 16384, 32768),
                selected = state.values.internalTraceRetainedRows,
                label = { it.toString() },
                onSelected = { dispatch.invoke(SetInternalTraceRetainedRows(it)) }
            )
        }
        SettingsGroup(title = "Log Actions") {
            androidx.compose.foundation.layout.Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
                horizontalArrangement =
                    androidx.compose.foundation.layout.Arrangement
                        .spacedBy(8.dp)
            ) {
                androidx.compose.material3.OutlinedButton(
                    onClick = onClearInternalTrace,
                    modifier = Modifier.weight(1f).heightIn(min = 40.dp)
                ) {
                    Text("Clear Log")
                }
                androidx.compose.material3.FilledTonalButton(
                    onClick = onDumpInternalTrace,
                    modifier = Modifier.weight(1f).heightIn(min = 40.dp)
                ) {
                    Text("Export Log")
                }
            }
        }
        SettingsGroup(
            title = "Continuous File Audit",
            description = "Subordinate file writers under Internal Logging. Flags stay independent: continuous audit files are written even when ring capture is off."
        ) {
            SettingsSwitchRow(
                title = "Persistent Diagnostics",
                checked = state.values.persistentDiagnosticsEnabled
            ) { dispatch.invoke(SetPersistentDiagnosticsEnabled(it)) }
            SettingDivider()
            SettingsRow(
                title = "Export Diagnostics Bundle",
                value = "Zip",
                onClick = onExportDiagnosticsBundle
            )
        }
    }
}

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
    onRequestSaveNotificationPermission: () -> Unit
) {
    SettingsPageContainer {
        SettingsGroup(description = "Developer diagnostics are expensive and should stay off for normal photography.") {
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
            SettingDivider()
            SettingsRow(
                title = "Internal Logging",
                value = if (state.values.internalTraceCaptureEnabled) "Capturing" else "Off",
                onClick = { dispatch.invoke(OpenSection(SettingsSection.InternalLogging)) }
            )
        }
    }
}
