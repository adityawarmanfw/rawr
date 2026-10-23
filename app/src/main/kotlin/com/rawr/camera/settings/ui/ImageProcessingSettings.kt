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
internal fun HighlightReconstructionSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("Highlight Reconstruction")) {
        HighlightReconstructionGroup(state, dispatch)
    }
}

@Composable
internal fun HighlightReconstructionGroup(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsGroup(description = "Captured stills use the Photo method. Video recordings use the Video method. Live preview uses color propagation.") {
        PhotoVideoTargetSelector(
            photoOn = v.photoHighlightEnabled,
            videoOn = v.videoHighlightEnabled,
            onPhotoChange = { dispatch.invoke(SetPhotoHighlightEnabled(it)) },
            onVideoChange = { dispatch.invoke(SetVideoHighlightEnabled(it)) }
        )
    }
    SettingsGroup(title = "Photo") {
        HighlightMethodOptions(
            selectedMethod = v.photoHighlightMethod,
            selectionEnabled = v.photoHighlightEnabled,
            strengthEnabled = v.photoHighlightEnabled,
            threshold = v.photoHighlightThreshold,
            compression = v.photoHighlightCompression,
            compressionDefault = 45f,
            thresholdIdentity = "highlight_threshold_photo",
            onMethod = { dispatch.invoke(SetPhotoHighlightMethod(it)) },
            onThreshold = { dispatch.invoke(SetPhotoHighlightThreshold(it)) },
            onCompression = { dispatch.invoke(SetPhotoHighlightCompression(it)) }
        )
    }
    SettingsGroup(title = "Video") {
        HighlightMethodOptions(
            selectedMethod = v.videoHighlightMethod,
            selectionEnabled = v.videoHighlightEnabled,
            strengthEnabled = v.videoHighlightEnabled,
            threshold = v.videoHighlightThreshold,
            compression = v.videoHighlightCompression,
            compressionDefault = 100f,
            thresholdIdentity = "highlight_threshold_video",
            onMethod = { dispatch.invoke(SetVideoHighlightMethod(it)) },
            onThreshold = { dispatch.invoke(SetVideoHighlightThreshold(it)) },
            onCompression = { dispatch.invoke(SetVideoHighlightCompression(it)) }
        )
    }
}

@Composable
private fun HighlightMethodOptions(
    selectedMethod: Int,
    selectionEnabled: Boolean,
    strengthEnabled: Boolean,
    threshold: Float,
    compression: Float,
    compressionDefault: Float,
    thresholdIdentity: String,
    onMethod: (Int) -> Unit,
    onThreshold: (Float) -> Unit,
    onCompression: (Float) -> Unit
) {
    SettingsSelectionRow(
        title = "Color propagation",
        selected = selectedMethod == 0,
        enabled = selectionEnabled,
        onClick = { onMethod(0) }
    )
    SettingDivider()
    SettingsSelectionRow(
        title = "Inpaint Opposed (Coloropp)",
        selected = selectedMethod == 1,
        enabled = selectionEnabled,
        onClick = { onMethod(1) }
    )
    if (selectedMethod == 1) {
        SettingDivider()
        StandaloneNumericSliderRow(
            identity = "${thresholdIdentity}",
            label = "Highlight threshold",
            supportingText = "RawTherapee Hlth; higher values reconstruct more pixels",
            minimum = 0.5f, maximum = 2f, step = 0.05f, decimals = 2,
            value = threshold, defaultValue = 1f, enabled = strengthEnabled
        ) { onThreshold(it) }
        SettingDivider()
        StandaloneNumericSliderRow(
            identity = "${thresholdIdentity}_compression",
            label = "Highlight compression",
            supportingText = "SDR shoulder strength; UltraHDR color stays uncompressed",
            minimum = 0f, maximum = 300f, step = 1f, decimals = 0,
            value = compression, defaultValue = compressionDefault, enabled = strengthEnabled
        ) { onCompression(it) }
    }
}

private fun dynamicRangeSummary(v: SettingsValues): String {
    val photo = when (v.photoDynamicRange()) {
        DynamicRangeMethod.Local -> "Local +${"%.1f".format(java.util.Locale.US, v.localToneShadowsEv)} EV"
        DynamicRangeMethod.Global -> "Global ${v.autoKeyExposure.label}"
        DynamicRangeMethod.Off -> "Off"
    }
    val video = if (v.videoDynamicRange() == DynamicRangeMethod.Off) "Off" else "Global ${v.videoAutoKeyExposure.label}"
    return "Photo $photo · Video $video"
}

