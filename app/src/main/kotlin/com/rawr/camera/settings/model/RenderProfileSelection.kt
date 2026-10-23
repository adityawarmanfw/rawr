package com.rawr.camera.settings.model

import com.rawr.camera.model.RenderProfileSelection

fun SettingsValues.withRenderProfile(selection: RenderProfileSelection): SettingsValues = when (selection) {
    is RenderProfileSelection.Log -> if (isLogActive) copy(videoLogProfile = selection.profile) else this
    is RenderProfileSelection.BuiltIn -> {
        val allowed = listOf(ColorRenderProfile.RawrBase, if (isVideo) ColorRenderProfile.Rec709 else ColorRenderProfile.SRgb)
        if (isLogActive || selection.profile !in allowed) this
        else if (isVideo) copy(videoColorRenderProfile = selection.profile, videoUserLutProfileId = null)
        else copy(colorRenderProfile = selection.profile, selectedUserLutProfileId = null)
    }
    is RenderProfileSelection.Agx -> when {
        isLogActive -> this
        isVideo -> copy(videoColorRenderProfile = ColorRenderProfile.AgX, videoAgxPreset = selection.preset,
            videoUserLutProfileId = null)
        else -> copy(colorRenderProfile = ColorRenderProfile.AgX, agxPreset = selection.preset,
            selectedUserLutProfileId = null)
    }
    is RenderProfileSelection.SavedAgx -> {
        val look = agxLooks.firstOrNull { it.id == selection.lookId }
        if (isLogActive || look == null) this
        else withRenderProfile(RenderProfileSelection.Agx(look.base))
            .copy(agxValues = agxValues + (look.base to look.values))
    }
    is RenderProfileSelection.Imported -> {
        if (isLogActive || userLutProfiles.none { it.id == selection.profileId && it.stages.isNotEmpty() }) this
        else if (isVideo) copy(videoColorRenderProfile = ColorRenderProfile.UserLut, videoUserLutProfileId = selection.profileId)
        else copy(colorRenderProfile = ColorRenderProfile.UserLut, selectedUserLutProfileId = selection.profileId)
    }
}
