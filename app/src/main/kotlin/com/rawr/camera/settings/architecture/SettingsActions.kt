package com.rawr.camera.settings.architecture

import com.rawr.camera.model.CaptureControlLayout
import com.rawr.camera.model.ControlSurfaceStyle
import com.rawr.camera.model.GridMode
import com.rawr.camera.model.ImageToneState
import com.rawr.camera.settings.model.*

sealed interface SettingsAction

sealed interface SettingsPresentationAction : SettingsAction

sealed interface SettingsApplicationAction : SettingsAction

data class OpenSection(val section: SettingsSection) : SettingsPresentationAction

data object NavigateBack : SettingsPresentationAction

data class SelectImageToneTab(val tab: ImageToneTab) : SettingsPresentationAction

data class OpenSelector(val kind: ChoiceSelectorKind) : SettingsPresentationAction

data class OpenFilmSimSubPage(val section: FilmSimSection) : SettingsPresentationAction

data class OpenFilmSimDetail(val detail: FilmSimDetail) : SettingsPresentationAction

data class RequestReset(val target: ResetTarget) : SettingsPresentationAction

data object DismissReset : SettingsPresentationAction

data class OpenLensEditor(val lensName: String?) : SettingsPresentationAction

/** Replaces the user lens list; null restores the device's built-in lenses. */
data class SetLensProfiles(val lenses: List<LensProfile>?) : SettingsApplicationAction

data class SetChoice(val kind: ChoiceSelectorKind, val candidateId: String) : SettingsApplicationAction

data class SetSaveLocationTree(val treeUri: String) : SettingsApplicationAction

data class RestorePersistentSettings(val values: SettingsValues) : SettingsApplicationAction

data class ApplySettingsCapabilities(val capabilities: SettingsCapabilities) : SettingsApplicationAction

data class SetNumericValue(val parameter: ImageToneNumericParameter, val value: Float) : SettingsApplicationAction

data class SetSelfTimer(val value: SelfTimer) : SettingsApplicationAction

data class SetLocationTagging(val enabled: Boolean) : SettingsApplicationAction

data class SetOisPreference(val enabled: Boolean) : SettingsApplicationAction

data class SetAntiFlicker(val value: AntiFlicker) : SettingsApplicationAction

data class SetExposureStep(val value: ExposureStep) : SettingsApplicationAction

data class SetHighlightProtection(val value: HighlightProtection) : SettingsApplicationAction

data class SetControlSurfaceStyle(val style: ControlSurfaceStyle) : SettingsApplicationAction

data class SetCaptureControlLayout(val layout: CaptureControlLayout) : SettingsApplicationAction

data class SetGridMode(val value: GridMode) : SettingsApplicationAction

data class SetColorRenderProfile(val value: ColorRenderProfile) : SettingsApplicationAction

data class SelectUserLutProfile(val profileId: String) : SettingsApplicationAction

data class ImportLutStage(val targetProfileId: String?, val stage: ImportedLutStage) : SettingsApplicationAction

data class RenameUserLutProfile(val profileId: String, val name: String) : SettingsApplicationAction

data class RemoveUserLutStage(val profileId: String, val stageId: String) : SettingsApplicationAction

data class MoveUserLutStage(val profileId: String, val stageId: String, val delta: Int) : SettingsApplicationAction

data class DeleteUserLutProfile(val profileId: String) : SettingsApplicationAction

data class SetUserLutInputGamut(val profileId: String, val value: LutGamut) : SettingsApplicationAction

data class SetUserLutInputTransfer(val profileId: String, val value: LutTransfer) : SettingsApplicationAction

data class SetUserLutOutputGamut(val profileId: String, val value: LutGamut) : SettingsApplicationAction

data class SetUserLutOutputTransfer(val profileId: String, val value: LutTransfer) : SettingsApplicationAction

data class SetUserLutAfterAction(val profileId: String, val value: AfterLutAction) : SettingsApplicationAction

data class SetPipelineDiagnosticsEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetExperimentalZeroCopyEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetSaveBaseDng(val enabled: Boolean) : SettingsApplicationAction

data class SetExperimentalMultiframeEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetUltraHdrEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetPersistentEngineEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetFilmSimEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetFilmSimPreviewDivisor(val divisor: Int) : SettingsApplicationAction

data class SetFilmSimNumericValue(val parameter: FilmSimNumericParameter, val value: Float) :
    SettingsApplicationAction

data class SetFilmSimDiscreteValue(val field: FilmSimDiscreteField, val value: Int) : SettingsApplicationAction

data class SetFilmSimFlag(val flag: FilmSimFlag, val enabled: Boolean) : SettingsApplicationAction

data class SelectFilmPreset(val id: String) : SettingsApplicationAction

data class SaveFilmPreset(val name: String) : SettingsApplicationAction

data class RenameFilmPreset(val id: String, val name: String) : SettingsApplicationAction

data class DeleteFilmPreset(val id: String) : SettingsApplicationAction

data object RevertFilmPreset : SettingsApplicationAction

data class UpdateFilmPreset(val id: String) : SettingsApplicationAction

// Maps film actions onto FilmSimLook. Ranges mirror the native validate
// contract; out-of-range values are clamped, never rejected silently.
internal fun FilmSimLook.withNumeric(action: SetFilmSimNumericValue): FilmSimLook {
    val v = action.value
    return when (action.parameter) {
        FilmSimNumericParameter.FilmExposureEv -> copy(filmExposureEv = v)
        FilmSimNumericParameter.PrintExposureEv -> copy(printExposureEv = v)
        FilmSimNumericParameter.FilmPushPullStops -> copy(filmPushPullStops = v)
        FilmSimNumericParameter.FilmGamma -> copy(filmGamma = v)
        FilmSimNumericParameter.PrintPushPullStops -> copy(printPushPullStops = v)
        FilmSimNumericParameter.PrintGamma -> copy(printGamma = v)
        FilmSimNumericParameter.PrintShadowShape -> copy(printShadowShape = v)
        FilmSimNumericParameter.PrintHighlightShape -> copy(printHighlightShape = v)
        FilmSimNumericParameter.NegativeBleachBypassAmount -> copy(negativeBleachBypassAmount = v)
        FilmSimNumericParameter.NegativeLeucoCyanCoupling -> copy(negativeLeucoCyanCoupling = v)
        FilmSimNumericParameter.PrintBleachBypassAmount -> copy(printBleachBypassAmount = v)
        FilmSimNumericParameter.PreflashExposure -> copy(preflashExposure = v)
        FilmSimNumericParameter.PreflashMFilterShift -> copy(preflashMFilterShift = v)
        FilmSimNumericParameter.PreflashYFilterShift -> copy(preflashYFilterShift = v)
        FilmSimNumericParameter.FilterC -> copy(filterC = v)
        FilmSimNumericParameter.FilterMShift -> copy(filterMShift = v)
        FilmSimNumericParameter.FilterYShift -> copy(filterYShift = v)
        FilmSimNumericParameter.PrinterLightsR -> copy(printerLightsR = v)
        FilmSimNumericParameter.PrinterLightsG -> copy(printerLightsG = v)
        FilmSimNumericParameter.PrinterLightsB -> copy(printerLightsB = v)
        FilmSimNumericParameter.EnlargerScale -> copy(enlargerScale = v)
        FilmSimNumericParameter.EnlargerOffsetXPercent -> copy(enlargerOffsetXPercent = v)
        FilmSimNumericParameter.EnlargerOffsetYPercent -> copy(enlargerOffsetYPercent = v)
        FilmSimNumericParameter.GrainAmount -> copy(grainAmount = v)
        FilmSimNumericParameter.GrainSaturation -> copy(grainSaturation = v)
        FilmSimNumericParameter.GrainParticleAreaUm2 -> copy(grainParticleAreaUm2 = v)
        FilmSimNumericParameter.GrainParticleScaleR -> copy(grainParticleScaleR = v)
        FilmSimNumericParameter.GrainParticleScaleG -> copy(grainParticleScaleG = v)
        FilmSimNumericParameter.GrainParticleScaleB -> copy(grainParticleScaleB = v)
        FilmSimNumericParameter.GrainParticleScaleLayer0 -> copy(grainParticleScaleLayer0 = v)
        FilmSimNumericParameter.GrainParticleScaleLayer1 -> copy(grainParticleScaleLayer1 = v)
        FilmSimNumericParameter.GrainParticleScaleLayer2 -> copy(grainParticleScaleLayer2 = v)
        FilmSimNumericParameter.GrainDensityMinR -> copy(grainDensityMinR = v)
        FilmSimNumericParameter.GrainDensityMinG -> copy(grainDensityMinG = v)
        FilmSimNumericParameter.GrainDensityMinB -> copy(grainDensityMinB = v)
        FilmSimNumericParameter.GrainUniformityR -> copy(grainUniformityR = v)
        FilmSimNumericParameter.GrainUniformityG -> copy(grainUniformityG = v)
        FilmSimNumericParameter.GrainUniformityB -> copy(grainUniformityB = v)
        FilmSimNumericParameter.GrainFinalBlurUm -> copy(grainFinalBlurUm = v)
        FilmSimNumericParameter.GrainBlurDyeCloudsUm -> copy(grainBlurDyeCloudsUm = v)
        FilmSimNumericParameter.GrainMicroStructureScale -> copy(grainMicroStructureScale = v)
        FilmSimNumericParameter.GrainMicroStructureSigmaNm -> copy(grainMicroStructureSigmaNm = v)
        FilmSimNumericParameter.CameraUvCutNm -> copy(cameraUvCutNm = v)
        FilmSimNumericParameter.CameraIrCutNm -> copy(cameraIrCutNm = v)
        FilmSimNumericParameter.GrainSeed -> copy(grainSeed = v.toInt())
        FilmSimNumericParameter.DirCouplersAmount -> copy(dirCouplersAmount = v.coerceIn(0f, 1f))
        FilmSimNumericParameter.DirCouplersDiffusionUm -> copy(dirCouplersDiffusionUm = v)
        FilmSimNumericParameter.DirCouplersDiffusionTailUm -> copy(dirCouplersDiffusionTailUm = v)
        FilmSimNumericParameter.DirCouplersDiffusionTailWeight -> copy(dirCouplersDiffusionTailWeight = v)
        FilmSimNumericParameter.DirCouplersInhibitionSameLayer -> copy(dirCouplersInhibitionSameLayer = v)
        FilmSimNumericParameter.DirCouplersInhibitionInterlayer -> copy(dirCouplersInhibitionInterlayer = v)
        FilmSimNumericParameter.DirCouplersGammaSameLayerR -> copy(dirCouplersGammaSameLayerR = v)
        FilmSimNumericParameter.DirCouplersGammaSameLayerG -> copy(dirCouplersGammaSameLayerG = v)
        FilmSimNumericParameter.DirCouplersGammaSameLayerB -> copy(dirCouplersGammaSameLayerB = v)
        FilmSimNumericParameter.DirCouplersGammaRToG -> copy(dirCouplersGammaRToG = v)
        FilmSimNumericParameter.DirCouplersGammaRToB -> copy(dirCouplersGammaRToB = v)
        FilmSimNumericParameter.DirCouplersGammaGToR -> copy(dirCouplersGammaGToR = v)
        FilmSimNumericParameter.DirCouplersGammaGToB -> copy(dirCouplersGammaGToB = v)
        FilmSimNumericParameter.DirCouplersGammaBToR -> copy(dirCouplersGammaBToR = v)
        FilmSimNumericParameter.DirCouplersGammaBToG -> copy(dirCouplersGammaBToG = v)
        FilmSimNumericParameter.ScannerWhiteLevel -> copy(scannerWhiteLevel = v)
        FilmSimNumericParameter.ScannerBlackLevel -> copy(scannerBlackLevel = v)
        FilmSimNumericParameter.GlarePercent -> copy(glarePercent = v)
        FilmSimNumericParameter.GlareRoughness -> copy(glareRoughness = v)
        FilmSimNumericParameter.GlareBlur -> copy(glareBlur = v)
        FilmSimNumericParameter.ScannerMtf50LpMm -> copy(scannerMtf50LpMm = v)
        FilmSimNumericParameter.ScannerUnsharpRadiusUm -> copy(scannerUnsharpRadiusUm = v)
        FilmSimNumericParameter.ScannerUnsharpAmount -> copy(scannerUnsharpAmount = v)
        FilmSimNumericParameter.ScatterAmount -> copy(scatterAmount = v)
        FilmSimNumericParameter.ScatterScale -> copy(scatterScale = v)
        FilmSimNumericParameter.HalationAmount -> copy(halationAmount = v)
        FilmSimNumericParameter.HalationScale -> copy(halationScale = v)
        FilmSimNumericParameter.HalationStrengthR -> copy(halationStrengthR = v)
        FilmSimNumericParameter.HalationStrengthG -> copy(halationStrengthG = v)
        FilmSimNumericParameter.HalationStrengthB -> copy(halationStrengthB = v)
        FilmSimNumericParameter.HalationFirstSigmaUmR -> copy(halationFirstSigmaUmR = v)
        FilmSimNumericParameter.HalationFirstSigmaUmG -> copy(halationFirstSigmaUmG = v)
        FilmSimNumericParameter.HalationFirstSigmaUmB -> copy(halationFirstSigmaUmB = v)
        FilmSimNumericParameter.HalationBoostEv -> copy(halationBoostEv = v)
        FilmSimNumericParameter.HalationBoostRange -> copy(halationBoostRange = v)
        FilmSimNumericParameter.HalationProtectEv -> copy(halationProtectEv = v)
        FilmSimNumericParameter.CameraDiffusionStrength -> copy(cameraDiffusionStrength = v)
        FilmSimNumericParameter.CameraDiffusionSpatialScale -> copy(cameraDiffusionSpatialScale = v)
        FilmSimNumericParameter.CameraDiffusionHaloWarmth -> copy(cameraDiffusionHaloWarmth = v)
        FilmSimNumericParameter.CameraDiffusionCoreIntensity -> copy(cameraDiffusionCoreIntensity = v)
        FilmSimNumericParameter.CameraDiffusionCoreSize -> copy(cameraDiffusionCoreSize = v)
        FilmSimNumericParameter.CameraDiffusionHaloIntensity -> copy(cameraDiffusionHaloIntensity = v)
        FilmSimNumericParameter.CameraDiffusionHaloSize -> copy(cameraDiffusionHaloSize = v)
        FilmSimNumericParameter.CameraDiffusionBloomIntensity -> copy(cameraDiffusionBloomIntensity = v)
        FilmSimNumericParameter.CameraDiffusionBloomSize -> copy(cameraDiffusionBloomSize = v)
        FilmSimNumericParameter.PrintDiffusionStrength -> copy(printDiffusionStrength = v)
        FilmSimNumericParameter.PrintDiffusionSpatialScale -> copy(printDiffusionSpatialScale = v)
        FilmSimNumericParameter.PrintDiffusionHaloWarmth -> copy(printDiffusionHaloWarmth = v)
        FilmSimNumericParameter.PrintDiffusionCoreIntensity -> copy(printDiffusionCoreIntensity = v)
        FilmSimNumericParameter.PrintDiffusionCoreSize -> copy(printDiffusionCoreSize = v)
        FilmSimNumericParameter.PrintDiffusionHaloIntensity -> copy(printDiffusionHaloIntensity = v)
        FilmSimNumericParameter.PrintDiffusionHaloSize -> copy(printDiffusionHaloSize = v)
        FilmSimNumericParameter.PrintDiffusionBloomIntensity -> copy(printDiffusionBloomIntensity = v)
        FilmSimNumericParameter.PrintDiffusionBloomSize -> copy(printDiffusionBloomSize = v)
    }
}

