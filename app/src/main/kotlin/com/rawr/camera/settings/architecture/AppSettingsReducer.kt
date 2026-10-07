package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Edits owned by the app settings feature. */
internal fun reduceAppSettings(
    state: SettingsUiState, action: SettingsApplicationAction
): SettingsUiState = when (action) {
    is SetSaveLocationTree -> {
        state.withValues(state.values.copy(saveLocationId = "storage.tree:${action.treeUri}"))
    }

    is SetPipelineDiagnosticsEnabled -> {
        state.withValues(state.values.copy(pipelineDiagnosticsEnabled = action.enabled))
    }

    is SetExperimentalZeroCopyEnabled -> {
        state.withValues(state.values.copy(experimentalZeroCopyEnabled = action.enabled))
    }

    is SetSaveBaseDng -> state.withValues(state.values.copy(saveBaseDng = action.enabled))

    is SetExperimentalMultiframeEnabled -> {
        state.withValues(state.values.copy(experimentalMultiframeEnabled = action.enabled))
    }

    is SetUltraHdrEnabled -> {
        state.withValues(state.values.copy(ultraHdrEnabled = action.enabled))
    }

    is SetAePriorityDisabled -> {
        state.withValues(state.values.copy(aePriorityDisabled = action.disabled))
    }

    is SetPersistentEngineEnabled -> {
        state.withValues(state.values.copy(persistentEngineEnabled = action.enabled))
    }

    is SetCustomGpuDriverEnabled -> {
        state.withValues(
            state.values.copy(
                customGpuDriverEnabled =
                    action.enabled && state.values.customGpuDriverName != null
            )
        )
    }

    is SetCustomGpuDriverInstalled -> {
        state.withValues(state.values.copy(customGpuDriverName = action.displayName))
    }

    is SetPersistentDiagnosticsEnabled -> {
        state.withValues(state.values.copy(persistentDiagnosticsEnabled = action.enabled))
    }

    is SetPersistZslRingEnabled -> {
        state.withValues(state.values.copy(persistZslRingEnabled = action.enabled))
    }

    is SetPersistZslRingOnShutterEnabled -> {
        state.withValues(state.values.copy(persistZslRingOnShutterEnabled = action.enabled))
    }

    is SetInternalTraceCaptureEnabled -> {
        state.withValues(state.values.copy(internalTraceCaptureEnabled = action.enabled))
    }

    is SetInternalTraceRetainedRows -> {
        state.withValues(
            state.values.copy(
                internalTraceRetainedRows =
                    action.rows.takeIf { it in setOf(4096, 8192, 16384, 32768) } ?: 8192
            )
        )
    }
    else -> error("Unsupported app settings action: $action")
}
