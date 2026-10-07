package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Edits owned by the capture settings feature. */
internal fun reduceCaptureSettings(
    state: SettingsUiState, action: SettingsApplicationAction
): SettingsUiState = when (action) {
    is SetSelfTimer -> {
        state.withValues(state.values.copy(selfTimer = action.value))
    }

    is SetLocationTagging -> {
        state.withValues(state.values.copy(locationTagging = action.enabled)).withApplication(
            locationTagging = SettingApplicationState(action.enabled, SettingApplicationStatus.Applied)
        )
    }

    is SetOisPreference -> {
        if (state.capabilities.oisSupported) {
            state.withValues(state.values.copy(oisEnabledPreference = action.enabled)).withApplication(
                ois = SettingApplicationState(action.enabled, SettingApplicationStatus.Applied)
            )
        } else {
            state
        }
    }

    is SetAntiFlicker -> {
        state.withValues(state.values.copy(antiFlicker = action.value)).withApplication(
            antiFlicker = SettingApplicationState(action.value, SettingApplicationStatus.Applied)
        )
    }

    is SetExposureStep -> {
        state.withValues(state.values.copy(exposureStep = action.value))
    }

    is SetHighlightProtection -> {
        state.withValues(state.values.copy(highlightProtection = action.value))
    }

    is SetControlSurfaceStyle -> {
        state.withValues(state.values.copy(controlSurfaceStyle = action.style))
    }

    is SetLensProfiles -> {
        state.withValues(state.values.copy(lensProfiles = action.lenses))
    }

    is SetCaptureControlLayout -> {
        state.withValues(state.values.copy(captureControlLayout = action.layout))
    }

    is SetGridMode -> {
        state.withValues(state.values.copy(gridMode = action.value))
    }

    is SetViewfinderDivisor -> {
        state.withValues(state.values.copy(viewfinderDivisor = action.divisor.coerceIn(1, 4)))
    }
    else -> error("Unsupported capture settings action: $action")
}
