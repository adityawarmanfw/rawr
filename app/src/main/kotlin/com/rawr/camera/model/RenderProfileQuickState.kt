package com.rawr.camera.model

import com.rawr.camera.settings.model.AgxPreset
import com.rawr.camera.settings.model.ColorRenderProfile
import com.rawr.camera.settings.model.VideoLogProfile

sealed interface RenderProfileSelection {
    data class BuiltIn(val profile: ColorRenderProfile) : RenderProfileSelection
    data class Imported(val profileId: String) : RenderProfileSelection
    data class Log(val profile: VideoLogProfile) : RenderProfileSelection
    /** Each AgX preset is its own picker entry on the AgX render profile. */
    data class Agx(val preset: AgxPreset) : RenderProfileSelection

    val key: String get() = when (this) {
        is BuiltIn -> when (profile) {
            ColorRenderProfile.RawrBase -> "base"
            else -> "builtin_${profile.name}"
        }
        is Imported -> profileId
        is Log -> "log_${profile.name}"
        is Agx -> "agx_${preset.name}"
    }
}

data class RenderProfileQuickOption(val id: RenderProfileSelection, val label: String)

/** AgX controls for the strip's AgX sections while an AgX preset is active. */
data class AgxQuickState(
    val values: com.rawr.camera.settings.model.AgxValues,
    val defaults: com.rawr.camera.settings.model.AgxValues
) {
    val modified: Boolean get() = com.rawr.camera.settings.model.AgxCatalog.all.any {
        !it.isDefault(values.valueOf(it.parameter), defaults.valueOf(it.parameter))
    }
}

data class RenderProfileQuickState(
    val options: List<RenderProfileQuickOption> = listOf(
        RenderProfileQuickOption(RenderProfileSelection.BuiltIn(ColorRenderProfile.RawrBase), "RAWR NTRL")),
    val selectedId: RenderProfileSelection = RenderProfileSelection.BuiltIn(ColorRenderProfile.RawrBase),
    val log: Boolean = false,
    val locked: Boolean = false,
    val agx: AgxQuickState? = null
)
