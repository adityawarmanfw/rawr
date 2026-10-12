package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Shared edit rules for global settings, capture shortcuts, and independent renderer recipes. */
internal fun SettingsValues.withFilmPreset(id: String): SettingsValues {
    val preset = (FilmFactoryPresets.all + filmPresets).firstOrNull { it.id == id } ?: return this
    return copy(filmSimLook = preset.look, selectedFilmPresetId = preset.id)
}

internal fun SettingsValues.withFilmNumeric(
    action: SetFilmSimNumericValue,
    quantize: Boolean = false,
    enableGrain: Boolean = false
): SettingsValues {
    val value = if (quantize) {
        val spec = FilmSimSpecs.forParameter(action.parameter)
        val factor = Math.pow(10.0, spec.decimals.coerceIn(0, 3).toDouble()).toFloat()
        kotlin.math.round(action.value.coerceIn(spec.minimum, spec.maximum) * factor) / factor
    } else action.value
    var look = filmSimLook.withNumeric(action.copy(value = value))
    if (enableGrain && action.parameter == FilmSimNumericParameter.GrainAmount) look = look.copy(grainEnabled = true)
    return copy(filmSimLook = look)
}

internal fun SettingsValues.withFilmDiscrete(action: SetFilmSimDiscreteValue): SettingsValues =
    copy(filmSimLook = filmSimLook.withDiscrete(action))

internal fun SettingsValues.withFilmFlag(action: SetFilmSimFlag): SettingsValues =
    copy(filmSimLook = filmSimLook.withFlag(action))

internal fun SettingsValues.editActiveProfileTone(edit: (ProfileTone) -> ProfileTone): SettingsValues {
    if (isLogActive) return this
    if (regularRenderProfile() == ColorRenderProfile.SRgb) return copy(srgbTone = edit(srgbTone))
    if (regularRenderProfile() == ColorRenderProfile.Rec709) return copy(rec709Tone = edit(rec709Tone))
    // AgX skips the display tone controls; only render exposure applies.
    if (regularRenderProfile() == ColorRenderProfile.AgX)
        return copy(agxTone = ProfileTone(renderExposure = edit(agxTone).renderExposure))
    val selected = selectedLutProfile()
    return if (selected == null) copy(rawrBaseTone = edit(rawrBaseTone)) else copy(
        userLutProfiles = userLutProfiles.map { if (it.id == selected.id) it.copy(tone = edit(it.tone)) else it }
    )
}

/** Edits the active AgX preset's control; ignored unless AgX is the active profile. */
internal fun SettingsValues.withAgxValue(parameter: AgxParameter, value: Float): SettingsValues {
    if (!isAgxActive() || !value.isFinite()) return this
    val preset = regularAgxPreset()
    val next = AgxCatalog.sanitized(agxValuesFor(preset).with(parameter, value), preset.defaults)
    return copy(agxValues = agxValues + (preset to next))
}

/** Restores the active AgX preset's control to the preset default. */
internal fun SettingsValues.withAgxDefault(parameter: AgxParameter): SettingsValues =
    withAgxValue(parameter, regularAgxPreset().defaults.valueOf(parameter))

/** Commits only changed capture fields; unrelated settings edited by another screen are retained. */
internal fun SettingsValues.withCapturePreferenceChanges(
    before: com.rawr.camera.model.CaptureUiState,
    after: com.rawr.camera.model.CaptureUiState
): SettingsValues = copy(
    jpegEnabled = if (before.jpegEnabled != after.jpegEnabled) after.jpegEnabled else jpegEnabled,
    dngEnabled = if (before.dngEnabled != after.dngEnabled) after.dngEnabled else dngEnabled,
    gridMode = if (before.grid != after.grid) after.grid else gridMode,
    armedOverlays = if (before.armedOverlays != after.armedOverlays) after.armedOverlays else armedOverlays,
    falseColorManual = if (before.falseColorManual != after.falseColorManual) after.falseColorManual else falseColorManual,
    activeScopes = if (before.activeScopes != after.activeScopes) after.activeScopes else activeScopes,
    waveformMode = if (before.waveformMode != after.waveformMode) after.waveformMode else waveformMode,
    filmSimEnabled = if (before.filmSimEnabled != after.filmSimEnabled) after.filmSimEnabled else filmSimEnabled,
    experimentalMultiframeEnabled = if (before.experimentalMultiframeEnabled != after.experimentalMultiframeEnabled)
        after.experimentalMultiframeEnabled else experimentalMultiframeEnabled,
    captureControlLayout = if (before.captureLayout != after.captureLayout) after.captureLayout else captureControlLayout,
    captureModeId = if (before.captureMode != after.captureMode) after.captureMode.persistedId() else captureModeId,
    videoResolutionId = if (before.videoResolution != after.videoResolution) after.videoResolution.persistedId() else videoResolutionId,
    videoLogEnabled = if (before.videoLogEnabled != after.videoLogEnabled) after.videoLogEnabled else videoLogEnabled,
    videoFps = if (before.videoFps != after.videoFps) videoFpsOrDefault(after.videoFps) else videoFps,
    selfTimer = if (before.selfTimer != after.selfTimer) after.selfTimer else selfTimer
)

internal fun SettingsValues.capturePreferences() = com.rawr.camera.architecture.RestoreCapturePreferences(
    jpegEnabled = jpegEnabled,
    dngEnabled = dngEnabled,
    grid = gridMode,
    armedOverlays = armedOverlays,
    falseColorManual = falseColorManual,
    activeScopes = activeScopes,
    waveformMode = waveformMode,
    filmSimEnabled = filmSimEnabled,
    experimentalMultiframeEnabled = experimentalMultiframeEnabled,
    captureLayout = captureControlLayout,
    captureMode = captureModeForId(captureModeId),
    videoResolution = videoResolutionForId(videoResolutionId),
    videoFps = videoFpsOrDefault(videoFps),
    videoLogEnabled = videoLogEnabled,
    selfTimer = selfTimer
)
