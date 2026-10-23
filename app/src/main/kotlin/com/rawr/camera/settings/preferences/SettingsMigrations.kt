package com.rawr.camera.settings.preferences

import com.rawr.camera.model.ImageToneState

/** Migrates only values that actually existed in the v11 preference file. */
internal fun migrateV11ColorControls(
    tone: ImageToneState,
    defaults: ImageToneState,
    hasPersistedSaturation: Boolean,
    hasPersistedVibrance: Boolean
): ImageToneState = tone.copy(
    saturation =
        if (hasPersistedSaturation) {
            ((tone.saturation - 1f) * 100f).coerceIn(-100f, 100f)
        } else {
            defaults.saturation
        },
    vibrance =
        if (hasPersistedVibrance) {
            (tone.vibrance * 100f).coerceIn(-100f, 100f)
        } else {
            defaults.vibrance
        }
)

/** Seed the new independent video selection from the legacy shared selection. */
internal fun migrateV33RenderProfiles(values: com.rawr.camera.settings.model.SettingsValues) = values.copy(
    videoColorRenderProfile = values.colorRenderProfile,
    videoUserLutProfileId = values.selectedUserLutProfileId
)

/** Lens shading correction is on by default for photo and video; existing installs adopt it once. */
internal fun migrateV37LensShadingDefaults(values: com.rawr.camera.settings.model.SettingsValues) = values.copy(
    photoLensShadingEnabled = true,
    videoLensShadingEnabled = true
)