/** Image > Rendering > Dynamic Range: fitting the scene's range before Tone Mapping. */
@Composable
internal fun DynamicRangeSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("DynamicRange")) {
        SettingsGroup(
            title = "Photo",
            description = "Fits a high-contrast scene into the picture by brightening shadows; highlights keep their brightness. Runs before Tone Mapping. JPEG and viewfinder only; not applied to LOG."
        ) {
            val method = v.photoDynamicRange()
            CompactChoiceRow(visibleDynamicRangeMethods, method, { it.label }) {
                when (it) {
                    DynamicRangeMethod.Off -> {
                        dispatch.invoke(SetLocalTone(mode = PhotoToneMapping.Adaptive))
                        dispatch.invoke(SetAutoKeyExposure(AutoKeyExposure.Off))
                    }
                    DynamicRangeMethod.Global -> {
                        dispatch.invoke(SetLocalTone(mode = PhotoToneMapping.Adaptive))
                        if (v.autoKeyExposure == AutoKeyExposure.Off) dispatch.invoke(SetAutoKeyExposure(AutoKeyExposure.Medium))
                    }
                    DynamicRangeMethod.Local -> dispatch.invoke(SetLocalTone(mode = PhotoToneMapping.Local))
                }
            }
            ChoiceSummary(method.summary)
            when (method) {
                DynamicRangeMethod.Local -> if (LOCAL_TONE_CONTROLS_VISIBLE) {
                    SettingDivider()
                    StandaloneNumericSliderRow(
                        identity = "local_tone_shadows",
                        label = "Strength",
                        supportingText = "How far dark regions may be brightened. Lowered automatically when the frame is too noisy for it. Lower: closer to the plain capture. Higher: brighter shadows, flatter overall.",
                        minimum = LocalToneLimits.MIN_SHADOWS_EV,
                        maximum = LocalToneLimits.MAX_SHADOWS_EV,
                        step = .1f,
                        decimals = 1,
                        unit = "EV",
                        value = v.localToneShadowsEv,
                        defaultValue = LocalToneLimits.DEFAULT_SHADOWS_EV
                    ) { dispatch.invoke(SetLocalTone(shadowsEv = it)) }
                    val tuning = v.localToneTuning
                    SettingDivider()
                    StandaloneNumericSliderRow(
                        identity = "local_tone_highlights", label = "Highlight protection",
                        supportingText = "Uses a darker exposure for bright areas. Higher: stronger highlight reduction.",
                        minimum = .5f, maximum = 2f, step = .1f, decimals = 1, unit = "EV",
                        value = tuning.highlightsEv, defaultValue = 2f
                    ) { dispatch.invoke(SetLocalTone(tuning = tuning.copy(highlightsEv = it))) }
                    SettingDivider()
                    StandaloneNumericSliderRow(
                        identity = "local_tone_selectivity", label = "Exposure selectivity",
                        supportingText = "Higher: stronger separation of shadows and highlights. Lower: gentler blending.",
                        minimum = 3f, maximum = 5f, step = .1f, decimals = 1,
                        value = tuning.sigma, defaultValue = 5f
                    ) { dispatch.invoke(SetLocalTone(tuning = tuning.copy(sigma = it))) }
                    SettingDivider()
                    StandaloneNumericSliderRow(
                        identity = "local_tone_scale", label = "Blend scale",
                        supportingText = "Higher: blends brightness across larger areas and changes local contrast.",
                        minimum = 1f, maximum = 3f, step = 1f, decimals = 0,
                        value = (tuning.coarsestMip - 5).toFloat(), defaultValue = 1f
                    ) { dispatch.invoke(SetLocalTone(tuning = tuning.copy(coarsestMip = it.toInt() + 5))) }
                    SettingDivider()
                    SettingsSwitchRow(title = "Local contrast boost", checked = tuning.boostLocalContrast) {
                        dispatch.invoke(SetLocalTone(tuning = tuning.copy(boostLocalContrast = it)))
                    }
                    ChoiceSummary("Conservative ranges. Strong combinations can still exaggerate edges; leave contrast boost off for a softer look.")
                    androidx.compose.material3.TextButton(onClick = {
                        dispatch.invoke(SetLocalTone(shadowsEv = LocalToneLimits.DEFAULT_SHADOWS_EV, tuning = LocalToneTuning()))
                    }) { Text("Reset local tone") }
                }
                DynamicRangeMethod.Global -> {
                    SettingDivider()
                    ChoiceLabel("Strength")
                    CompactChoiceRow(globalDynamicRangeStrengths, v.autoKeyExposure, { it.label }) {
                        dispatch.invoke(SetAutoKeyExposure(it))
                    }
                    ChoiceSummary(v.autoKeyExposure.summary)
                }
                DynamicRangeMethod.Off -> Unit
            }
        }
        SettingsGroup(
            title = "Video",
            description = "Video uses the global curve. Not applied to LOG."
        ) {
            val method = v.videoDynamicRange()
            CompactChoiceRow(listOf(DynamicRangeMethod.Off, DynamicRangeMethod.Global), method, { it.label }) {
                dispatch.invoke(
                    SetAutoKeyExposure(
                        if (it == DynamicRangeMethod.Off) AutoKeyExposure.Off
                        else v.videoAutoKeyExposure.takeIf { s -> s != AutoKeyExposure.Off } ?: AutoKeyExposure.Medium,
                        video = true
                    )
                )
            }
            ChoiceSummary(method.summary)
            if (method == DynamicRangeMethod.Global) {
                SettingDivider()
                ChoiceLabel("Strength")
                CompactChoiceRow(globalDynamicRangeStrengths, v.videoAutoKeyExposure, { it.label }) {
                    dispatch.invoke(SetAutoKeyExposure(it, video = true))
                }
                ChoiceSummary(v.videoAutoKeyExposure.summary)
            }
        }
    }
}