internal fun FilmSimLook.withDiscrete(action: SetFilmSimDiscreteValue): FilmSimLook {
    val v = action.value
    val applied = when (action.field) {
        FilmSimDiscreteField.Film -> copy(film = v)
        FilmSimDiscreteField.Paper -> copy(paper = v)
        FilmSimDiscreteField.InputColorSpace -> copy(inputColorSpace = v)
        FilmSimDiscreteField.OutputColorSpace -> copy(outputColorSpace = v)
        FilmSimDiscreteField.SpectralMethod -> copy(rgbToRawMethod = v)
        FilmSimDiscreteField.PushPullMode -> copy(filmPushPullMode = v)
        FilmSimDiscreteField.FilmFormat -> copy(filmFormat = v)
        FilmSimDiscreteField.GrainSubLayerCount -> copy(grainSubLayerCount = v)
        FilmSimDiscreteField.Process -> copy(process = v)
        FilmSimDiscreteField.CameraDiffusionFamily -> copy(cameraDiffusionFamily = v)
        FilmSimDiscreteField.PrintDiffusionFamily -> copy(printDiffusionFamily = v)
        FilmSimDiscreteField.GrainModel -> copy(grainModel = v)
    }
    // Reversal (positive) stocks would print negative: route them through
    // scan presentation automatically (manual override stays available via
    // the process selector). Centralized here so Settings, capture and
    // renderer paths can never disagree.
    if (action.field != FilmSimDiscreteField.Film) return applied
    return if (v in FilmStocks.positiveFilmIndices) {
        applied.copy(process = 1, scanNegativeInvert = false)
    } else {
        applied.copy(process = 0)
    }
}

