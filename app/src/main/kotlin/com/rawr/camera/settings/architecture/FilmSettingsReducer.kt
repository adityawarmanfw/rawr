package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Edits owned by the film settings feature. */
internal fun reduceFilmSettings(
    state: SettingsUiState, action: SettingsApplicationAction
): SettingsUiState = when (action) {
    is SetFilmSimEnabled -> {
        state.withValues(state.values.copy(filmSimEnabled = action.enabled))
    }

    is SetFilmSimNumericValue -> state.withValues(state.values.withFilmNumeric(action))

    is SetFilmSimDiscreteValue -> state.withValues(state.values.withFilmDiscrete(action))

    is SetFilmSimFlag -> state.withValues(state.values.withFilmFlag(action))

    is SelectFilmPreset -> state.withValues(state.values.withFilmPreset(action.id))

    is SaveFilmPreset -> {
        val trimmed = action.name.trim().take(48)
        if (trimmed.isEmpty()) {
            state
        } else {
            val id = "user-" + System.currentTimeMillis().toString(36)
            val preset = FilmPreset(id, trimmed, state.values.filmSimLook)
            state.withValues(
                state.values.copy(
                    filmPresets = state.values.filmPresets + preset,
                    selectedFilmPresetId = id
                )
            )
        }
    }

    is RenameFilmPreset -> {
        val trimmed = action.name.trim().take(48)
        if (trimmed.isEmpty()) {
            state
        } else {
            state.withValues(
                state.values.copy(
                    filmPresets = state.values.filmPresets.map {
                        if (it.id == action.id) it.copy(name = trimmed) else it
                    }
                )
            )
        }
    }

    is DeleteFilmPreset -> {
        state.withValues(
            state.values.copy(
                filmPresets = state.values.filmPresets.filterNot { it.id == action.id },
                selectedFilmPresetId = state.values.selectedFilmPresetId
                    ?.takeUnless { it == action.id }
            )
        )
    }

    is RevertFilmPreset -> {
        val preset = state.values.selectedFilmPreset()
        if (preset == null || preset.look == state.values.filmSimLook) {
            state
        } else {
            state.withValues(state.values.copy(filmSimLook = preset.look))
        }
    }

    is UpdateFilmPreset -> {
        val existing = state.values.filmPresets.firstOrNull { it.id == action.id }
        if (existing == null || existing.look == state.values.filmSimLook) {
            state
        } else {
            state.withValues(
                state.values.copy(
                    filmPresets = state.values.filmPresets.map {
                        if (it.id == action.id) it.copy(look = state.values.filmSimLook) else it
                    },
                    selectedFilmPresetId = action.id
                )
            )
        }
    }
    else -> error("Unsupported film settings action: $action")
}
