package com.rawr.camera.settings.model

import com.rawr.camera.model.CaptureControlLayout
import com.rawr.camera.model.ControlSurfaceStyle
import com.rawr.camera.model.GridMode
import com.rawr.camera.model.ImageToneState
import com.rawr.camera.model.OverlayMode
import com.rawr.camera.model.ScopeType
import com.rawr.camera.model.ToneParameter
import com.rawr.camera.model.TonemapCatalog
import com.rawr.camera.model.TonemapControlContract
import com.rawr.camera.model.TonemapParam
import com.rawr.camera.model.WaveformMode
import com.rawr.camera.model.valueFor
import com.rawr.camera.model.withValue
import kotlin.math.roundToInt

enum class SettingsSection(val title: String) {
    Capture("Capture"),
    Lens("Lens"),
    Exposure("Exposure"),
    HighlightReconstruction("Highlight Reconstruction"),
    Ois("OIS"),
    Image("Image"),
    ImageTone("Tone & Color"),
    LutProfile("LUT Profile"),
    Demosaic("Demosaic"),
    Defringe("Defringe"),
    Denoise("Denoise"),
    LensShading("Lens Shading"),
    Jpeg("JPEG"),
    DisplayControls("Display & Control"),
    Dng("DNG"),
    Monitoring("Monitoring"),
    ControlStyle("Control Style"),
    Storage("Storage"),
    VideoEncoder("Video Encoder"),
    Experimental("Experimental"),
    GpuDriver("GPU Driver"),
    ZeroCopy("Zero Copy"),
    Multiframe("Multiframe"),
    FilmSim("Film Simulation"),
    Debug("Debug"),
    InternalLogging("Internal Logging"),
    About("About"),

    // Kept only for compatibility with older persisted/navigation tests; no longer exposed.
    CameraDevice("Camera / Device")
}

enum class ImageToneTab(val title: String) {
    ExposureTonality("Exposure & Tonality"),
    Color("Color"),
    Output("Output")
}

enum class ResetTarget {
    ExposureTonality,
    Color,
    Output,
    AllImageTone
}

data class ChoiceCandidate(val id: String, val label: String, val supportingText: String? = null)

data class FalseColorPresetCandidate(val id: String, val displayName: String, val description: String)

enum class ImageToneNumericParameter {
    RenderExposure,
    BlackToe,
    Shadows,
    Contrast,
    MidtonePivot,
    Highlights,
    ShoulderWhitePoint,
    Saturation,
    Vibrance,
    ColorRenderingStrength,
    WbTemperature,
    WbTint,
    JpegQuality
}

data class NumericSettingSpec(
    val parameter: ImageToneNumericParameter,
    val label: String,
    val minimum: Float,
    val maximum: Float,
    val defaultValue: Float,
    val step: Float,
    val unit: String = "",
    val decimals: Int = 0
) {
    init {
        require(maximum > minimum)
        require(step > 0f)
        require(defaultValue in minimum..maximum)
    }
}

data class CameraInformation(
    val displayName: String,
    val lensSummary: String,
    val sensorSummary: String,
    val capabilitySummary: String
)

data class SettingsCapabilities(
    val saveLocationChoices: List<ChoiceCandidate>,
    val falseColorPresets: List<FalseColorPresetCandidate>,
    val peakingSensitivityChoices: List<ChoiceCandidate>,
    val outputColorSpaces: List<ChoiceCandidate>,
    val transferFunctions: List<ChoiceCandidate>,
    val jpegChromaSubsamplingChoices: List<ChoiceCandidate>,
    val dngCompressionChoices: List<ChoiceCandidate>,
    val maxPostGainChoices: List<ChoiceCandidate>,
    val autoMinFpsChoices: List<ChoiceCandidate>,
    val oisSupported: Boolean,
    val cameraInformation: CameraInformation
) {
    init {
        require(saveLocationChoices.isNotEmpty())
        require(falseColorPresets.isNotEmpty())
        require(peakingSensitivityChoices.isNotEmpty())
        require(outputColorSpaces.isNotEmpty())
        require(transferFunctions.isNotEmpty())
        require(jpegChromaSubsamplingChoices.isNotEmpty())
        require(maxPostGainChoices.isNotEmpty())
        require(autoMinFpsChoices.isNotEmpty())
        requireUniqueIds("saveLocationChoices", saveLocationChoices.map { it.id })
        requireUniqueIds("falseColorPresets", falseColorPresets.map { it.id })
        requireUniqueIds("peakingSensitivityChoices", peakingSensitivityChoices.map { it.id })
        requireUniqueIds("outputColorSpaces", outputColorSpaces.map { it.id })
        requireUniqueIds("transferFunctions", transferFunctions.map { it.id })
        requireUniqueIds("jpegChromaSubsamplingChoices", jpegChromaSubsamplingChoices.map { it.id })
        requireUniqueIds("maxPostGainChoices", maxPostGainChoices.map { it.id })
        requireUniqueIds("autoMinFpsChoices", autoMinFpsChoices.map { it.id })
    }

    private fun requireUniqueIds(name: String, ids: List<String>) {
        require(ids.size == ids.toSet().size) { "$name must contain unique candidate IDs" }
    }
}

enum class SelfTimer(val label: String) {
    Off("Off"),
    TwoSeconds("2 s"),
    ThreeSeconds("3 s"),
    FiveSeconds("5 s"),
    TenSeconds("10 s");

    /** Whole-second delay; 0 means no delay. */
    val seconds: Int
        get() = when (this) {
            Off -> 0
            TwoSeconds -> 2
            ThreeSeconds -> 3
            FiveSeconds -> 5
            TenSeconds -> 10
        }

    /** Compact top-bar label. */
    val shortLabel: String
        get() = when (this) {
            Off -> "OFF"
            TwoSeconds -> "2s"
            ThreeSeconds -> "3s"
            FiveSeconds -> "5s"
            TenSeconds -> "10s"
        }

    companion object {
        /** Capture-screen cycle order: Off → 3s → 5s → 10s → Off. */
        fun nextCaptureOption(current: SelfTimer): SelfTimer = when (current) {
            Off -> ThreeSeconds
            ThreeSeconds -> FiveSeconds
            FiveSeconds -> TenSeconds
            TenSeconds -> Off
            // Legacy persisted value outside the cycle: re-enter at 3s.
            TwoSeconds -> ThreeSeconds
        }
    }
}

enum class AntiFlicker(val label: String) {
    Auto("Auto"),
    Hz50("50 Hz"),
    Hz60("60 Hz"),
    Off("Off")
}

enum class ExposureStep(val label: String) {
    Third("1/3 EV"),
    Half("1/2 EV")
}