@Composable
private fun ChoiceLabel(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodyLarge,
        modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp)
    )
}

@Composable
private fun ChoiceSummary(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(start = 16.dp, end = 16.dp, bottom = 12.dp)
    )
}

private fun highlightMethodLabel(method: Int): String =
    if (method == 1) "Inpaint Opposed" else "Color propagation"

@Composable
internal fun ImageSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("Image")) {
        SettingsGroup(title = "Rendering") {
            SettingsRow(
                "Dynamic Range",
                dynamicRangeSummary(v),
                testTag = SettingsTestTags.row("Image", "dynamic_range"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.DynamicRange)) }
            )
            SettingDivider()
            SettingsRow(
                "Tone & Color",
                "Tone mapping, look and tone controls",
                testTag = SettingsTestTags.row("Image", "tone_color"),
                onClick = {
                    dispatch.invoke(OpenSection(SettingsSection.ImageTone))
                }
            )
            SettingDivider()
            ToggleSubmenuRow(
                title = "Film Simulation",
                checked = v.filmSimEnabled,
                testTag = SettingsTestTags.row("Image", "film_simulation"),
                onOpen = { dispatch.invoke(OpenSection(SettingsSection.FilmSim)) },
                onCheckedChange = { dispatch.invoke(SetFilmSimEnabled(it)) }
            )
        }
        SettingsGroup(title = "Enhancement") {
            SettingsRow(
                "Demosaic",
                if (v.videoFccEnabled) {
                    "Photo FCC ${v.photoFccSteps} · Video FCC on"
                } else {
                    "Photo FCC ${v.photoFccSteps} · Video FCC off"
                },
                testTag = SettingsTestTags.row("Image", "demosaic"),
                onClick = {
                    dispatch.invoke(OpenSection(SettingsSection.Demosaic))
                }
            )
            SettingDivider()
            SettingsRow(
                title = "Highlight reconstruction",
                value = photoVideoSummary(v.photoHighlightEnabled, v.videoHighlightEnabled),
                testTag = SettingsTestTags.row("Image", "highlight_reconstruction"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.HighlightReconstruction)) }
            )
            SettingDivider()
            SettingsRow(
                title = "Defringe",
                value = photoVideoSummary(v.photoDefringeEnabled, v.videoDefringeEnabled),
                testTag = SettingsTestTags.row("Image", "defringe"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.Defringe)) }
            )
            SettingDivider()
            SettingsRow(
                title = "Denoise",
                value = photoVideoSummary(v.photoDenoise is DenoiseConfig.Wavelet, v.videoDenoiseEnabled),
                testTag = SettingsTestTags.row("Image", "denoise"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.Denoise)) }
            )
            SettingDivider()
            CaptureSharpeningRows(state, dispatch)
            SettingDivider()
            SettingsRow(
                title = "Distortion correction",
                value = photoVideoSummary(v.distortionCorrectionEnabled, v.videoDistortionCorrectionEnabled),
                testTag = SettingsTestTags.row("Image", "distortion"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.Distortion)) }
            )
            SettingDivider()
            SettingsRow(
                title = "Lens shading",
                value = photoVideoSummary(v.photoLensShadingEnabled, v.videoLensShadingEnabled),
                testTag = SettingsTestTags.row("Image", "lens_shading"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.LensShading)) }
            )
        }
    }
}