internal fun FilmSimLook.withFlag(action: SetFilmSimFlag): FilmSimLook {
    val v = action.enabled
    return when (action.flag) {
        FilmSimFlag.PrinterLightsGang -> copy(printerLightsGang = v)
        FilmSimFlag.PrinterLightCalibration -> copy(printerLightCalibration = v)
        FilmSimFlag.GrainEnabled -> copy(grainEnabled = v)
        FilmSimFlag.GrainSublayersEnabled -> copy(grainSublayersEnabled = v)
        FilmSimFlag.GrainAnimate -> copy(grainAnimate = v)
        FilmSimFlag.CameraUvEnabled -> copy(cameraUvFilterEnabled = v)
        FilmSimFlag.CameraIrEnabled -> copy(cameraIrFilterEnabled = v)
        FilmSimFlag.ScanNegativeInvert -> copy(scanNegativeInvert = v)
        FilmSimFlag.ScannerEnabled -> copy(scannerEnabled = v)
        FilmSimFlag.ScannerWhiteCorrection -> copy(scannerWhiteCorrection = v)
        FilmSimFlag.ScannerBlackCorrection -> copy(scannerBlackCorrection = v)
        FilmSimFlag.HalationEnabled -> copy(halationEnabled = v)
        FilmSimFlag.CameraDiffusionEnabled -> copy(cameraDiffusionEnabled = v)
        FilmSimFlag.PrintDiffusionEnabled -> copy(printDiffusionEnabled = v)
    }
}