enum class HighlightProtection(val label: String, val kneeSummary: String) {
    Low("Low", "Soft 2EV knee past the cap · brightest shadows, most clipping risk"),
    Normal("Normal", "1EV knee past the cap · balanced"),
    High("High", "Hard 0.5EV knee past the cap · darkest shadows, safest highlights")
}

enum class ColorRenderProfile(val label: String) {
    RawrBase("RAWR NTRL"),
    UserLut("Imported LUT"),
    SRgb("sRGB"),
    Rec709("Rec.709")
}

enum class LutGamut(val label: String, val nativeId: Int) {
    SRgbRec709("Rec.709 / sRGB", 0),
    AcesCgAp1("ACEScg / AP1", 1),
    DaVinciWideGamut("DaVinci Wide Gamut", 2),
    Rec2020("Rec.2020", 3),
    ArriWideGamut3("ARRI Wide Gamut 3", 4),
    SonySGamut3Cine("Sony S-Gamut3.Cine", 5),
    PanasonicVGamut("Panasonic V-Gamut", 6),
    FujifilmFGamutC("FUJIFILM F-Gamut C", 7)
}

enum class LutTransfer(val label: String, val nativeId: Int) {
    Linear("Linear", 0),
    SRgb("sRGB", 1),
    DaVinciIntermediate("DaVinci Intermediate", 2),
    Rec2020("Rec.2020", 3),
    Gamma22("Gamma 2.2", 4),
    Gamma24("Gamma 2.4", 5),
    LogC3("ARRI LogC3 · EI 800", 6),
    SLog3("Sony S-Log3", 7),
    VLog("Panasonic V-Log", 8),
    FLog2C("FUJIFILM F-Log2 C", 9),
    Rec709("BT.709", 10)
}

enum class AfterLutAction(val label: String) {
    UseDirectly("Use directly"),
    ConvertToJpegSrgb("Convert to JPEG sRGB")
}

data class ImportedLutStage(val id: String, val fileName: String, val relativePath: String)

/**
 * Per-render-profile TONE controls.
 *
 * These are the 9 sliders shown in the Tone group (and the capture tone
 * drawer): render exposure, blacks, shadows, contrast, midtones, highlights,
 * whites, saturation and vibrance. Each render profile — RAWR NTRL, sRGB, Rec.709, and every
 * imported LUT profile — owns one [ProfileTone]; switching profiles switches
 * tone. All other [ImageToneState] fields (WB, JPEG, output spaces, color
 * rendering strength) stay global.
 */
data class ProfileTone(
    val renderExposure: Float = TonemapControlContract.EXPOSURE_NEUTRAL_EV,
    val blacks: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val shadows: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val contrast: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val midtones: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val highlights: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val whites: Float = TonemapControlContract.TONE_UI_NEUTRAL,
    val saturation: Float = TonemapControlContract.SATURATION_NEUTRAL,
    val vibrance: Float = TonemapControlContract.VIBRANCE_NEUTRAL
) {
    companion object {
        val Neutral = ProfileTone()
    }
}

/** True for the 9 tone parameters stored per-profile; false for global image params. */
fun ImageToneNumericParameter.isPerProfileTone(): Boolean = when (this) {
    ImageToneNumericParameter.RenderExposure,
    ImageToneNumericParameter.BlackToe,
    ImageToneNumericParameter.Shadows,
    ImageToneNumericParameter.Contrast,
    ImageToneNumericParameter.MidtonePivot,
    ImageToneNumericParameter.Highlights,
    ImageToneNumericParameter.ShoulderWhitePoint,
    ImageToneNumericParameter.Saturation,
    ImageToneNumericParameter.Vibrance -> true
    ImageToneNumericParameter.ColorRenderingStrength,
    ImageToneNumericParameter.WbTemperature,
    ImageToneNumericParameter.WbTint,
    ImageToneNumericParameter.JpegQuality -> false
}

fun ProfileTone.withNumeric(parameter: ImageToneNumericParameter, value: Float): ProfileTone =
    when (parameter) {
        ImageToneNumericParameter.RenderExposure -> copy(renderExposure = value)
        ImageToneNumericParameter.BlackToe -> copy(blacks = value)
        ImageToneNumericParameter.Shadows -> copy(shadows = value)
        ImageToneNumericParameter.Contrast -> copy(contrast = value)
        ImageToneNumericParameter.MidtonePivot -> copy(midtones = value)
        ImageToneNumericParameter.Highlights -> copy(highlights = value)
        ImageToneNumericParameter.ShoulderWhitePoint -> copy(whites = value)
        ImageToneNumericParameter.Saturation -> copy(saturation = value)
        ImageToneNumericParameter.Vibrance -> copy(vibrance = value)
        else -> this
    }

data class ImportedLutProfile(
    val id: String,
    val name: String,
    val stages: List<ImportedLutStage>,
    val inputGamut: LutGamut = LutGamut.SRgbRec709,
    val inputTransfer: LutTransfer = LutTransfer.SRgb,
    val outputGamut: LutGamut = LutGamut.SRgbRec709,
    val outputTransfer: LutTransfer = LutTransfer.SRgb,
    val afterLut: AfterLutAction = AfterLutAction.UseDirectly,
    val tone: ProfileTone = ProfileTone.Neutral
)

enum class DemosaicAlgorithm(val label: String) {
    Rcd("RCD"),
    Vng4("VNG4"),
    DualRcdVng4("RCD + VNG4")
}

enum class MultiframeOutputResolution(val label: String, val outputScale: Float) {
    Native("12.5 MP", 1f),
    Mp15("≈15 MP", 1.1f),
    Mp20("≈20 MP", 1.264911f),
    Mp24("≈24 MP", 1.385641f)
}

enum class MultiframeMergeAlgorithm(val nativeId: Int, val label: String) {
    Wronski(0, "Super-resolution"),
    HdrPlus(1, "HDR+"),
    HdrPlusQuality(2, "HDR+ Quality"),
    HdrPlusBracketed(3, "HDR+ Bracketed")
}

enum class MultiframeBaseFrameMode(val nativeId: Int, val label: String) {
    Middle(0, "Middle"),
    Sharpest(1, "Sharpest")
}

enum class MultiframeNumericParameter {
    MaxFrames,
    LkIterations,
    HessianEpsilonExponent,
    KDetail,
    KDenoise,
    DThreshold,
    DTransition,
    KStretch,
    KShrink,
    RobustnessT,
    RobustnessS1,
    RobustnessS2,
    MotionThreshold,
    FlatSigma,
    DetailFloorSigma,
    ScaleBandwidthGain,
    CoverageNeffLo,
    CoverageNeffHi,
    CoverageMassLo,
    CoverageMassHi,
    FallbackChroma,
    FallbackLuma,
    HdrPlusStrength,
    HdrPlusTileSize,
    BracketEv,
    BracketFrames
}