private fun photoDenoiseLabel(denoise: DenoiseConfig): String = when (denoise) {
    is DenoiseConfig.Off -> "Off"
    is DenoiseConfig.Wavelet -> "Wavelet %.1f".format(denoise.config.strength)
}

@Composable
internal fun DemosaicSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsPageContainer {
        DemosaicGroup(state, dispatch)
    }
}

@Composable
internal fun DemosaicGroup(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsGroup(
        title = "Demosaic · Photo",
        description = "Still-capture demosaic only. The live RAW preview pipeline is unchanged."
    ) {
        SettingsSelectionRow(
            title = "AMaZE",
            selected = state.values.demosaicAlgorithm == DemosaicAlgorithm.Amaze,
            onClick = { dispatch.invoke(SetDemosaicAlgorithm(DemosaicAlgorithm.Amaze)) }
        )
        SettingDivider()
        SettingsSelectionRow(
            title = "AMaZE + VNG4",
            selected = state.values.demosaicAlgorithm == DemosaicAlgorithm.DualAmazeVng4,
            onClick = { dispatch.invoke(SetDemosaicAlgorithm(DemosaicAlgorithm.DualAmazeVng4)) }
        )
        SettingDivider()
        SettingsSelectionRow(
            title = "RCD",
            selected = state.values.demosaicAlgorithm == DemosaicAlgorithm.Rcd,
            onClick = { dispatch.invoke(SetDemosaicAlgorithm(DemosaicAlgorithm.Rcd)) }
        )
        SettingDivider()
        SettingsSelectionRow(
            title = "VNG4",
            selected = state.values.demosaicAlgorithm == DemosaicAlgorithm.Vng4,
            onClick = { dispatch.invoke(SetDemosaicAlgorithm(DemosaicAlgorithm.Vng4)) }
        )
        SettingDivider()
        SettingsSelectionRow(
            title = "RCD + VNG4",
            selected = state.values.demosaicAlgorithm == DemosaicAlgorithm.DualRcdVng4,
            onClick = { dispatch.invoke(SetDemosaicAlgorithm(DemosaicAlgorithm.DualRcdVng4)) }
        )
        if (state.values.demosaicAlgorithm == DemosaicAlgorithm.DualRcdVng4 ||
            state.values.demosaicAlgorithm == DemosaicAlgorithm.DualAmazeVng4
        ) {
            SettingDivider()
            SettingsSwitchRow(
                title = "Auto threshold",
                checked = state.values.dualAutoContrast
            ) { dispatch.invoke(SetDualAutoContrast(it)) }
            SettingDivider()
            StandaloneNumericSliderRow(
                identity = "dual_contrast_threshold",
                label = "Contrast threshold",
                minimum = 0f,
                maximum = 100f,
                step = 1f,
                decimals = 0,
                value = state.values.dualContrastPercent,
                enabled = !state.values.dualAutoContrast
            ) { dispatch.invoke(SetDualContrastPercent(it)) }
        }
        SettingDivider()
        StandaloneNumericSliderRow(
            identity = "fcc_steps_photo",
            label = "False color correction steps",
            supportingText = "Still captures",
            minimum = 1f,
            maximum = 8f,
            step = 1f,
            decimals = 0,
            value = state.values.photoFccSteps.toFloat()
        ) { dispatch.invoke(SetPhotoFccSteps(it.toInt())) }
    }
    SettingsGroup(
        title = "False color correction · Video",
        description = "Runs one correction step in new video recordings when enabled."
    ) {
        SettingsSwitchRow(
            title = "Video false color correction",
            checked = state.values.videoFccEnabled
        ) { dispatch.invoke(SetVideoFccEnabled(it)) }
    }
}

@Composable
internal fun DefringeSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsPageContainer {
        DefringeGroup(state, dispatch)
    }
}

@Composable
internal fun LensShadingSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("LensShading")) {
        SettingsGroup(
            description = "Camera2 gain map. Preview and stills follow Photo; recordings follow Video."
        ) {
            PhotoVideoTargetSelector(
                photoOn = v.photoLensShadingEnabled,
                videoOn = v.videoLensShadingEnabled,
                onPhotoChange = { dispatch.invoke(SetPhotoLensShadingEnabled(it)) },
                onVideoChange = { dispatch.invoke(SetVideoLensShadingEnabled(it)) }
            )
        }
    }
}

