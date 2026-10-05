package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*
import kotlin.math.roundToInt

@Composable
internal fun MultiframeSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val enabled = state.values.experimentalMultiframeEnabled
    val tuning = state.values.multiframeTuning
    val spec = { parameter: MultiframeNumericParameter -> MultiframeSpecs.forParameter(parameter) }
    val hdrPlus = tuning.mergeAlgorithm != MultiframeMergeAlgorithm.Wronski
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
            description = "Super-resolution: kernel-regression merge, smoother with optional upscaling. HDR+: tile-aligned robust average on the RAW mosaic; keeps natural single-frame grain at higher SNR and avoids blotches in low light. HDR+ Quality: per-frequency merge, cleaner with mild sharpening and blends motion more, slower (both HDR+ modes: native resolution only)."
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
            title = "Reference Frame",
            description = "Which burst frame anchors alignment and merge."
        ) {
            MultiframeBaseFrameMode.entries.forEachIndexed { index, candidate ->
                if (index > 0) SettingDivider()
                SettingsSelectionRow(
                    title = candidate.label,
                    selected = state.values.multiframeBaseFrameMode == candidate
                ) { dispatch.invoke(SetMultiframeBaseFrameMode(candidate)) }
            }
        }
        SettingsGroup(
            title = "Output",
            description = "Double-tap any parameter label to restore its default."
        ) {
            // Needs the burst noise fit, which only the super-resolution merge
            // measures; HDR+ would always skip it. JPEG only: the merged DNG is
            // written before rendering.
            SettingsSwitchRow(
                title = "Chroma Denoise",
                checked = state.values.multiframeChromaDenoise && !hdrPlus,
                supportingText =
                    if (hdrPlus) "Not available for HDR+ yet."
                    else "JPEG only. Removes leftover colour blotches from the merged image (chroma only, luma untouched) using the noise measured from the burst. Separate from Image > Denoise, which applies to single frames.",
                enabled = enabled && !hdrPlus
            ) { dispatch.invoke(SetMultiframeChromaDenoise(it)) }
            SettingDivider()
            if (hdrPlus) {
                MultiframeSlider(spec(MultiframeNumericParameter.HdrPlusStrength), tuning, enabled, dispatch)
                SettingDivider()
                MultiframeSlider(spec(MultiframeNumericParameter.HdrPlusTileSize), tuning, enabled, dispatch)
            } else {
                StandaloneNumericSliderRow(
                    identity = "mf_output_resolution",
                    label = "Merged Resolution",
                    supportingText = "Lower: faster, smaller files at native resolution. Higher: a larger reconstructed image with more cost; it cannot recover absent detail.",
                    minimum = 0f,
                    maximum = (MultiframeOutputResolution.entries.size - 1).toFloat(),
                    step = 1f,
                    decimals = 0,
                    value = tuning.outputResolution.ordinal.toFloat(),
                    enabled = enabled,
                    defaultValue = MultiframeOutputResolution.Native.ordinal.toFloat(),
                    valueFormatter = { value ->
                        MultiframeOutputResolution.entries[
                            value.roundToInt().coerceIn(
                                0,
                                MultiframeOutputResolution.entries.lastIndex
                            )
                        ].label
                    }
                ) { value ->
                    dispatch.invoke(
                        SetMultiframeOutputResolution(
                            MultiframeOutputResolution.entries[
                                value.roundToInt().coerceIn(
                                    0,
                                    MultiframeOutputResolution.entries.lastIndex
                                )
                            ]
                        )
                    )
                }
            }
            SettingDivider()
            MultiframeSlider(spec(MultiframeNumericParameter.MaxFrames), tuning, enabled, dispatch)
        }
        if (!hdrPlus) {
            SettingsGroup(
                title = "Alignment",
                description = "Subpixel refinement and weak-texture rejection. Higher cutoff rejects more ambiguous tiles."
            ) {
                MultiframeSlider(spec(MultiframeNumericParameter.LkIterations), tuning, enabled, dispatch)
                SettingDivider()
                MultiframeSlider(spec(MultiframeNumericParameter.HessianEpsilonExponent), tuning, enabled, dispatch)
            }
            SettingsGroup(
                title = "Reconstruction",
                description = "Kernel shape and the transition between detail reconstruction and denoising."
            ) {
                listOf(
                    spec(MultiframeNumericParameter.KDetail),
                    spec(MultiframeNumericParameter.KDenoise),
                    spec(MultiframeNumericParameter.DThreshold),
                    spec(MultiframeNumericParameter.DTransition),
                    spec(MultiframeNumericParameter.KStretch),
                    spec(MultiframeNumericParameter.KShrink)
                ).forEachIndexed { index, spec ->
                    if (index > 0) SettingDivider()
                    MultiframeSlider(spec, tuning, enabled, dispatch)
                }
            }
            SettingsGroup(
                title = "Detail Bandwidth",
                description = "Decoupled flat denoising, narrow-kernel floor and coverage fallback. The floor never narrows kernels; scale gain grows it above 1x only."
            ) {
                listOf(
                    spec(MultiframeNumericParameter.FlatSigma),
                    spec(MultiframeNumericParameter.DetailFloorSigma),
                    spec(MultiframeNumericParameter.ScaleBandwidthGain),
                    spec(MultiframeNumericParameter.CoverageNeffLo),
                    spec(MultiframeNumericParameter.CoverageNeffHi),
                    spec(MultiframeNumericParameter.CoverageMassLo),
                    spec(MultiframeNumericParameter.CoverageMassHi)
                ).forEachIndexed { index, spec ->
                    if (index > 0) SettingDivider()
                    MultiframeSlider(spec, tuning, enabled, dispatch)
                }
            }
            SettingsGroup(
                title = "Motion Fallback Cleanup",
                description = "Where motion or rejection leaves fewer merged frames, smooth to match the rest of the image. Strength scales per pixel with the missing frames; fully merged areas are untouched."
            ) {
                listOf(
                    spec(MultiframeNumericParameter.FallbackChroma),
                    spec(MultiframeNumericParameter.FallbackLuma)
                ).forEachIndexed { index, spec ->
                    if (index > 0) SettingDivider()
                    MultiframeSlider(spec, tuning, enabled, dispatch)
                }
            }
            SettingsGroup(
                title = "Robustness",
                description = "Controls companion rejection for photometric differences and independently moving regions."
            ) {
                listOf(
                    spec(MultiframeNumericParameter.RobustnessT),
                    spec(MultiframeNumericParameter.RobustnessS1),
                    spec(MultiframeNumericParameter.RobustnessS2),
                    spec(MultiframeNumericParameter.MotionThreshold)
                ).forEachIndexed { index, spec ->
                    if (index > 0) SettingDivider()
                    MultiframeSlider(spec, tuning, enabled, dispatch)
                }
            }
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
        value = tuning.value(spec.parameter),
        enabled = enabled,
        defaultValue = spec.defaultValue,
        unit = spec.unit,
        valueFormatter =
            if (spec.parameter == MultiframeNumericParameter.HessianEpsilonExponent) {
                { value -> "1e${value.roundToInt()}" }
            } else {
                null
            }
    ) { dispatch.invoke(SetMultiframeNumericValue(spec.parameter, it)) }
}
