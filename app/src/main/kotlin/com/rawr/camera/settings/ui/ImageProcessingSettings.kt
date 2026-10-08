package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
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
            value = compression, defaultValue = 100f, enabled = strengthEnabled
        ) { onCompression(it) }
    }
}

private fun highlightMethodLabel(method: Int): String =
    if (method == 1) "Inpaint Opposed" else "Color propagation"

@Composable
internal fun ImageSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("Image")) {
        SettingsGroup(title = "Tonemap") {
            SettingsRow(
                "Tone & Color",
                "Realtime rendering controls",
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
                    "Photo FCC ${v.photoFccSteps} · Video FCC ${v.videoFccSteps}"
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
                value = photoVideoSummary(v.photoDenoise != DenoiseConfig.Off, v.videoDenoiseEnabled),
                testTag = SettingsTestTags.row("Image", "denoise"),
                onClick = { dispatch.invoke(OpenSection(SettingsSection.Denoise)) }
            )
            SettingDivider()
            SettingsSwitchRow(title = "Distortion correction", checked = v.distortionCorrectionEnabled, value = "Photo only", onCheckedChange = {
                dispatch.invoke(SetDistortionCorrectionEnabled(it))
            })
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
    is DenoiseConfig.Galosh -> "Galosh"
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
        if (state.values.demosaicAlgorithm == DemosaicAlgorithm.DualRcdVng4) {
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
        SettingsSwitchRow(
            title = "Quad-cell lattice filter",
            checked = state.values.quadfixEnabled
        ) { dispatch.invoke(SetQuadfixEnabled(it)) }
        if (state.values.quadfixEnabled) {
            SettingDivider()
            SettingsSwitchRow(
                title = "Fast guide median",
                checked = state.values.quadfixFastMedian
            ) { dispatch.invoke(SetQuadfixFastMedian(it)) }
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
        description = "Runs in new video recordings when enabled."
    ) {
        SettingsSwitchRow(
            title = "Video false color correction",
            checked = state.values.videoFccEnabled
        ) { dispatch.invoke(SetVideoFccEnabled(it)) }
        if (state.values.videoFccEnabled) {
            SettingDivider()
            StandaloneNumericSliderRow(
                identity = "fcc_steps_video",
                label = "False color correction steps",
                supportingText = "New video recordings",
                minimum = 1f,
                maximum = 8f,
                step = 1f,
                decimals = 0,
                value = state.values.videoFccSteps.toFloat()
            ) { dispatch.invoke(SetVideoFccSteps(it.toInt())) }
        }
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
internal fun DefringeGroup(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsGroup(
        description = "Reduces purple and blue halos around bright edges. Photo repairs nearby color while preserving brightness; Video uses a lighter desaturation pass."
    ) {
        PhotoVideoTargetSelector(
            photoOn = v.photoDefringeEnabled,
            videoOn = v.videoDefringeEnabled,
            onPhotoChange = { dispatch.invoke(SetPhotoDefringeEnabled(it)) },
            onVideoChange = { dispatch.invoke(SetVideoDefringeEnabled(it)) }
        )
    }
    SettingsGroup(
        title = "Photo",
        description = "Fringe detection and bright-edge protection are automatic. Strength controls how much correction is applied to detected fringes."
    ) {
        StandaloneNumericSliderRow(
            identity = "defringe_strength_photo",
            label = "Strength",
            minimum = 0f, maximum = 1f, step = 0.01f, decimals = 2,
            value = v.photoDefringeStrength,
            enabled = v.photoDefringeEnabled
        ) { dispatch.invoke(SetPhotoDefringeStrength(it)) }
    }
    SettingsGroup(title = "Video") {
        DefringeSliders(
            enabled = v.videoDefringeEnabled,
            identitySuffix = "video",
            strength = v.videoDefringeStrength,
            edgeThreshold = v.videoDefringeEdgeThreshold,
            lumaFloor = v.videoDefringeLumaFloor,
            onStrength = { dispatch.invoke(SetVideoDefringeStrength(it)) },
            onEdgeThreshold = { dispatch.invoke(SetVideoDefringeEdgeThreshold(it)) },
            onLumaFloor = { dispatch.invoke(SetVideoDefringeLumaFloor(it)) }
        )
    }
}

@Composable
private fun DefringeSliders(
    enabled: Boolean,
    identitySuffix: String,
    strength: Float,
    edgeThreshold: Float,
    lumaFloor: Float,
    onStrength: (Float) -> Unit,
    onEdgeThreshold: (Float) -> Unit,
    onLumaFloor: (Float) -> Unit
) {
    StandaloneNumericSliderRow(
        identity = "defringe_strength_$identitySuffix",
        label = "Strength",
        minimum = 0f,
        maximum = 1f,
        step = 0.01f,
        decimals = 2,
        value = strength,
        enabled = enabled
    ) { onStrength(it) }
    SettingDivider()
    StandaloneNumericSliderRow(
        identity = "defringe_edge_threshold_$identitySuffix",
        label = "Edge threshold",
        minimum = 0.005f,
        maximum = 0.2f,
        step = 0.005f,
        decimals = 3,
        value = edgeThreshold,
        enabled = enabled
    ) { onEdgeThreshold(it) }
    SettingDivider()
    StandaloneNumericSliderRow(
        identity = "defringe_luma_floor_$identitySuffix",
        label = "Luma floor",
        minimum = 0f,
        maximum = 0.5f,
        step = 0.01f,
        decimals = 2,
        value = lumaFloor,
        enabled = enabled
    ) { onLumaFloor(it) }
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