data class SetMultiframeBaseFrameMode(val value: MultiframeBaseFrameMode) : SettingsApplicationAction

data class SetMultiframeChromaDenoise(val enabled: Boolean) : SettingsApplicationAction


data class SetMultiframeNumericValue(val parameter: MultiframeNumericParameter, val value: Float) :
    SettingsApplicationAction

data class SetMultiframeOutputResolution(val value: MultiframeOutputResolution) : SettingsApplicationAction

data class SetMultiframeMergeAlgorithm(val value: MultiframeMergeAlgorithm) : SettingsApplicationAction

data class SetCustomGpuDriverEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetCustomGpuDriverInstalled(val displayName: String) : SettingsApplicationAction

data class SetPersistentDiagnosticsEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetPersistZslRingEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetPersistZslRingOnShutterEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetInternalTraceCaptureEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetInternalTraceRetainedRows(val rows: Int) : SettingsApplicationAction

data class SetDemosaicAlgorithm(val value: DemosaicAlgorithm) : SettingsApplicationAction

data class SetDualAutoContrast(val enabled: Boolean) : SettingsApplicationAction

data class SetDualContrastPercent(val value: Float) : SettingsApplicationAction

data class SetQuadfixEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetQuadfixFastMedian(val enabled: Boolean) : SettingsApplicationAction

    data class SetPhotoFccSteps(val value: Int) : SettingsApplicationAction
    data class SetVideoFccEnabled(val enabled: Boolean) : SettingsApplicationAction
    data class SetVideoFccSteps(val value: Int) : SettingsApplicationAction
    data class SetVideoEncoder(val value: VideoEncoderConfig) : SettingsApplicationAction

    data class SetPhotoDefringeEnabled(val enabled: Boolean) : SettingsApplicationAction

    data class SetPhotoDefringeStrength(val value: Float) : SettingsApplicationAction

    data class SetPhotoDefringeEdgeThreshold(val value: Float) : SettingsApplicationAction

    data class SetPhotoDefringeLumaFloor(val value: Float) : SettingsApplicationAction

    data class SetVideoDefringeEnabled(val enabled: Boolean) : SettingsApplicationAction

    data class SetVideoDefringeStrength(val value: Float) : SettingsApplicationAction

    data class SetVideoDefringeEdgeThreshold(val value: Float) : SettingsApplicationAction

    data class SetVideoDefringeLumaFloor(val value: Float) : SettingsApplicationAction

    data class SetPhotoDenoiseEnabled(val enabled: Boolean) : SettingsApplicationAction

    data class SetPhotoDenoiseStrength(val value: Float) : SettingsApplicationAction

    data class SetPhotoDenoiseDetail(val value: Float) : SettingsApplicationAction

    data class SetPhotoDenoiseLuma(val value: Float) : SettingsApplicationAction

    data class SetPhotoDenoiseScales(val value: Int) : SettingsApplicationAction

    data class SetPhotoDenoiseMethod(val method: Int) : SettingsApplicationAction

    data class SetVideoDenoiseEnabled(val enabled: Boolean) : SettingsApplicationAction

    data class SetVideoDenoiseStrength(val value: Float) : SettingsApplicationAction

    data class SetVideoDenoiseDetail(val value: Float) : SettingsApplicationAction

    data class SetVideoDenoiseLuma(val value: Float) : SettingsApplicationAction

    data class SetVideoDenoiseScales(val value: Int) : SettingsApplicationAction

    data class SetGaloshRawMode(val mode: Int) : SettingsApplicationAction

    data class SetGaloshStrength(val value: Float) : SettingsApplicationAction

    data class SetGaloshLuma(val value: Float) : SettingsApplicationAction

    data class SetGaloshChroma(val value: Float) : SettingsApplicationAction

    data class SetGaloshYuvMode(val mode: Int) : SettingsApplicationAction

    data class SetGaloshYuvStrengthY(val value: Float) : SettingsApplicationAction

    data class SetGaloshYuvStrengthC(val value: Float) : SettingsApplicationAction