data class MultiframeNumericSpec(
    val parameter: MultiframeNumericParameter,
    val label: String,
    val supportingText: String,
    val minimum: Float,
    val maximum: Float,
    val defaultValue: Float,
    val step: Float,
    val decimals: Int,
    val unit: String = ""
) {
    fun accepts(value: Float): Boolean = value.isFinite() && value in minimum..maximum
}

object MultiframeSpecs {
    val maxFrames =
        MultiframeNumericSpec(
            MultiframeNumericParameter.MaxFrames,
            "Maximum Frames",
            "Lower: faster but noisier. Higher: cleaner flats yet slightly softer edges, slower with more motion risk.",
            2f,
            30f,
            16f,
            1f,
            0
        )
    val lkIterations =
        MultiframeNumericSpec(
            MultiframeNumericParameter.LkIterations,
            "LK Iterations",
            "Lower: faster with less subpixel refinement. Higher: refines alignment further at greater GPU cost.",
            2f,
            5f,
            5f,
            1f,
            0
        )
    val hessianEpsilonExponent =
        MultiframeNumericSpec(
            MultiframeNumericParameter.HessianEpsilonExponent,
            "Weak-texture Cutoff",
            "Lower: accepts weaker texture but risks unstable flow. Higher: rejects ambiguous tiles and falls back more often.",
            -12f,
            -7f,
            -10f,
            1f,
            0
        )
    val kDetail =
        MultiframeNumericSpec(
            MultiframeNumericParameter.KDetail,
            "Detail Kernel",
            "Lower: sharper, narrower detail with more ringing risk. Higher: safer, broader reconstruction that becomes softer.",
            .08f,
            .33f,
            .15f,
            .01f,
            2
        )
    val kDenoise =
        MultiframeNumericSpec(
            MultiframeNumericParameter.KDenoise,
            "Denoise Strength",
            "Lower: noisier flats with more texture. Higher: cleaner flats; strong edges stay sharp. Ignored while Flat Sigma is 0 or above (decoupled mode); set Flat Sigma to -1 to re-couple denoise strength to this knob.",
            1f,
            8f,
            5f,
            .25f,
            2
        )
    val dThreshold =
        MultiframeNumericSpec(
            MultiframeNumericParameter.DThreshold,
            "Detail Threshold",
            "Lower: treats more regions as detail. Higher: moves more regions toward denoising.",
            .2f,
            1f,
            .71f,
            .01f,
            2
        )
    val dTransition =
        MultiframeNumericSpec(
            MultiframeNumericParameter.DTransition,
            "Detail Transition",
            "Lower: makes a steeper, more sensitive detail/denoise switch. Higher: blends the regimes over a wider range.",
            .3f,
            2f,
            1f,
            .1f,
            1
        )
    val kStretch =
        MultiframeNumericSpec(
            MultiframeNumericParameter.KStretch,
            "Along-edge Stretch",
            "Lower: less smoothing along edges. Higher: steadier edge continuity, with more risk of smearing along structure.",
            1f,
            6f,
            2f,
            .25f,
            2
        )
    val kShrink =
        MultiframeNumericSpec(
            MultiframeNumericParameter.KShrink,
            "Across-edge Shrink",
            "Lower: wider, smoother sampling across edges. Higher: crisper edges with more shimmer risk on fine structure. With Detail Floor at 0 every step bites; raise the floor to make higher shrink values safe.",
            1f,
            10f,
            8f,
            .25f,
            2
        )
    val robustnessT =
        MultiframeNumericSpec(
            MultiframeNumericParameter.RobustnessT,
            "Photometric Threshold",
            "Lower: rejects more differing samples, reducing ghosts but retaining noise. Higher: accepts more, with greater ghost risk.",
            .05f,
            .25f,
            .08f,
            .01f,
            2
        )
    val robustnessS1 =
        MultiframeNumericSpec(
            MultiframeNumericParameter.RobustnessS1,
            "Robustness Scale S1",
            "Lower: stricter rejection in moving, low-variance areas. Higher: accepts more variation in those areas.",
            1f,
            4f,
            1.5f,
            .25f,
            2
        )
    val robustnessS2 =
        MultiframeNumericSpec(
            MultiframeNumericParameter.RobustnessS2,
            "Robustness Scale S2",
            "Lower: stricter rejection in noisy, high-variance areas. Higher: accepts more variation there.",
            6f,
            24f,
            9f,
            1f,
            0
        )
    val motionThreshold =
        MultiframeNumericSpec(
            MultiframeNumericParameter.MotionThreshold,
            "Motion Threshold",
            "Lower: detects and rejects local motion sooner. Higher: tolerates more flow variation, with greater ghost risk.",
            .5f,
            1f,
            .65f,
            .05f,
            2
        )
    // Bandwidth/coverage knobs. flatSigma -1 = legacy coupled
    // (kDetail*kDenoise); >= 0 sets the flat-region sigma independently.
    // The floor never narrows kernels; scale gain grows it above 1x only.
    val flatSigma =
        MultiframeNumericSpec(
            MultiframeNumericParameter.FlatSigma,
            "Flat Sigma",
            "Independent flat-region kernel sigma. Lower: more texture and noise in flats. Higher: calmer flats; edges keep their own narrow kernels. -1 keeps the legacy kDetail x kDenoise coupling.",
            -1f,
            2f,
            1f,
            .05f,
            2
        )
    val detailFloorSigma =
        MultiframeNumericSpec(
            MultiframeNumericParameter.DetailFloorSigma,
            "Detail Floor",
            "Minimum edge-kernel sigma in input pixels. Lower: narrower kernels with more shimmer risk on fine structure. Higher: safer, broader sampling that becomes softer. Protective at 2x via Scale Gain.",
            0f,
            .25f,
            .15f,
            .01f,
            2
        )
    val scaleBandwidthGain =
        MultiframeNumericSpec(
            MultiframeNumericParameter.ScaleBandwidthGain,
            "Scale Gain",
            "Grows the detail floor with output scale: floor x (1 + gain x (scale-1)). Lower: scale-agnostic kernels, sharper 2x with more shimmer risk. Higher: safer 2x upscales while leaving 1x untouched.",
            0f,
            2f,
            1f,
            .1f,
            1
        )
    val coverageNeffLo =
        MultiframeNumericSpec(
            MultiframeNumericParameter.CoverageNeffLo,
            "Coverage Neff Lo",
            "Effective-frame-count gate start for the bounded-reference fallback. Lower: trusts the merge sooner, with more smear risk in weak support. Higher: falls back to the stable reconstruction sooner.",
            0f,
            6f,
            1f,
            .25f,
            2
        )
    val coverageNeffHi =
        MultiframeNumericSpec(
            MultiframeNumericParameter.CoverageNeffHi,
            "Coverage Neff Hi",
            "Effective-frame-count gate end. Lower: blends toward the merge sooner. Higher: demands stronger frame support before fully trusting the merge.",
            0f,
            6f,
            3f,
            .25f,
            2
        )
    val coverageMassLo =
        MultiframeNumericSpec(
            MultiframeNumericParameter.CoverageMassLo,
            "Coverage Mass Lo",
            "Gaussian phase-mass gate start. Lower: trusts weak phase sampling sooner, with more artifact risk. Higher: falls back sooner when sampling is poor.",
            0f,
            .1f,
            .01f,
            .001f,
            3
        )
    val coverageMassHi =
        MultiframeNumericSpec(
            MultiframeNumericParameter.CoverageMassHi,
            "Coverage Mass Hi",
            "Gaussian phase-mass gate end. Lower: accepts sparser sampling. Higher: demands adequate sampling before trusting the narrow kernel.",
            0f,
            .1f,
            .04f,
            .001f,
            3
        )
    val fallbackChroma =
        MultiframeNumericSpec(
            MultiframeNumericParameter.FallbackChroma,
            "Fallback Chroma Cleanup",
            "Chroma smoothing where the merge fell back to fewer frames (motion, rejection), scaled per pixel by the missing frames. Lower: fallback areas keep more single-frame colour noise (0 = off). Higher: cleaner colour in moving areas; edges stay guarded by the burst noise level.",
            0f,
            8f,
            4f,
            .25f,
            2
        )
    val fallbackLuma =
        MultiframeNumericSpec(
            MultiframeNumericParameter.FallbackLuma,
            "Fallback Luma Cleanup",
            "Luma smoothing where motion checks rejected frames. Lower: moving areas keep more single-frame grain (0 = off). Higher: moving areas read as smooth motion blur; static edges are never touched.",
            0f,
            8f,
            2f,
            .25f,
            2
        )

