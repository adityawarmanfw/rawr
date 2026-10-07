package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import com.rawr.camera.model.CaptureControlLayout
import com.rawr.camera.model.ControlSurfaceStyle
import com.rawr.camera.model.GridMode
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun DisplayControlsSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("DisplayControls")) {
        SettingsGroup {
            GridModeSettingsRow(state.values.gridMode) { dispatch.invoke(SetGridMode(it)) }
            SettingDivider()
            ViewfinderResolutionRow(state.values.viewfinderDivisor) { dispatch.invoke(SetViewfinderDivisor(it)) }
            SettingDivider()
            SettingsRow(
                "Monitoring",
                "Focus peaking behavior",
                testTag = SettingsTestTags.row("DisplayControls", "monitoring"),
                onClick = {
                    dispatch.invoke(OpenSection(SettingsSection.Monitoring))
                }
            )
            SettingDivider()
            SettingsRow(
                "Control Style",
                "Capture control surface appearance",
                testTag = SettingsTestTags.row("DisplayControls", "control_style"),
                onClick = {
                    dispatch.invoke(OpenSection(SettingsSection.ControlStyle))
                }
            )
        }
    }
}

@Composable
internal fun MonitoringSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val peaking =
        state.capabilities.peakingSensitivityChoices.firstOrNull {
            it.id == state.values.peakingSensitivityId
        }
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("Monitoring")) {
        SettingsGroup(description = "Monitoring overlays are activated from the Capture Screen.") {
            SettingsRow(
                "Focus Peaking Sensitivity",
                peaking?.label ?: "Unavailable",
                testTag = SettingsTestTags.row("Monitoring", "peaking_sensitivity"),
                onClick = {
                    dispatch.invoke(OpenSelector(ChoiceSelectorKind.PeakingSensitivity))
                }
            )
        }
    }
}

@Composable
internal fun ControlStyleSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val haptics = LocalSettingsHaptics.current
    SettingsPageContainer {
        SettingsGroup(
            title = "Capture Layout",
            description = "Classic keeps SS/ISO/EV sliders in the viewfinder. Compact moves WB/SS/ISO/EV to scrubbable buttons by the shutter, with multiframe and film sim pills."
        ) {
            SettingsSelectionRow(
                title = "Classic",
                selected = state.values.captureControlLayout == CaptureControlLayout.Classic,
                onClick = {
                    haptics.detent()
                    dispatch.invoke(SetCaptureControlLayout(CaptureControlLayout.Classic))
                }
            )
            SettingDivider()
            SettingsSelectionRow(
                title = "Compact",
                selected = state.values.captureControlLayout == CaptureControlLayout.Compact,
                onClick = {
                    haptics.detent()
                    dispatch.invoke(SetCaptureControlLayout(CaptureControlLayout.Compact))
                }
            )
        }
        SettingsGroup(
            description = "Changes only the visual material used by Capture Screen controls. Layout and interaction stay identical."
        ) {
            SettingsSelectionRow(
                title = "Frosted",
                selected = state.values.controlSurfaceStyle == ControlSurfaceStyle.Frosted,
                onClick = {
                    haptics.detent()
                    dispatch.invoke(SetControlSurfaceStyle(ControlSurfaceStyle.Frosted))
                }
            )
            SettingDivider()
            SettingsSelectionRow(
                title = "Basic",
                selected = state.values.controlSurfaceStyle == ControlSurfaceStyle.Basic,
                onClick = {
                    haptics.detent()
                    dispatch.invoke(SetControlSurfaceStyle(ControlSurfaceStyle.Basic))
                }
            )
        }
    }
}

@Composable
private fun GridModeSettingsRow(selected: GridMode, onSelect: (GridMode) -> Unit) {
    val expanded = androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf(false) }
    val labels =
        mapOf(
            GridMode.Off to "Off",
            GridMode.Thirds to "Rule of thirds",
            GridMode.FourByFour to "4 × 4",
            GridMode.Cross to "Crosshair"
        )
    Box {
        SettingsRow("Grid", labels.getValue(selected), onClick = { expanded.value = true })
        DropdownMenu(expanded = expanded.value, onDismissRequest = { expanded.value = false }) {
            GridMode.entries.forEach { mode ->
                DropdownMenuItem(
                    text = { Text(labels.getValue(mode)) },
                    onClick = {
                        onSelect(mode)
                        expanded.value = false
                    }
                )
            }
        }
    }
}

@Composable
private fun ViewfinderResolutionRow(divisor: Int, onSelect: (Int) -> Unit) {
    val expanded = remember { mutableStateOf(false) }
    // Divisor of the preview size the viewfinder look renders at. Film sim
    // never renders above High, so Full and High look the same there.
    val labels = mapOf(1 to "Full", 2 to "High", 3 to "Balanced", 4 to "Fast")
    Box {
        SettingsRow(
            "Viewfinder Resolution",
            labels.getValue(divisor.coerceIn(1, 4)),
            supportingText = "Lower resolution reduces GPU load and heat. Photos and video are not affected.",
            testTag = SettingsTestTags.row("DisplayControls", "viewfinder_resolution"),
            onClick = { expanded.value = true }
        )
        DropdownMenu(expanded = expanded.value, onDismissRequest = { expanded.value = false }) {
            labels.forEach { (value, label) ->
                DropdownMenuItem(
                    text = { Text(label) },
                    onClick = {
                        onSelect(value)
                        expanded.value = false
                    }
                )
            }
        }
    }
}
