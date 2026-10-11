package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Edits owned by the renderprofile settings feature. */
internal fun reduceRenderProfileSettings(
    state: SettingsUiState, action: SettingsApplicationAction
): SettingsUiState {
    val v = state.values
    if (v.isLogActive) return state
    if (action is SetColorRenderProfile && action.value !in listOf(ColorRenderProfile.RawrBase, ColorRenderProfile.AgX,
            if (v.isVideo) ColorRenderProfile.Rec709 else ColorRenderProfile.SRgb)) return state
    val editing = if (v.isVideo) state.copy(values = v.copy(
        captureModeId = "photo", colorRenderProfile = v.videoColorRenderProfile,
        selectedUserLutProfileId = v.videoUserLutProfileId
    )) else state
    val reduced = reduceRegularRenderProfile(editing, action)
    val next = if (v.isVideo) reduced.values.copy(
        captureModeId = v.captureModeId, videoColorRenderProfile = reduced.values.colorRenderProfile,
        videoUserLutProfileId = reduced.values.selectedUserLutProfileId,
        colorRenderProfile = v.colorRenderProfile, selectedUserLutProfileId = v.selectedUserLutProfileId
    ) else reduced.values
    // Removing the last stage or deleting a shared LUT invalidates either selection.
    fun usable(id: String?) = next.userLutProfiles.any { it.id == id && it.stages.isNotEmpty() }
    val invalidPhoto = next.colorRenderProfile == ColorRenderProfile.UserLut && !usable(next.selectedUserLutProfileId)
    val invalidVideo = next.videoColorRenderProfile == ColorRenderProfile.UserLut && !usable(next.videoUserLutProfileId)
    return reduced.withValues(next.copy(
        colorRenderProfile = if (invalidPhoto) ColorRenderProfile.RawrBase else next.colorRenderProfile,
        selectedUserLutProfileId = if (invalidPhoto) null else next.selectedUserLutProfileId,
        videoColorRenderProfile = if (invalidVideo) ColorRenderProfile.RawrBase else next.videoColorRenderProfile,
        videoUserLutProfileId = if (invalidVideo) null else next.videoUserLutProfileId
    ))
}

private fun reduceRegularRenderProfile(state: SettingsUiState, action: SettingsApplicationAction): SettingsUiState = when (action) {
    is SetColorRenderProfile -> {
        state.withValues(
            state.values.copy(
                colorRenderProfile = action.value,
                selectedUserLutProfileId =
                    if (action.value !=
                        ColorRenderProfile.UserLut
                    ) {
                        null
                    } else {
                        state.values.selectedUserLutProfileId
                    }
            )
        )
    }

    is SelectUserLutProfile -> {
        state.values.userLutProfiles.firstOrNull { it.id == action.profileId }?.let { profile ->
            state.withValues(
                state.values.copy(
                    colorRenderProfile = if (profile.stages.isEmpty()) ColorRenderProfile.RawrBase else ColorRenderProfile.UserLut,
                    selectedUserLutProfileId = action.profileId
                )
            )
        }
            ?: state
    }

    is ImportLutStage -> {
        val values = state.values
        val target = action.targetProfileId?.let { id -> values.userLutProfiles.firstOrNull { it.id == id } }
        if (target != null && target.stages.size < 8) {
            val updated = target.copy(stages = target.stages + action.stage)
            state.withValues(
                values.copy(
                    userLutProfiles =
                        values.userLutProfiles.map {
                            if (it.id ==
                                target.id
                            ) {
                                updated
                            } else {
                                it
                            }
                        },
                    colorRenderProfile = ColorRenderProfile.UserLut,
                    selectedUserLutProfileId = target.id
                )
            )
        } else if (action.targetProfileId == null) {
            val stem =
                action.stage.fileName
                    .substringBeforeLast('.')
                    .ifBlank { "Imported LUT" }
            val profile = ImportedLutProfile(id = action.stage.id, name = stem, stages = listOf(action.stage))
            state.withValues(
                values.copy(
                    userLutProfiles = values.userLutProfiles + profile,
                    colorRenderProfile = ColorRenderProfile.UserLut,
                    selectedUserLutProfileId = profile.id
                )
            )
        } else {
            state
        }
    }

    is RenameUserLutProfile -> {
        state.updateLutProfile(action.profileId) {
            it.copy(
                name =
                    action.name
                        .trim()
                        .take(80)
                        .ifBlank { it.name }
            )
        }
    }

    is RemoveUserLutStage -> {
        val current = state.values.userLutProfiles.firstOrNull { it.id == action.profileId }
        if (current == null) {
            state
        } else {
            val nextStages = current.stages.filterNot { it.id == action.stageId }
            val updated = state.updateLutProfile(action.profileId) { it.copy(stages = nextStages) }
            if (nextStages.isEmpty() && state.values.selectedUserLutProfileId == action.profileId) {
                updated.withValues(
                    updated.values.copy(
                        colorRenderProfile = ColorRenderProfile.RawrBase,
                        selectedUserLutProfileId = null
                    )
                )
            } else {
                updated
            }
        }
    }

    is MoveUserLutStage -> {
        state.updateLutProfile(action.profileId) { profile ->
            val list = profile.stages.toMutableList()
            val from = list.indexOfFirst { it.id == action.stageId }
            if (from < 0 || list.size < 2) {
                profile
            } else {
                val to = (from + action.delta).coerceIn(0, list.lastIndex)
                if (from != to) {
                    val item = list.removeAt(from)
                    list.add(to, item)
                }
                profile.copy(stages = list)
            }
        }
    }

    is DeleteUserLutProfile -> {
        val nextProfiles = state.values.userLutProfiles.filterNot { it.id == action.profileId }
        val deletingSelected = state.values.selectedUserLutProfileId == action.profileId
        state.withValues(
            state.values.copy(
                userLutProfiles = nextProfiles,
                colorRenderProfile = if (deletingSelected) ColorRenderProfile.RawrBase else state.values.colorRenderProfile,
                selectedUserLutProfileId = if (deletingSelected) null else state.values.selectedUserLutProfileId
            )
        )
    }

    is SetUserLutInputGamut -> {
        state.updateLutProfile(action.profileId) { it.copy(inputGamut = action.value) }
    }

    is SetUserLutInputTransfer -> {
        state.updateLutProfile(action.profileId) { it.copy(inputTransfer = action.value) }
    }

    is SetUserLutOutputGamut -> {
        state.updateLutProfile(action.profileId) { it.copy(outputGamut = action.value) }
    }

    is SetUserLutOutputTransfer -> {
        state.updateLutProfile(action.profileId) { it.copy(outputTransfer = action.value) }
    }

    is SetUserLutAfterAction -> {
        state.updateLutProfile(action.profileId) { it.copy(afterLut = action.value) }
    }
    else -> error("Unsupported renderprofile settings action: $action")
}

private fun SettingsUiState.updateLutProfile(
    profileId: String,
    transform: (ImportedLutProfile) -> ImportedLutProfile
): SettingsUiState {
    if (values.userLutProfiles.none { it.id == profileId }) return this
    val profiles = values.userLutProfiles.map { if (it.id == profileId) transform(it) else it }
    return withValues(values.copy(userLutProfiles = profiles))
}
