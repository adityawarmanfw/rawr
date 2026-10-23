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

    is SaveFilmPreset, is ImportFilmPreset -> {
        val name = if (action is SaveFilmPreset) action.name else (action as ImportFilmPreset).name
        val look = if (action is ImportFilmPreset) action.look else state.values.filmSimLook
        val trimmed = name.trim().take(48)
        if (trimmed.isEmpty()) {
            state
        } else {
            val id = newPresetId(state.values.filmPresets.map { it.id })
            val preset = FilmPreset(id, trimmed, look)
            state.withValues(
                state.values.copy(
                    filmPresets = state.values.filmPresets + preset,
                    selectedFilmPresetId = if (action is SaveFilmPreset) id else state.values.selectedFilmPresetId
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

/** Unique among [taken]: imports can land within the same millisecond. */
internal fun newPresetId(taken: List<String>): String {
    var stamp = System.currentTimeMillis()
    while ("user-" + stamp.toString(36) in taken) stamp++
    return "user-" + stamp.toString(36)
}

/** Saved AgX looks: a look is applied by selecting its base and writing its values there. */
internal fun reduceAgxLooks(state: SettingsUiState, action: SettingsApplicationAction): SettingsUiState {
    val v = state.values
    fun rename(name: String) = name.trim().take(48)
    return when (action) {
        is SaveAgxLook -> {
            val name = rename(action.name)
            if (name.isEmpty() || !v.isAgxActive()) state
            else state.withValues(v.copy(agxLooks = v.agxLooks +
                AgxLook(newPresetId(v.agxLooks.map { it.id }), name, v.regularAgxPreset(), v.activeAgxValues())))
        }
        is ImportAgxLook -> {
            val name = rename(action.name)
            if (name.isEmpty()) state
            else state.withValues(v.copy(agxLooks = v.agxLooks + AgxLook(newPresetId(v.agxLooks.map { it.id }), name,
                action.base, AgxCatalog.sanitized(action.values, action.base.defaults))))
        }
        is ApplyAgxLook -> {
            state.withValues(v.withRenderProfile(com.rawr.camera.model.RenderProfileSelection.SavedAgx(action.id)))
        }
        is RenameAgxLook -> {
            val name = rename(action.name)
            if (name.isEmpty()) state
            else state.withValues(v.copy(agxLooks = v.agxLooks.map { if (it.id == action.id) it.copy(name = name) else it }))
        }
        is DeleteAgxLook -> state.withValues(v.copy(agxLooks = v.agxLooks.filterNot { it.id == action.id }))
        is UpdateAgxLook -> if (!v.isAgxActive()) state else state.withValues(v.copy(agxLooks = v.agxLooks.map {
            if (it.id == action.id) it.copy(base = v.regularAgxPreset(), values = v.activeAgxValues()) else it
        }))
        else -> error("Unsupported AgX look action: $action")
    }
}
