package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Edits owned by the runtime settings feature. */
internal fun reduceRuntimeSettings(
    state: SettingsUiState, action: SettingsApplicationAction, defaults: SettingsValues
): SettingsUiState = when (action) {
    is RestorePersistentSettings -> {
        restorePersistentValues(state, action.values, defaults)
    }

    is ApplySettingsCapabilities -> {
        applyCapabilities(state, action.capabilities)
    }

    is ReportOisApplication -> {
        state.acceptReport(action.contextGeneration) { withApplication(ois = action.state) }
    }

    is ReportAntiFlickerApplication -> {
        state.acceptReport(action.contextGeneration) { withApplication(antiFlicker = action.state) }
    }

    is ReportImageToneApplication -> {
        state.acceptReport(action.contextGeneration) { withApplication(imageTone = action.state) }
    }
    else -> error("Unsupported runtime settings action: $action")
}

private fun applyCapabilities(state: SettingsUiState, capabilities: SettingsCapabilities): SettingsUiState {
    val nextGeneration = state.runtime.capabilityContextGeneration + 1L
    return state.copy(
        capabilities = capabilities,
        runtime =
            state.runtime.copy(
                effective = capabilities.resolveEffective(state.values),
                application =
                    state.runtime.application.copy(
                        // A supported replacement context has not acknowledged these preferences yet.
                        // Never carry Applied/Failed/Unsupported state from the previous camera.
                        ois =
                            if (capabilities.oisSupported) {
                                SettingApplicationState(status = SettingApplicationStatus.Pending)
                            } else {
                                SettingApplicationState(status = SettingApplicationStatus.Unsupported)
                            }
                    ),
                capabilityContextGeneration = nextGeneration
            )
    )
}

private inline fun SettingsUiState.acceptReport(
    generation: Long,
    update: SettingsUiState.() -> SettingsUiState
): SettingsUiState = if (generation == runtime.capabilityContextGeneration) update() else this

internal fun restorePersistentValues(
    state: SettingsUiState, persisted: SettingsValues, defaults: SettingsValues
): SettingsUiState {
    val clean = sanitizeSettingsValues(persisted, defaults, state.capabilities)
    return state.copy(
        values = clean,
        runtime =
            state.runtime.copy(
                effective = state.capabilities.resolveEffective(clean),
                application =
                    state.runtime.application.copy(
                        ois =
                            if (state.capabilities.oisSupported) {
                                SettingApplicationState(
                                    clean.oisEnabledPreference,
                                    SettingApplicationStatus.Applied
                                )
                            } else {
                                SettingApplicationState(status = SettingApplicationStatus.Unsupported)
                            },
                        antiFlicker =
                            SettingApplicationState(
                                clean.antiFlicker,
                                SettingApplicationStatus.Applied
                            ),
                        imageTone = SettingApplicationState(
                            clean.activeImageTone(),
                            SettingApplicationStatus.Applied
                        )
                    )
            )
    )
}