    val hdrPlusStrength =
        MultiframeNumericSpec(
            MultiframeNumericParameter.HdrPlusStrength,
            "HDR+ Strength",
            "Lower: keeps more single-frame grain and rejects motion sooner. Higher: cleaner, but moving areas risk ghosting.",
            1f,
            22f,
            13f,
            1f,
            0
        )

    val hdrPlusTileSize =
        MultiframeNumericSpec(
            MultiframeNumericParameter.HdrPlusTileSize,
            "HDR+ Tile Size",
            "Lower: 16 px follows small moving parts (wind, hands) with less ghosting, slightly noisier at very high ISO. Higher: 32 px aligns more steadily in heavy noise.",
            16f,
            32f,
            32f,
            16f,
            0,
            "px"
        )

    val bracketEv =
        MultiframeNumericSpec(
            MultiframeNumericParameter.BracketEv,
            "Bracket Exposure",
            "Exposure of the extra dark frames taken after the shutter, relative to the burst. Lower: recovers brighter highlights (sun, lamps) but the dark frames are noisier. Higher: cleaner merge, recovers only mildly clipped areas.",
            -4f,
            -1f,
            -2f,
            .5f,
            1,
            "EV"
        )

    val bracketFrames =
        MultiframeNumericSpec(
            MultiframeNumericParameter.BracketFrames,
            "Bracket Frames",
            "Dark frames captured after the shutter. Lower: shorter wait after the shutter. Higher: cleaner recovered highlights.",
            1f,
            4f,
            2f,
            1f,
            0
        )

    val all =
        listOf(
            maxFrames,
            lkIterations,
            hessianEpsilonExponent,
            kDetail,
            kDenoise,
            dThreshold,
            dTransition,
            kStretch,
            kShrink,
            robustnessT,
            robustnessS1,
            robustnessS2,
            motionThreshold,
            flatSigma,
            detailFloorSigma,
            scaleBandwidthGain,
            coverageNeffLo,
            coverageNeffHi,
            coverageMassLo,
            coverageMassHi,
            fallbackChroma,
            fallbackLuma,
            hdrPlusStrength,
            hdrPlusTileSize,
            bracketEv,
            bracketFrames
        )

    fun forParameter(parameter: MultiframeNumericParameter): MultiframeNumericSpec =
        all.first { it.parameter == parameter }
}

