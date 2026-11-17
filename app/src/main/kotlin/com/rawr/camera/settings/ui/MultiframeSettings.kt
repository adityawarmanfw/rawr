package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun MultiframeSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val enabled = state.values.experimentalMultiframeEnabled
    val tuning = state.values.multiframeTuning
    val spec = { parameter: MultiframeNumericParameter -> MultiframeSpecs.forParameter(parameter) }
    val rexture = tuning.mergeAlgorithm == MultiframeMergeAlgorithm.Rexture
    SettingsPageContainer {
        SettingsGroup(
            description = "Multiframe still pipeline. Off leaves the existing single-frame capture path untouched."
        ) {
            SettingsSwitchRow(
                title = "Multiframe",
                checked = enabled
            ) { dispatch.invoke(SetExperimentalMultiframeEnabled(it)) }
        }
        SettingsGroup {
            SettingsSwitchRow(
                title = "Save Base DNG",
                checked = state.values.saveBaseDng,
                enabled = enabled && state.values.dngEnabled
            ) { dispatch.invoke(SetSaveBaseDng(it)) }
        }
        SettingsGroup(
            title = "Merge Algorithm",
            description = "Both merges use the sharpest frame of the last quarter second before the shutter as the base. " +
                "HDR+ Quality: per-frequency merge, cleaner with mild sharpening; a frame is left out of the tiles where it moved, so moving subjects stay sharp. " +
                "Rexture: kernel-regression merge of the whole burst at native resolution, aligned and weighted per pixel. Equal-exposure bursts. Up to about 56 MP it merges the whole frame (JPEG and DNG); above that it merges in strips and saves DNG only. " +
                "(Native resolution only.)"
        ) {
            MultiframeMergeAlgorithm.entries.forEachIndexed { index, candidate ->
                if (index > 0) SettingDivider()
                SettingsSelectionRow(
                    title = candidate.label,
                    selected = tuning.mergeAlgorithm == candidate,
                    enabled = enabled
                ) { dispatch.invoke(SetMultiframeMergeAlgorithm(candidate)) }
            }
        }
        SettingsGroup(
            title = "Output",
            description = "Double-tap any parameter label to restore its default."
        ) {
            // Needs the burst noise fit, which both merges run; JPEG only: the merged DNG is written before rendering.
            SettingsSwitchRow(
                title = "Chroma Denoise",
                checked = state.values.multiframeChromaDenoise,
                supportingText =
                    "JPEG only. Removes leftover colour blotches from the merged image (chroma only, luma untouched) using the noise measured from the burst. Separate from Image > Denoise, which applies to single frames.",
                enabled = enabled
            ) { dispatch.invoke(SetMultiframeChromaDenoise(it)) }
            SettingDivider()
            if (rexture) {
                SettingsSwitchRow(
                    title = "Rexture Native RGB",
                    checked = state.values.multiframeRextureNativeRgb,
                    supportingText = "JPEG only. Renders the JPEG from Rexture's own merged colour instead of re-mosaicking it and running the Demosaic setting. The DNG is unaffected.",
                    enabled = enabled
                ) { dispatch.invoke(SetMultiframeRextureNativeRgb(it)) }
            }
            SettingDivider()
            MultiframeSlider(spec(MultiframeNumericParameter.MaxFrames), tuning, enabled, dispatch)
        }
    }
}

@Composable
private fun MultiframeSlider(
    spec: MultiframeNumericSpec,
    tuning: MultiframeTuning,
    enabled: Boolean,
    dispatch: SettingsDispatch
) {
    StandaloneNumericSliderRow(
        identity = "mf_${spec.parameter.name}",
        label = spec.label,
        supportingText = spec.supportingText,
        minimum = spec.minimum,
        maximum = spec.maximum,
        step = spec.step,
        decimals = spec.decimals,
        value = tuning.value(spec.parameter).coerceIn(spec.minimum, spec.maximum),
        enabled = enabled,
        defaultValue = spec.defaultValue,
        unit = spec.unit,
        valueFormatter = null
    ) { dispatch.invoke(SetMultiframeNumericValue(spec.parameter, it)) }
}
