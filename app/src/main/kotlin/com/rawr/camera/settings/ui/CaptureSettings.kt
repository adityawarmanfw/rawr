package com.rawr.camera.settings.ui

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun CaptureSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val context = LocalContext.current
    fun locationPermitted() = listOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
        .any { context.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }
    var locationGranted by remember(context) { mutableStateOf(locationPermitted()) }
    LifecycleResumeEffect(context) {
        locationGranted = locationPermitted()
        onPauseOrDispose { }
    }
    val locationPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
        locationGranted = granted.values.any { it }
        if (locationGranted) dispatch.invoke(SetLocationTagging(true))
    }
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
                value =
                    if (state.values.experimentalMultiframeEnabled) {
                        state.values.multiframeTuning.mergeAlgorithm.label
                    } else {
                        "Off"
                    },
                testTag = SettingsTestTags.row("Capture", "multiframe"),
                onOpen = { dispatch.invoke(OpenSection(SettingsSection.Multiframe)) },
                onCheckedChange = { dispatch.invoke(SetExperimentalMultiframeEnabled(it)) }
            )
            SettingDivider()
            SettingsRow(
                title = "Auto Exposure",
                value = if (state.values.rawrAe.enabled) "Rawr" else "Device",
                testTag = SettingsTestTags.row("Capture", "exposure"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.Exposure)) }
            )
            SettingDivider()
            SettingsSwitchRow(
                title = "OIS",
                checked = state.values.oisEnabledPreference,
                enabled = state.capabilities.oisSupported
            ) { dispatch.invoke(SetOisPreference(it)) }
            SettingDivider()
            SettingsSwitchRow(
                title = "Location Tagging",
                checked = state.values.locationTagging && locationGranted,
                testTag = SettingsTestTags.row("Capture", "location_tagging")
            ) { enabled ->
                locationGranted = locationPermitted()
                if (!enabled || locationGranted) {
                    dispatch.invoke(SetLocationTagging(enabled))
                } else {
                    locationPermission.launch(arrayOf(Manifest.permission.ACCESS_COARSE_LOCATION, Manifest.permission.ACCESS_FINE_LOCATION))
                }
            }
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
    // Optimistic until probed so a supporting camera never flashes disabled.
    val hardware = LocalLensHardware.current
    val prioritySupported by produceState(true, hardware) { value = hardware.aePrioritySupported() }
    val boostLimited = state.values.maxPostGainId != "gain.100"
    SettingsPageContainer {
        val ae = state.values.rawrAe
        val setAe: (RawrAeSettings) -> Unit = { dispatch.invoke(SetRawrAeSettings(it)) }
        SettingsGroup(
            title = "Auto Exposure",
            description =
                if (ae.enabled) {
                    "Rawr picks shutter and ISO in Auto, shutter priority and ISO priority, for photo and video."
                } else {
                    "Auto uses the camera's own exposure."
                }
        ) {
            SegmentedOptionRow(
                options = listOf("Device AE", "Rawr AE"),
                selected = if (ae.enabled) 1 else 0,
                onSelect = { setAe(ae.copy(enabled = it == 1)) }
            )
            if (ae.enabled) {
                RawrAeChoice(
                    "Metering", RawrMetering.entries, ae.metering, { it.label },
                    "Long-press the viewfinder to meter a spot. Matrix also leans toward the focus area",
                    {
                        when (it) {
                            RawrMetering.Matrix ->
                                "Whole frame, centre counts more. Darkens a little to save highlights; lets windows and very bright skies clip"
                            RawrMetering.CenterWeighted ->
                                "Average weighted to the centre. Bright areas pull it darker; no highlight protection"
                            RawrMetering.Average ->
                                "Whole-frame average. Bright areas pull it darker; no highlight protection"
                            RawrMetering.Highlight -> "Keeps the brightest areas just below white; the rest may go dark"
                        }
                    }
                ) { setAe(ae.copy(metering = it)) }
                RawrAeChoice(
                    "Night Look", RawrNightLook.entries, ae.nightLook, { it.label }, null,
                    {
                        when (it) {
                            RawrNightLook.Natural -> "Dim scenes stay dark, like they look"
                            RawrNightLook.Bright -> "Dim scenes only slightly darker than day"
                            RawrNightLook.Off -> "Every scene at day brightness"
                        }
                    }
                ) { setAe(ae.copy(nightLook = it)) }
                RawrAeChoice(
                    "Slowest Shutter",
                    RawrMaxShutter.entries,
                    ae.maxShutter,
                    { it.label },
                    "In dim light the shutter never goes slower than this; ISO makes up the rest. " +
                        "Auto is 1/15 s for photo and the frame time for video"
                ) { setAe(ae.copy(maxShutter = it)) }
                RawrAeChoice(
                    "Shutter / ISO Balance", RawrBalance.entries, ae.balance, { it.label },
                    "Shifts the shutter by whole stops and ISO makes up the difference, so brightness stays the same. " +
                        "No effect in bright light",
                    {
                        when (it) {
                            RawrBalance.LowIso -> "Shutter 1 stop slower, half the ISO: cleaner, more blur"
                            RawrBalance.Balanced -> "Shutter and ISO as the light needs"
                            RawrBalance.Faster -> "Shutter 1 stop faster, double the ISO"
                            RawrBalance.Fastest -> "Shutter 2 stops faster, four times the ISO"
                        }
                    }
                ) { setAe(ae.copy(balance = it)) }
                RawrAeChoice(
                    "Digital Gain",
                    RawrDigitalGain.entries,
                    ae.digitalGain,
                    { it.label },
                    "Brightening past the sensor's highest real ISO, applied after RAW; DNGs carry it as BaselineExposure. " +
                        "Maximum has no limit and gets noisy in the dark"
                ) { setAe(ae.copy(digitalGain = it)) }
                RawrAeChoice("Adaptation Speed", RawrAeSpeed.entries, ae.speed, { it.label }, null) {
                    setAe(ae.copy(speed = it))
                }
            }
            if (!ae.enabled) {
                // The camera's own AE ceiling; rawr AE uses Longest Shutter instead.
                SettingDivider()
                SelectorDropdownRow(state, ChoiceSelectorKind.AutoMinFps, dispatch)
            }
            SettingDivider()
            DropdownChoiceRow(
                title = "Anti-flicker",
                selected = state.values.antiFlicker,
                values = AntiFlicker.entries,
                label = { it.label },
                supportingText = "Keeps shutter speeds in step with mains lighting so it doesn't band. " +
                    "Applies to both auto exposures; no effect in Manual."
            ) { dispatch.invoke(SetAntiFlicker(it)) }
        }
        // Device AE only: rawr AE has its own Digital Gain and runs S/I itself.
        if (!ae.enabled) {
            SettingsGroup(
                title = "Brightness Boost",
                description = "Digital gain the camera adds in low light, applied evenly to the whole image."
            ) {
                SelectorDropdownRow(state, ChoiceSelectorKind.MaxPostGain, dispatch)
                if (boostLimited) {
                    SettingDivider()
                    DropdownChoiceRow(
                        title = "Above the Limit",
                        selected = state.values.highlightProtection,
                        values = HighlightProtection.entries,
                        label = { it.label },
                        optionText = { it.kneeSummary }
                    ) { dispatch.invoke(SetHighlightProtection(it)) }
                }
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
}

@Composable
internal fun JpegSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val tone = state.values.imageTone
    SettingsPageContainer {
        SettingsGroup {
            NumericSliderRow(ImageToneSpecs.jpegQuality, tone.jpegQuality) {
                dispatch.invoke(SetNumericValue(ImageToneNumericParameter.JpegQuality, it))
            }
            SettingDivider()
            SelectorDropdownRow(state, ChoiceSelectorKind.JpegChromaSubsampling, dispatch)
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
    SettingsPageContainer {
        SettingsGroup(
            description = "Lossless writes compressed DNGs at roughly half the size. Uncompressed keeps the legacy byte layout."
        ) {
            SelectorDropdownRow(state, ChoiceSelectorKind.DngCompression, dispatch, title = "Compression")
        }
    }
}

@Composable
private fun <T> RawrAeChoice(
    title: String,
    values: List<T>,
    selected: T,
    label: (T) -> String,
    summary: String?,
    optionText: ((T) -> String?)? = null,
    onSelected: (T) -> Unit
) {
    SettingDivider()
    DropdownChoiceRow(
        title = title,
        selected = selected,
        values = values,
        label = label,
        supportingText = summary,
        optionText = optionText,
        onSelect = onSelected
    )
}
