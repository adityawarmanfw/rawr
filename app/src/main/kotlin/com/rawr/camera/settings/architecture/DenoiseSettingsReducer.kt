package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Edits owned by the denoise settings feature. */
internal fun reduceDenoiseSettings(
    state: SettingsUiState, action: SettingsApplicationAction
): SettingsUiState = when (action) {
    is SetPhotoDenoiseEnabled -> {
        // Master switch for photo denoisers. Transitions live on the
        // sealed type; native enforces the same precedence as backstop.
        state.withValues(state.values.copy(photoDenoise = state.values.photoDenoise.withMaster(action.enabled)))
    }

    is SetPhotoDenoiseStrength -> {
        state.withValues(state.values.copy(photoDenoise = state.values.photoDenoise.withWaveletStrength(action.value)))
    }

    is SetPhotoDenoiseDetail -> {
        state.withValues(state.values.copy(photoDenoise = state.values.photoDenoise.withWaveletDetail(action.value)))
    }

    is SetPhotoDenoiseLuma -> {
        state.withValues(state.values.copy(photoDenoise = state.values.photoDenoise.withWaveletLuma(action.value)))
    }

    is SetPhotoDenoiseScales -> {
        state.withValues(state.values.copy(photoDenoise = state.values.photoDenoise.withWaveletScales(action.value)))
    }

    is SetVideoDenoiseEnabled -> {
        state.withValues(state.values.copy(videoDenoiseEnabled = action.enabled))
    }

    is SetVideoDenoiseStrength -> {
        state.withValues(state.values.copy(videoDenoiseStrength = action.value.coerceIn(0f, 8f)))
    }

    is SetVideoDenoiseDetail -> {
        state.withValues(state.values.copy(videoDenoiseDetail = action.value.coerceIn(0f, 1.8f)))
    }

    is SetVideoDenoiseLuma -> {
        state.withValues(state.values.copy(videoDenoiseLuma = action.value.coerceIn(0f, 1f)))
    }

    is SetVideoDenoiseScales -> {
        state.withValues(state.values.copy(videoDenoiseScales = action.value.coerceIn(1, 7)))
    }
    else -> error("Unsupported denoise settings action: $action")
}
