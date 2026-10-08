package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Edits owned by the imageprocessing settings feature. */
internal fun reduceImageProcessingSettings(
    state: SettingsUiState, action: SettingsApplicationAction
): SettingsUiState = when (action) {
    is SetDemosaicAlgorithm -> {
        state.withValues(state.values.copy(demosaicAlgorithm = action.value))
    }

    is SetDualAutoContrast -> {
        state.withValues(state.values.copy(dualAutoContrast = action.enabled))
    }

    is SetDualContrastPercent -> {
        state.withValues(state.values.copy(dualContrastPercent = action.value.coerceIn(0f, 100f)))
    }

    is SetQuadfixEnabled -> {
        state.withValues(state.values.copy(quadfixEnabled = action.enabled))
    }

    is SetQuadfixFastMedian -> {
        state.withValues(state.values.copy(quadfixFastMedian = action.enabled))
    }

    is SetPhotoFccSteps -> {
        state.withValues(state.values.copy(photoFccSteps = action.value.coerceIn(1, 8)))
    }

    is SetVideoFccEnabled -> {
        state.withValues(state.values.copy(videoFccEnabled = action.enabled))
    }

    is SetVideoEncoder -> {
        state.withValues(state.values.copy(videoEncoder = action.value.sanitized()))
    }

    is SetPhotoDefringeEnabled -> {
        state.withValues(state.values.copy(photoDefringeEnabled = action.enabled))
    }

    is SetPhotoDefringeStrength -> {
        state.withValues(state.values.copy(photoDefringeStrength = action.value.coerceIn(0f, 1f)))
    }

    is SetPhotoDefringeEdgeThreshold -> {
        state.withValues(state.values.copy(photoDefringeEdgeThreshold = action.value.coerceIn(0.005f, 0.2f)))
    }

    is SetPhotoDefringeLumaFloor -> {
        state.withValues(state.values.copy(photoDefringeLumaFloor = action.value.coerceIn(0f, 0.5f)))
    }

    is SetVideoDefringeEnabled -> {
        state.withValues(state.values.copy(videoDefringeEnabled = action.enabled))
    }

    is SetVideoDefringeStrength -> {
        state.withValues(state.values.copy(videoDefringeStrength = action.value.coerceIn(0f, 1f)))
    }

    is SetPhotoLensShadingEnabled -> {
        state.withValues(state.values.copy(photoLensShadingEnabled = action.enabled))
    }

    is SetVideoLensShadingEnabled -> {
        state.withValues(state.values.copy(videoLensShadingEnabled = action.enabled))
    }

    is SetDistortionCorrectionEnabled -> {
        state.withValues(state.values.copy(distortionCorrectionEnabled = action.enabled))
    }

    is SetPhotoHighlightEnabled -> {
        state.withValues(state.values.copy(photoHighlightEnabled = action.enabled))
    }

    is SetPhotoHighlightMethod -> {
        state.withValues(state.values.copy(photoHighlightMethod = action.method.coerceIn(0, 1)))
    }

    is SetPhotoHighlightThreshold -> {
        state.withValues(state.values.copy(photoHighlightThreshold = action.value.coerceIn(0.5f, 2f)))
    }

    is SetPhotoHighlightCompression -> {
        state.withValues(state.values.copy(photoHighlightCompression = action.value.coerceIn(0f, 300f)))
    }

    is SetVideoHighlightEnabled -> {
        state.withValues(state.values.copy(videoHighlightEnabled = action.enabled))
    }

    is SetVideoHighlightMethod -> {
        state.withValues(state.values.copy(videoHighlightMethod = action.method.coerceIn(0, 1)))
    }

    is SetVideoHighlightThreshold -> {
        state.withValues(state.values.copy(videoHighlightThreshold = action.value.coerceIn(0.5f, 2f)))
    }

    is SetVideoHighlightCompression -> {
        state.withValues(state.values.copy(videoHighlightCompression = action.value.coerceIn(0f, 300f)))
    }
    else -> error("Unsupported imageprocessing settings action: $action")
}