data class MultiframeTuning(
    val outputResolution: MultiframeOutputResolution = MultiframeOutputResolution.Native,
    val mergeAlgorithm: MultiframeMergeAlgorithm = MultiframeMergeAlgorithm.HdrPlus,
    val hdrPlusStrength: Float = 13f,
    val hdrPlusTileSize: Int = 32,
    val bracketEv: Float = -2f,
    val bracketFrames: Int = 2,
    val maxFrames: Int = 16,
    val lkIterations: Int = 5,
    val hessianEpsilonExponent: Int = -10,
    val kDetail: Float = .15f,
    val kDenoise: Float = 5f,
    val dThreshold: Float = .71f,
    val dTransition: Float = 1f,
    val kStretch: Float = 2f,
    val kShrink: Float = 8f,
    val robustnessT: Float = .08f,
    val robustnessS1: Float = 1.5f,
    val robustnessS2: Float = 9f,
    val motionThreshold: Float = .65f,
    val flatSigma: Float = 1f,
    val detailFloorSigma: Float = .15f,
    val scaleBandwidthGain: Float = 1f,
    val coverageNeffLo: Float = 1f,
    val coverageNeffHi: Float = 3f,
    val coverageMassLo: Float = .01f,
    val coverageMassHi: Float = .04f,
    val fallbackChroma: Float = 4f,
    val fallbackLuma: Float = 2f
) {
    fun value(parameter: MultiframeNumericParameter): Float = when (parameter) {
        MultiframeNumericParameter.MaxFrames -> maxFrames.toFloat()
        MultiframeNumericParameter.LkIterations -> lkIterations.toFloat()
        MultiframeNumericParameter.HessianEpsilonExponent -> hessianEpsilonExponent.toFloat()
        MultiframeNumericParameter.KDetail -> kDetail
        MultiframeNumericParameter.KDenoise -> kDenoise
        MultiframeNumericParameter.DThreshold -> dThreshold
        MultiframeNumericParameter.DTransition -> dTransition
        MultiframeNumericParameter.KStretch -> kStretch
        MultiframeNumericParameter.KShrink -> kShrink
        MultiframeNumericParameter.RobustnessT -> robustnessT
        MultiframeNumericParameter.RobustnessS1 -> robustnessS1
        MultiframeNumericParameter.RobustnessS2 -> robustnessS2
        MultiframeNumericParameter.MotionThreshold -> motionThreshold
        MultiframeNumericParameter.FlatSigma -> flatSigma
        MultiframeNumericParameter.DetailFloorSigma -> detailFloorSigma
        MultiframeNumericParameter.ScaleBandwidthGain -> scaleBandwidthGain
        MultiframeNumericParameter.CoverageNeffLo -> coverageNeffLo
        MultiframeNumericParameter.CoverageNeffHi -> coverageNeffHi
        MultiframeNumericParameter.CoverageMassLo -> coverageMassLo
        MultiframeNumericParameter.CoverageMassHi -> coverageMassHi
        MultiframeNumericParameter.FallbackChroma -> fallbackChroma
        MultiframeNumericParameter.FallbackLuma -> fallbackLuma
        MultiframeNumericParameter.HdrPlusStrength -> hdrPlusStrength
        MultiframeNumericParameter.HdrPlusTileSize -> hdrPlusTileSize.toFloat()
        MultiframeNumericParameter.BracketEv -> bracketEv
        MultiframeNumericParameter.BracketFrames -> bracketFrames.toFloat()
    }

    fun withValue(parameter: MultiframeNumericParameter, value: Float): MultiframeTuning = when (parameter) {
        MultiframeNumericParameter.MaxFrames -> copy(maxFrames = value.roundToInt())
        MultiframeNumericParameter.LkIterations -> copy(lkIterations = value.roundToInt())
        MultiframeNumericParameter.HessianEpsilonExponent -> copy(hessianEpsilonExponent = value.roundToInt())
        MultiframeNumericParameter.KDetail -> copy(kDetail = value)
        MultiframeNumericParameter.KDenoise -> copy(kDenoise = value)
        MultiframeNumericParameter.DThreshold -> copy(dThreshold = value)
        MultiframeNumericParameter.DTransition -> copy(dTransition = value)
        MultiframeNumericParameter.KStretch -> copy(kStretch = value)
        MultiframeNumericParameter.KShrink -> copy(kShrink = value)
        MultiframeNumericParameter.RobustnessT -> copy(robustnessT = value)
        MultiframeNumericParameter.RobustnessS1 -> copy(robustnessS1 = value)
        MultiframeNumericParameter.RobustnessS2 -> copy(robustnessS2 = value)
        MultiframeNumericParameter.MotionThreshold -> copy(motionThreshold = value)
        MultiframeNumericParameter.FlatSigma -> copy(flatSigma = value)
        MultiframeNumericParameter.DetailFloorSigma -> copy(detailFloorSigma = value)
        MultiframeNumericParameter.ScaleBandwidthGain -> copy(scaleBandwidthGain = value)
        MultiframeNumericParameter.CoverageNeffLo -> copy(coverageNeffLo = value)
        MultiframeNumericParameter.CoverageNeffHi -> copy(coverageNeffHi = value)
        MultiframeNumericParameter.CoverageMassLo -> copy(coverageMassLo = value)
        MultiframeNumericParameter.CoverageMassHi -> copy(coverageMassHi = value)
        MultiframeNumericParameter.FallbackChroma -> copy(fallbackChroma = value)
        MultiframeNumericParameter.FallbackLuma -> copy(fallbackLuma = value)
        MultiframeNumericParameter.HdrPlusStrength -> copy(hdrPlusStrength = value.roundToInt().toFloat())
        MultiframeNumericParameter.HdrPlusTileSize -> copy(hdrPlusTileSize = if (value < 24f) 16 else 32)
        MultiframeNumericParameter.BracketEv -> copy(bracketEv = (value * 2f).roundToInt() / 2f)
        MultiframeNumericParameter.BracketFrames -> copy(bracketFrames = value.roundToInt())
    }

    fun sanitized(defaults: MultiframeTuning = MultiframeTuning()): MultiframeTuning {
        var clean =
            copy(
                outputResolution = outputResolution
            )
        for (spec in MultiframeSpecs.all) {
            val candidate = clean.value(spec.parameter)
            clean =
                clean.withValue(
                    spec.parameter,
                    if (spec.accepts(candidate)) candidate else defaults.value(spec.parameter)
                )
        }
        return clean
    }

    fun nativeValues(): FloatArray = floatArrayOf(
        outputResolution.outputScale,
        lkIterations.toFloat(),
        Math.pow(10.0, hessianEpsilonExponent.toDouble()).toFloat(),
        kDetail,
        kDenoise,
        dThreshold,
        dTransition,
        kStretch,
        kShrink,
        flatSigma,
        detailFloorSigma,
        scaleBandwidthGain,
        coverageNeffLo,
        coverageNeffHi,
        coverageMassLo,
        coverageMassHi,
        robustnessT,
        robustnessS1,
        robustnessS2,
        motionThreshold,
        fallbackChroma,
        fallbackLuma
    )

    /** ZSL frames to freeze at the shutter; HDR+ Bracketed leaves room for its post-shutter dark frames. */
    fun zslFrames(): Int =
        if (mergeAlgorithm == MultiframeMergeAlgorithm.HdrPlusBracketed) (maxFrames - bracketFrames).coerceAtLeast(2)
        else maxFrames

    /** Dark frames HDR+ Bracketed requests after the shutter (0 for other merges). */
    fun postShutterFrames(): Int =
        if (mergeAlgorithm == MultiframeMergeAlgorithm.HdrPlusBracketed) maxFrames - zslFrames() else 0

    // Appended after the multiframe chroma-denoise flag (native indices 23-27).
    fun nativeMergeValues(): FloatArray =
        floatArrayOf(
            mergeAlgorithm.nativeId.toFloat(),
            hdrPlusStrength,
            hdrPlusTileSize.toFloat(),
            bracketEv,
            bracketFrames.toFloat()
        )
}

