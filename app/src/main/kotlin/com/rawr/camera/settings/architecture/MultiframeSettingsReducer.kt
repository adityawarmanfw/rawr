package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Edits owned by the multiframe settings feature. */
internal fun reduceMultiframeSettings(
    state: SettingsUiState, action: SettingsApplicationAction
): SettingsUiState = when (action) {
    is SetMultiframeChromaDenoise -> {
        state.withValues(state.values.copy(multiframeChromaDenoise = action.enabled))
    }

    is SetMultiframeBaseFrameMode -> {
        state.withValues(state.values.copy(multiframeBaseFrameMode = action.value))
    }

    is SetMultiframeNumericValue -> {
        val spec = MultiframeSpecs.forParameter(action.parameter)
        if (!spec.accepts(action.value)) {
            state
        } else {
            val values = state.values
            state.withValues(
                values.copy(multiframeTuning = values.multiframeTuning.withValue(action.parameter, action.value))
            )
        }
    }

    is SetMultiframeOutputResolution -> {
        val values = state.values
        state.withValues(
            values.copy(multiframeTuning = values.multiframeTuning.copy(outputResolution = action.value))
        )
    }

    is SetMultiframeMergeAlgorithm -> {
        val values = state.values
        state.withValues(values.copy(multiframeTuning = values.multiframeTuning.copy(mergeAlgorithm = action.value)))
    }
    else -> error("Unsupported multiframe settings action: $action")
}