@Composable
internal fun DistortionSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("Distortion")) {
        SettingsGroup(
            description = "Straightens lens distortion on lenses with a calibration. Photo covers the viewfinder, " +
                "stills and the DNG lens correction; Video covers recordings and the video viewfinder."
        ) {
            PhotoVideoTargetSelector(
                photoOn = v.distortionCorrectionEnabled,
                videoOn = v.videoDistortionCorrectionEnabled,
                onPhotoChange = { dispatch.invoke(SetDistortionCorrectionEnabled(it)) },
                onVideoChange = { dispatch.invoke(SetVideoDistortionCorrectionEnabled(it)) }
            )
        }
    }
}

@Composable
internal fun DefringeGroup(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsGroup(
        description = "Reduces purple and blue halos around bright edges by repairing nearby color while preserving brightness. Fringe detection and bright-edge protection are automatic; Strength controls how much correction is applied."
    ) {
        PhotoVideoTargetSelector(
            photoOn = v.photoDefringeEnabled,
            videoOn = v.videoDefringeEnabled,
            onPhotoChange = { dispatch.invoke(SetPhotoDefringeEnabled(it)) },
            onVideoChange = { dispatch.invoke(SetVideoDefringeEnabled(it)) }
        )
    }
    SettingsGroup(title = "Photo") {
        StandaloneNumericSliderRow(
            identity = "defringe_strength_photo",
            label = "Strength",
            minimum = 0f, maximum = 1f, step = 0.01f, decimals = 2,
            value = v.photoDefringeStrength,
            enabled = v.photoDefringeEnabled
        ) { dispatch.invoke(SetPhotoDefringeStrength(it)) }
    }
    SettingsGroup(title = "Video") {
        StandaloneNumericSliderRow(
            identity = "defringe_strength_video",
            label = "Strength",
            minimum = 0f, maximum = 1f, step = 0.01f, decimals = 2,
            value = v.videoDefringeStrength,
            enabled = v.videoDefringeEnabled
        ) { dispatch.invoke(SetVideoDefringeStrength(it)) }
    }
}

/**
 * RawTherapee capture sharpening for photos (JPEG and Renderer; the DNG is
 * never sharpened). Off by default; switching it on starts at Low.
 */
@Composable
internal fun CaptureSharpeningRows(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsSwitchRow(
        title = "Capture sharpening",
        checked = v.photoCaptureSharpeningEnabled,
        value = "Photo only",
        supportingText = "Restores detail lost to the lens and demosaic.",
        testTag = SettingsTestTags.row("Image", "capture_sharpening")
    ) { dispatch.invoke(SetPhotoCaptureSharpeningEnabled(it)) }
    if (v.photoCaptureSharpeningEnabled) {
        CompactChoiceRow(CaptureSharpeningLevel.entries, v.photoCaptureSharpeningLevel, { it.label }) {
            dispatch.invoke(SetPhotoCaptureSharpeningLevel(it))
        }
    }
}

/**
 * Renderer-only Develop screen: demosaic, highlight reconstruction,
 * defringe and geometry in one scroll. Unlike [SettingsSection.Image] it
 * carries no Tone/Film/JPEG drill rows — those live on their own tabs.
 * Staged Tier B/C edits apply via the editor Apply bar, not per tap.
 */
@Composable
internal fun DevelopSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("Develop")) {
        DemosaicGroup(state, dispatch)
        SettingsGroup(title = "Highlight reconstruction") {
            SettingsSwitchRow(
                title = "Highlight reconstruction",
                checked = state.values.photoHighlightEnabled
            ) { dispatch.invoke(SetPhotoHighlightEnabled(it)) }
        }
        HighlightReconstructionGroup(state, dispatch)
        SettingsGroup(title = "Defringe") {
            DefringeGroup(state, dispatch)
        }
        SettingsGroup(title = "Sharpening") {
            CaptureSharpeningRows(state, dispatch)
        }
        SettingsGroup(title = "Geometry") {
            SettingsSwitchRow(
                title = "Lens shading correction",
                checked = state.values.photoLensShadingEnabled
            ) { dispatch.invoke(SetPhotoLensShadingEnabled(it)) }
            SettingDivider()
            SettingsSwitchRow(
                title = "Distortion correction",
                checked = state.values.distortionCorrectionEnabled
            ) { dispatch.invoke(SetDistortionCorrectionEnabled(it)) }
        }
    }
}