data class SettingsValues(
    val saveLocationId: String,
    val falseColorPresetId: String,
    val peakingSensitivityId: String,
    val imageTone: ImageToneState,
    val selfTimer: SelfTimer = SelfTimer.Off,
    val locationTagging: Boolean = false,
    val oisEnabledPreference: Boolean = true,
    /** User lens configuration in capture-screen order; null = this device's built-in lenses. */
    val lensProfiles: List<LensProfile>? = null,
    /** Lens in use when the app last switched lenses; null = the profile's first lens. Shared by photo and video. */
    val lastLensId: String? = null,
    val antiFlicker: AntiFlicker = AntiFlicker.Auto,
    val exposureStep: ExposureStep = ExposureStep.Third,
    val highlightProtection: HighlightProtection = HighlightProtection.High,
    val maxPostGainId: String = "gain.200",
    val autoMinFpsId: String = "fps.12",
    val controlSurfaceStyle: ControlSurfaceStyle = ControlSurfaceStyle.Basic,
    val captureControlLayout: CaptureControlLayout = CaptureControlLayout.Compact,
    val jpegEnabled: Boolean = true,
    val dngEnabled: Boolean = true,
    val dngCompressionId: String = "dng.uncompressed",
    val gridMode: GridMode = GridMode.Thirds,
    // Preview-only look-stage (tonemap or film) downsample divisor, 1..4.
    // Film always runs at 2 or more. Never touches stills, video, or EXIF.
    val viewfinderDivisor: Int = 2,
    val armedOverlays: Set<OverlayMode> =
        setOf(OverlayMode.Peaking, OverlayMode.TonemapShadows, OverlayMode.RawHighlights),
    val falseColorManual: Boolean = false,
    val activeScopes: List<ScopeType> = listOf(ScopeType.Waveform),
    val waveformMode: WaveformMode = WaveformMode.RgbOverlay,
    val colorRenderProfile: ColorRenderProfile = ColorRenderProfile.RawrBase,
    val selectedUserLutProfileId: String? = null,
    val userLutProfiles: List<ImportedLutProfile> = emptyList(),
    /**
     * RAWR NTRL's own TONE set. Each [ImportedLutProfile] carries its own
     * [ProfileTone]; this is the regular profile's counterpart. The 9 tone
     * fields inside [imageTone] are legacy storage only — the applied tone is
     * always [activeImageTone].
     */
    val rawrBaseTone: ProfileTone = ProfileTone.Neutral,
    val srgbTone: ProfileTone = ProfileTone.Neutral,
    val rec709Tone: ProfileTone = ProfileTone.Neutral,
    val videoColorRenderProfile: ColorRenderProfile = ColorRenderProfile.RawrBase,
    val videoUserLutProfileId: String? = null,
    val videoLogEnabled: Boolean = false,
    val videoLogProfile: VideoLogProfile = VideoLogProfile.LogC3,
    val pipelineDiagnosticsEnabled: Boolean = false,
    val experimentalZeroCopyEnabled: Boolean = true,
    val experimentalMultiframeEnabled: Boolean = false,
    val saveBaseDng: Boolean = true,
    val persistentEngineEnabled: Boolean = true,
    val filmSimEnabled: Boolean = false,
    val filmSimLook: FilmSimLook = FilmFactoryPresets.baseLook,
    val selectedFilmPresetId: String? = null,
    val filmPresets: List<FilmPreset> = emptyList(),
    val multiframeBaseFrameMode: MultiframeBaseFrameMode = MultiframeBaseFrameMode.Middle,
    // Chroma-only profiled wavelet on the merged image, fed the burst-fitted
    // noise / frame count. Independent of Image > Denoise (single frame only).
    val multiframeChromaDenoise: Boolean = true,
    val multiframeTuning: MultiframeTuning = MultiframeTuning(),
    val customGpuDriverEnabled: Boolean = false,
    val customGpuDriverName: String? = null,
    val persistentDiagnosticsEnabled: Boolean = false,
    val persistZslRingEnabled: Boolean = false,
    val persistZslRingOnShutterEnabled: Boolean = false,
    val internalTraceCaptureEnabled: Boolean = false,
    val internalTraceRetainedRows: Int = 8192,
    val demosaicAlgorithm: DemosaicAlgorithm = DemosaicAlgorithm.Rcd,
    val dualAutoContrast: Boolean = true,
    val dualContrastPercent: Float = 20f,
    val quadfixEnabled: Boolean = false,
    val quadfixFastMedian: Boolean = false,
    // Photo FCC steps (stills). Video has its own enable + steps below.
    val photoFccSteps: Int = 2,
    val videoFccEnabled: Boolean = false,
    val videoFccSteps: Int = 1,
    // Photo defringe (stills). Video strengths are independent.
    val photoDefringeEnabled: Boolean = true,
    val photoDefringeStrength: Float = 1f,
    val photoDefringeEdgeThreshold: Float = 0.02f,
    val photoDefringeLumaFloor: Float = 0.08f,
    val videoDefringeEnabled: Boolean = false,
    val videoDefringeStrength: Float = 1f,
    val videoDefringeEdgeThreshold: Float = 0.02f,
    val videoDefringeLumaFloor: Float = 0.08f,
    // Photo-pipeline denoise, single source of truth (see DenoiseConfig).
    // Off by default; Wavelet is profiled (sensor noise model), Galosh
    // carries the blind RAW and/or YUV lanes (stills only).
    val photoDenoise: DenoiseConfig = DenoiseConfig.Off,
    // Video denoise is wavelet-only primitives (no Galosh on video).
    val videoDenoiseEnabled: Boolean = false,
    val videoDenoiseStrength: Float = 1f,
    val videoDenoiseDetail: Float = 1f,
    val videoDenoiseLuma: Float = 0.25f,
    val videoDenoiseScales: Int = 7,
    // Photo lens shading (preview + JPEG stills). Video is independent.
    val photoLensShadingEnabled: Boolean = false,
    val videoLensShadingEnabled: Boolean = false,
    val distortionCorrectionEnabled: Boolean = false,
    // Photo highlight reconstruction (stills; live preview uses color
    // propagation). Video has its own method + controls below.
    val photoHighlightEnabled: Boolean = true,
    // 0 = existing propagation; 1 = RawTherapee Coloropp for still renders.
    val photoHighlightMethod: Int = 1,
    val photoHighlightThreshold: Float = 1f,
    val photoHighlightCompression: Float = 163f,
    val videoHighlightEnabled: Boolean = false,
    val videoHighlightMethod: Int = 0,
    val videoHighlightThreshold: Float = 1f,
    val videoHighlightCompression: Float = 163f,
    // UltraHDR (JPEG_R) still output: GPU gain map + MPF/XMP/ISO mux.
    // Off by default; legacy SDR JPEG when off.
    val ultraHdrEnabled: Boolean = false,
    // Photo/Video capture mode + video recording selectors. Plain primitives
    // keep the DataStore schema stable; validated through the helpers below.
    val captureModeId: String = "photo",
    val videoResolutionId: String = "1080p",
    val videoFps: Int = 30,
    val videoEncoder: VideoEncoderConfig = VideoEncoderConfig()
) {
    val isVideo: Boolean get() = captureModeId == "video"
    val isLogActive: Boolean get() = isVideo && videoLogEnabled
    fun regularRenderProfile(): ColorRenderProfile = if (isVideo) videoColorRenderProfile else colorRenderProfile
    fun regularLutProfileId(): String? = if (isVideo) videoUserLutProfileId else selectedUserLutProfileId
    fun effectiveVideoBitDepth(): Int = if (videoLogEnabled) 10 else videoEncoder.sanitized().bitDepth
    fun effectiveRenderProfileId(): Int = if (isLogActive) videoLogProfile.nativeId else regularRenderProfile().ordinal

    fun captureRendererDisplayName(): String = activeProfileLabel()

    fun selectedLutProfile(): ImportedLutProfile? =
        if (regularRenderProfile() == ColorRenderProfile.UserLut)
            userLutProfiles.firstOrNull { it.id == regularLutProfileId() }
        else null

    /** Tone edits must not rebuild immutable LUT resources. */
    fun renderProfileSyncKey(): Triple<Int, String?, ImportedLutProfile?> = Triple(
        effectiveRenderProfileId(),
        regularLutProfileId().takeUnless { isLogActive },
        selectedLutProfile()?.copy(tone = ProfileTone.Neutral).takeUnless { isLogActive }
    )

    fun activeProfileTone(): ProfileTone = when {
        isLogActive -> ProfileTone.Neutral
        regularRenderProfile() == ColorRenderProfile.SRgb -> srgbTone
        regularRenderProfile() == ColorRenderProfile.Rec709 -> rec709Tone
        else -> selectedLutProfile()?.tone ?: rawrBaseTone
    }

    fun activeImageTone(): ImageToneState = TonemapCatalog.all.fold(imageTone) { acc, descriptor ->
        acc.withValue(descriptor, activeProfileTone().valueFor(descriptor))
    }

    fun activeProfileLabel(): String = if (isLogActive) videoLogProfile.label
        else selectedLutProfile()?.name?.takeIf { it.isNotBlank() } ?: regularRenderProfile().label

    /** Copy of [ProfileTone] derived from the legacy global tone (v22 migration source). */
    fun profileToneFromLegacyImageTone(): ProfileTone = ProfileTone(
        renderExposure = imageTone.renderExposure,
        blacks = imageTone.blacks,
        shadows = imageTone.shadows,
        contrast = imageTone.contrast,
        midtones = imageTone.midtones,
        highlights = imageTone.highlights,
        whites = imageTone.whites,
        saturation = imageTone.saturation,
        vibrance = imageTone.vibrance
    )
}

