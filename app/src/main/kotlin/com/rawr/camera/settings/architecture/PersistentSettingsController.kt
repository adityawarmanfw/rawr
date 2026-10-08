package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Production settings state controller.
 *
 * Persistent preferences live in [SettingsValues]. Runtime observations and the values that are
 * currently applicable to the active capability context live in [SettingsRuntimeState]. Actions
 * owned locally are acknowledged immediately, while device-backed actions may report pending or
 * applied state asynchronously without changing the UI contract.
 */
class PersistentSettingsController(
    initialState: SettingsUiState = SettingsCatalog.initialState(),
    private val defaults: SettingsValues = SettingsCatalog.initialState().values,
    private val onValuesChanged: (SettingsValues) -> Unit = {},
    private val valuesEditor: SettingsEditor? = null
) : SettingsController {
    private val mutableState = MutableStateFlow(initialState)
    override val state: StateFlow<SettingsUiState> = mutableState.asStateFlow()

    override fun dispatch(action: SettingsAction) {
        val before = mutableState.value
        fun reduce(state: SettingsUiState): SettingsUiState = when (action) {
            is SettingsPresentationAction -> SettingsReducer.reduce(state, action)
            is SettingsApplicationAction -> applyApplicationAction(state, action)
        }
        val editor = valuesEditor
        val after = if (editor != null && action is SettingsApplicationAction &&
            action !is RestorePersistentSettings && action !is ApplySettingsCapabilities
        ) {
            var result = before
            editor.update { current ->
                result = reduce(before.withValues(current))
                result.values
            }
            result
        } else reduce(before)
        mutableState.value = after
        if (editor == null && action !is RestorePersistentSettings && after.values != before.values) {
            onValuesChanged(after.values)
        }
    }

    private fun applyApplicationAction(state: SettingsUiState, action: SettingsApplicationAction): SettingsUiState =
        when (action) {
            is SetColorRenderProfile, is SelectUserLutProfile, is ImportLutStage,
            is RenameUserLutProfile, is RemoveUserLutStage, is MoveUserLutStage,
            is DeleteUserLutProfile, is SetUserLutInputGamut, is SetUserLutInputTransfer,
            is SetUserLutOutputGamut, is SetUserLutOutputTransfer, is SetUserLutAfterAction -> reduceRenderProfileSettings(state, action)

            is SetFilmSimEnabled, is SetFilmSimNumericValue,
            is SetFilmSimDiscreteValue, is SetFilmSimFlag, is SelectFilmPreset,
            is SaveFilmPreset, is RenameFilmPreset, is DeleteFilmPreset,
            is RevertFilmPreset, is UpdateFilmPreset -> reduceFilmSettings(state, action)

            is SetMultiframeChromaDenoise, is SetMultiframeBaseFrameMode, is SetMultiframeNumericValue,
            is SetMultiframeOutputResolution, is SetMultiframeMergeAlgorithm -> reduceMultiframeSettings(state, action)

            is SetPhotoDenoiseEnabled, is SetPhotoDenoiseStrength, is SetPhotoDenoiseDetail,
            is SetPhotoDenoiseLuma, is SetPhotoDenoiseScales, is SetPhotoDenoiseMethod,
            is SetVideoDenoiseEnabled, is SetVideoDenoiseStrength, is SetVideoDenoiseDetail,
            is SetVideoDenoiseLuma, is SetVideoDenoiseScales, is SetGaloshRawMode,
            is SetGaloshStrength, is SetGaloshLuma, is SetGaloshChroma,
            is SetGaloshYuvMode, is SetGaloshYuvStrengthY, is SetGaloshYuvStrengthC -> reduceDenoiseSettings(state, action)

            is SetDemosaicAlgorithm, is SetDualAutoContrast, is SetDualContrastPercent,
            is SetQuadfixEnabled, is SetQuadfixFastMedian, is SetPhotoFccSteps,
            is SetVideoFccEnabled, is SetVideoEncoder,
            is SetPhotoDefringeEnabled, is SetPhotoDefringeStrength, is SetPhotoDefringeEdgeThreshold,
            is SetPhotoDefringeLumaFloor, is SetVideoDefringeEnabled, is SetVideoDefringeStrength,
            is SetPhotoLensShadingEnabled,
            is SetVideoLensShadingEnabled, is SetDistortionCorrectionEnabled, is SetPhotoHighlightEnabled,
            is SetPhotoHighlightMethod, is SetPhotoHighlightThreshold, is SetPhotoHighlightCompression,
            is SetVideoHighlightEnabled, is SetVideoHighlightMethod, is SetVideoHighlightThreshold,
            is SetVideoHighlightCompression -> reduceImageProcessingSettings(state, action)

            is SetNumericValue, is ConfirmReset -> reduceToneSettings(state, action, defaults)

            is RestorePersistentSettings, is ApplySettingsCapabilities, is ReportOisApplication,
            is ReportAntiFlickerApplication, is ReportLocationTaggingApplication,
            is ReportImageToneApplication -> reduceRuntimeSettings(state, action, defaults)

            is SetSaveLocationTree, is SetPipelineDiagnosticsEnabled, is SetExperimentalZeroCopyEnabled,
            is SetSaveBaseDng, is SetExperimentalMultiframeEnabled, is SetUltraHdrEnabled, is SetAePriorityDisabled,
            is SetPersistentEngineEnabled, is SetCustomGpuDriverEnabled, is SetCustomGpuDriverInstalled,
            is SetPersistentDiagnosticsEnabled, is SetPersistZslRingEnabled, is SetPersistZslRingOnShutterEnabled,
            is SetInternalTraceCaptureEnabled, is SetInternalTraceRetainedRows -> reduceAppSettings(state, action)

            is SetChoice -> reduceChoiceSettings(state, action)

            is SetSelfTimer, is SetLocationTagging, is SetOisPreference,
            is SetAntiFlicker, is SetExposureStep,
            is SetHighlightProtection, is SetControlSurfaceStyle, is SetCaptureControlLayout,
            is SetGridMode, is SetViewfinderDivisor, is SetLensProfiles -> reduceCaptureSettings(state, action)

        }
}