data class SetPhotoLensShadingEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetVideoLensShadingEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetDistortionCorrectionEnabled(val enabled: Boolean) : SettingsApplicationAction

data class SetPhotoHighlightEnabled(val enabled: Boolean) : SettingsApplicationAction
data class SetPhotoHighlightMethod(val method: Int) : SettingsApplicationAction
data class SetPhotoHighlightThreshold(val value: Float) : SettingsApplicationAction
data class SetPhotoHighlightCompression(val value: Float) : SettingsApplicationAction

data class SetVideoHighlightEnabled(val enabled: Boolean) : SettingsApplicationAction
data class SetVideoHighlightMethod(val method: Int) : SettingsApplicationAction
data class SetVideoHighlightThreshold(val value: Float) : SettingsApplicationAction
data class SetVideoHighlightCompression(val value: Float) : SettingsApplicationAction

data class ConfirmReset(val target: ResetTarget) : SettingsApplicationAction

/**
 * Backend acknowledgement/status update. The generation must match the active capability
 * context; stale callbacks from a replaced camera are ignored. These actions never mutate the
 * user's persisted preference.
 */
data class ReportOisApplication(val contextGeneration: Long, val state: SettingApplicationState<Boolean>) :
    SettingsApplicationAction

data class ReportAntiFlickerApplication(val contextGeneration: Long, val state: SettingApplicationState<AntiFlicker>) :
    SettingsApplicationAction

data class ReportLocationTaggingApplication(val contextGeneration: Long, val state: SettingApplicationState<Boolean>) :
    SettingsApplicationAction

data class ReportImageToneApplication(val contextGeneration: Long, val state: SettingApplicationState<ImageToneState>) :
    SettingsApplicationAction