sealed interface SettingsDestination {
    data object Home : SettingsDestination

    data class Section(val section: SettingsSection) : SettingsDestination

    data class ChoiceSelector(val kind: ChoiceSelectorKind) : SettingsDestination

    data class FilmSimSubPage(val section: FilmSimSection) : SettingsDestination

    data class FilmSimDetailPage(val detail: FilmSimDetail) : SettingsDestination

    /** Lens profile editor; [lensName] null creates a new lens. */
    data class LensEditor(val lensName: String?) : SettingsDestination
}

enum class ChoiceSelectorKind {
    SaveLocation,
    FalseColorPreset,
    PeakingSensitivity,
    OutputColorSpace,
    TransferFunction,
    JpegChromaSubsampling,
    DngCompression,
    MaxPostGain,
    AutoMinFps,
    FilmOutputSpace
}

data class SettingsPresentationState(
    val destination: SettingsDestination = SettingsDestination.Home,
    // Temporal back history (most-recent-last). NavigateBack pops; empty
    // falls back to the logical-parent mapping in SettingsReducer.
    val backStack: List<SettingsDestination> = emptyList(),
    val imageToneTab: ImageToneTab = ImageToneTab.ExposureTonality,
    val pendingReset: ResetTarget? = null
)

data class SettingsEffectiveState(
    val saveLocationId: String? = null,
    val falseColorPresetId: String? = null,
    val peakingSensitivityId: String? = null,
    val outputColorSpaceId: String? = null,
    val transferFunctionId: String? = null,
    val jpegChromaSubsamplingId: String? = null,
    val maxPostGainId: String? = null,
    val autoMinFpsId: String? = null
)

enum class SettingApplicationStatus { Applied, Pending, Failed, Unsupported }

data class SettingApplicationState<T>(
    val effectiveValue: T? = null,
    val status: SettingApplicationStatus = SettingApplicationStatus.Pending
)

data class SettingsApplicationState(
    val ois: SettingApplicationState<Boolean> = SettingApplicationState(),
    val antiFlicker: SettingApplicationState<AntiFlicker> = SettingApplicationState(),
    val locationTagging: SettingApplicationState<Boolean> = SettingApplicationState(),
    val imageTone: SettingApplicationState<ImageToneState> = SettingApplicationState()
)

data class SettingsRuntimeState(
    val effective: SettingsEffectiveState = SettingsEffectiveState(),
    val application: SettingsApplicationState = SettingsApplicationState(),
    /**
     * Monotonic identity for the currently active camera/capability application context.
     * Backend acknowledgements must carry this generation so late results from a replaced
     * camera cannot repopulate effective application state.
     */
    val capabilityContextGeneration: Long = 0L
) {
    init {
        require(capabilityContextGeneration >= 0L)
    }
}

data class SettingsUiState(
    val capabilities: SettingsCapabilities,
    val values: SettingsValues,
    val runtime: SettingsRuntimeState = SettingsRuntimeState(),
    val presentation: SettingsPresentationState = SettingsPresentationState()
)

fun SettingsCapabilities.resolveEffective(values: SettingsValues): SettingsEffectiveState = SettingsEffectiveState(
    saveLocationId =
        values.saveLocationId.takeIf { id ->
            id.startsWith("storage.tree:") ||
                saveLocationChoices.any { it.id == id }
        },
    falseColorPresetId = values.falseColorPresetId.takeIf { id -> falseColorPresets.any { it.id == id } },
    peakingSensitivityId =
        values.peakingSensitivityId.takeIf { id ->
            peakingSensitivityChoices.any { it.id == id }
        },
    outputColorSpaceId = values.imageTone.outputColorSpaceId.takeIf { id ->
        outputColorSpaces.any { it.id == id }
    },
    transferFunctionId = values.imageTone.transferFunctionId.takeIf { id -> transferFunctions.any { it.id == id } },
    jpegChromaSubsamplingId =
        values.imageTone.jpegChromaSubsamplingId.takeIf { id ->
            jpegChromaSubsamplingChoices.any {
                it.id ==
                    id
            }
        },
    maxPostGainId = values.maxPostGainId.takeIf { id -> maxPostGainChoices.any { it.id == id } },
    autoMinFpsId = values.autoMinFpsId.takeIf { id -> autoMinFpsChoices.any { it.id == id } }
)

object ImageToneSpecs {
    // Values wired directly to TonemapEngine 1.0.0 public TonemapParams where supported.

    /** Builds a spec from [TonemapCatalog]; the catalog owns labels/ranges/defaults. */
    private fun toneSpec(parameter: ImageToneNumericParameter): NumericSettingSpec =
        parameter.tonemapDescriptor().let {
            NumericSettingSpec(parameter, it.longLabel, it.minimum, it.maximum, it.defaultValue, it.step, it.unit, it.decimals)
        }

    val renderExposure = toneSpec(ImageToneNumericParameter.RenderExposure)
    val blacks = toneSpec(ImageToneNumericParameter.BlackToe)
    val shadows = toneSpec(ImageToneNumericParameter.Shadows)
    val contrast = toneSpec(ImageToneNumericParameter.Contrast)
    val midtones = toneSpec(ImageToneNumericParameter.MidtonePivot)
    val highlights = toneSpec(ImageToneNumericParameter.Highlights)
    val whites = toneSpec(ImageToneNumericParameter.ShoulderWhitePoint)
    val saturation = toneSpec(ImageToneNumericParameter.Saturation)
    val vibrance = toneSpec(ImageToneNumericParameter.Vibrance)
    val colorRenderingStrength =
        NumericSettingSpec(
            ImageToneNumericParameter.ColorRenderingStrength,
            "Color Rendering Strength",
            0f,
            1f,
            1f,
            .01f,
            "",
            2
        )
    val wbTemperature =
        NumericSettingSpec(ImageToneNumericParameter.WbTemperature, "WB Temperature", -100f, 100f, 0f, 1f)
    val wbTint = NumericSettingSpec(ImageToneNumericParameter.WbTint, "WB Tint", -100f, 100f, 0f, 1f)
    val jpegQuality = NumericSettingSpec(ImageToneNumericParameter.JpegQuality, "JPEG Quality", 95f, 100f, 98f, 1f, "%")
}

/**
 * Maps the settings parameter enum onto the canonical [TonemapCatalog] entry.
 * Only the nine per-profile tone controls participate; global image params
 * (color rendering strength, WB, JPEG) keep their own specs.
 */
fun ImageToneNumericParameter.tonemapDescriptor(): TonemapParam = when (this) {
    ImageToneNumericParameter.RenderExposure -> TonemapCatalog.renderExposure
    ImageToneNumericParameter.BlackToe -> TonemapCatalog.forParameter(ToneParameter.Blacks)
    ImageToneNumericParameter.Shadows -> TonemapCatalog.forParameter(ToneParameter.Shadows)
    ImageToneNumericParameter.Contrast -> TonemapCatalog.forParameter(ToneParameter.Contrast)
    ImageToneNumericParameter.MidtonePivot -> TonemapCatalog.forParameter(ToneParameter.Midtones)
    ImageToneNumericParameter.Highlights -> TonemapCatalog.forParameter(ToneParameter.Highlights)
    ImageToneNumericParameter.ShoulderWhitePoint -> TonemapCatalog.forParameter(ToneParameter.Whites)
    ImageToneNumericParameter.Saturation -> TonemapCatalog.forParameter(ToneParameter.Saturation)
    ImageToneNumericParameter.Vibrance -> TonemapCatalog.forParameter(ToneParameter.Vibrance)
    else -> error("$this is not a tonemap tone control")
}

fun ImageToneNumericParameter.reviewSpec(): NumericSettingSpec = when (this) {
    ImageToneNumericParameter.RenderExposure -> ImageToneSpecs.renderExposure
    ImageToneNumericParameter.BlackToe -> ImageToneSpecs.blacks
    ImageToneNumericParameter.Shadows -> ImageToneSpecs.shadows
    ImageToneNumericParameter.Contrast -> ImageToneSpecs.contrast
    ImageToneNumericParameter.MidtonePivot -> ImageToneSpecs.midtones
    ImageToneNumericParameter.Highlights -> ImageToneSpecs.highlights
    ImageToneNumericParameter.ShoulderWhitePoint -> ImageToneSpecs.whites
    ImageToneNumericParameter.Saturation -> ImageToneSpecs.saturation
    ImageToneNumericParameter.Vibrance -> ImageToneSpecs.vibrance
    ImageToneNumericParameter.ColorRenderingStrength -> ImageToneSpecs.colorRenderingStrength
    ImageToneNumericParameter.WbTemperature -> ImageToneSpecs.wbTemperature
    ImageToneNumericParameter.WbTint -> ImageToneSpecs.wbTint
    ImageToneNumericParameter.JpegQuality -> ImageToneSpecs.jpegQuality
}

fun NumericSettingSpec.accepts(value: Float): Boolean = value.isFinite() && value in minimum..maximum

/** Photo/Video capture mode for a persisted ID. Unknown values fall back to Photo. */
fun captureModeForId(id: String): com.rawr.camera.model.CaptureMode =
    if (id == "video") com.rawr.camera.model.CaptureMode.Video else com.rawr.camera.model.CaptureMode.Photo

fun com.rawr.camera.model.CaptureMode.persistedId(): String =
    if (this == com.rawr.camera.model.CaptureMode.Video) "video" else "photo"

/** Video recording resolution for a persisted ID. Unknown values fall back to 1080p. */
fun videoResolutionForId(id: String): com.rawr.camera.video.VideoResolutionMode = when (id) {
    "4k" -> com.rawr.camera.video.VideoResolutionMode.UHD4K
    "open_gate" -> com.rawr.camera.video.VideoResolutionMode.OPEN_GATE
    else -> com.rawr.camera.video.VideoResolutionMode.HD1080
}

fun com.rawr.camera.video.VideoResolutionMode.persistedId(): String = when (this) {
    com.rawr.camera.video.VideoResolutionMode.UHD4K -> "4k"
    com.rawr.camera.video.VideoResolutionMode.OPEN_GATE -> "open_gate"
    com.rawr.camera.video.VideoResolutionMode.HD1080 -> "1080p"
}

/** Recording frame rate clamp; only 24 and 30 are supported by the native recorder. */
fun videoFpsOrDefault(fps: Int): Int = if (fps == 24 || fps == 30) fps else 30

/** Linear AE post-gain cap for an ID (gain.100=1x, gain.200=2x, gain.400=4x). Defaults to 4x. */
fun maxPostGainForId(id: String): Float = when (id) {
    "gain.100" -> 1f
    "gain.200" -> 2f
    else -> 4f
}

/** Auto-AE FPS floor for an ID (fps.30 == off/template). Defaults to 15. */
fun autoMinFpsForId(id: String): Int = when (id) {
    "fps.30" -> 30
    "fps.24" -> 24
    "fps.20" -> 20
    "fps.12" -> 12
    "fps.8" -> 8
    else -> 15
}

/** NDK AE antibanding mode (OFF=0, 50HZ=1, 60HZ=2, AUTO=3). */
fun antibandingModeFor(value: AntiFlicker): Int = when (value) {
    AntiFlicker.Hz50 -> 1
    AntiFlicker.Hz60 -> 2
    AntiFlicker.Off -> 0
    AntiFlicker.Auto -> 3
}

/** Post-gain soft-knee width in EV for a HighlightProtection level. */
fun postGainKneeWidthEvFor(value: HighlightProtection): Float = when (value) {
    HighlightProtection.Low -> 2f
    HighlightProtection.High -> 0.5f
    HighlightProtection.Normal -> 1f
}
