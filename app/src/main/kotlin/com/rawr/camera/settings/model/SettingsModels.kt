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
    DynamicRange("Dynamic Range"),
    LutProfile("LUT Profile"),
    Demosaic("Demosaic"),
    Defringe("Defringe"),
    Denoise("Denoise"),
    LensShading("Lens Shading"),
    Distortion("Distortion Correction"),
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

/** rawr auto exposure: how far dim scenes are placed below day brightness; native NightLook ids. */
enum class RawrNightLook(val label: String, val nativeId: Int) {
    Natural("Natural", 1),
    Bright("Bright", 2),
    Off("Off", 0)
}

/**
 * Slowest shutter rawr AE will use. ISO takes over the rest of the light; the engine starts
 * raising ISO at 1/30 s (or this, if faster). Auto: 1/15 s for photo, the frame time for video.
 */
enum class RawrMaxShutter(val label: String, val ns: Long) {
    Auto("Auto (1/15)", 0L),
    S125("1/125", 8_000_000L),
    S60("1/60", 16_666_667L),
    S30("1/30", 33_333_333L),
    S15("1/15", 66_666_667L),
    S8("1/8", 125_000_000L),
    S5("1/5", 200_000_000L)
}

/** Auto shutter/ISO trade (EV): faster shutters raise ISO; lower ISO lets the shutter run longer first. */
enum class RawrBalance(val label: String, val ev: Float) {
    LowIso("Low ISO", -1f),
    Balanced("Balanced", 0f),
    Faster("Faster", 1f),
    Fastest("Fastest", 2f)
}

/** Digital gain allowed past the highest real ISO, applied cleanly after RAW (EV). */
enum class RawrDigitalGain(val label: String, val ev: Float) {
    Off("Off", 0f),
    Ev1("+1 EV", 1f),
    Ev2("+2 EV", 2f),
    Ev3("+3 EV", 3f),
    Ev4("+4 EV", 4f),
    /** No practical limit: whatever the scene still lacks becomes gain (noisy). */
    Maximum("Maximum", 16f)
}

/** How quickly rawr AE follows the scene; native speed ids. */
enum class RawrAeSpeed(val label: String, val nativeId: Int) {
    Slow("Slow", 0),
    Normal("Normal", 1),
    Fast("Fast", 2)
}

/** rawr metering mode; a tap-to-meter spot overrides it while set. */
enum class RawrMetering(val label: String, val nativeId: Int) {
    Matrix("Matrix", 0),
    CenterWeighted("Centre", 1),
    Average("Average", 2),
    Highlight("Highlight", 3)
}

/** rawr auto exposure: Auto/S/I solved in the app with Camera2 AE off. On by default. */
/** ISO always spans base to the sensor's analog maximum; no user range. */
data class RawrAeSettings(
    val enabled: Boolean = true,
    val nightLook: RawrNightLook = RawrNightLook.Natural,
    val maxShutter: RawrMaxShutter = RawrMaxShutter.Auto,
    val balance: RawrBalance = RawrBalance.Balanced,
    val digitalGain: RawrDigitalGain = RawrDigitalGain.Ev2,
    val speed: RawrAeSpeed = RawrAeSpeed.Normal,
    val metering: RawrMetering = RawrMetering.Matrix
)

enum class ExposureStep(val label: String) {
    Third("1/3 EV"),
    Half("1/2 EV")
}

/** How a boost request above the limit rolls off (native knee width 2 / 1 / 0.5 EV). */
enum class HighlightProtection(val label: String, val kneeSummary: String) {
    Low("Soft", "Up to 2 EV more past the limit · brightest, most clipping risk"),
    Normal("Normal", "Up to 1 EV more past the limit · balanced"),
    High("Hard", "Up to 0.5 EV more past the limit · darkest, safest highlights")
}

/** Shadow lift (adaptive scene-key curve) after the brightness boost; native AutoKeyMode ids. */
/** Photo dynamic range method as stored: Adaptive = the global curve (strength in autoKeyExposure). */
enum class PhotoToneMapping { Adaptive, Local }

/**
 * Dynamic Range as the UI presents it (Image > Rendering > Dynamic Range,
 * Renderer > Render): fitting the scene's range before Tone Mapping.
 * Off and Global are the adaptive curve's strength (Off / the rest).
 */
enum class DynamicRangeMethod(val label: String, val summary: String) {
    Off("Off", "No shadow brightening: the image as exposed"),
    Global("Global", "One curve for the whole image"),
    Local("Local", "Region by region (exposure fusion): keeps local contrast")
}

// Keep the experimental implementation and recipe support, but hide its controls.
const val LOCAL_TONE_CONTROLS_VISIBLE = false
val visibleDynamicRangeMethods: List<DynamicRangeMethod> = DynamicRangeMethod.entries.filter {
    LOCAL_TONE_CONTROLS_VISIBLE || it != DynamicRangeMethod.Local
}

fun SettingsValues.photoDynamicRange(): DynamicRangeMethod = when {
    photoToneMapping == PhotoToneMapping.Local -> DynamicRangeMethod.Local
    autoKeyExposure == AutoKeyExposure.Off -> DynamicRangeMethod.Off
    else -> DynamicRangeMethod.Global
}

fun SettingsValues.videoDynamicRange(): DynamicRangeMethod =
    if (videoAutoKeyExposure == AutoKeyExposure.Off) DynamicRangeMethod.Off else DynamicRangeMethod.Global

/** Global strengths (the adaptive curve without Off). */
val globalDynamicRangeStrengths: List<AutoKeyExposure> = AutoKeyExposure.entries.filter { it != AutoKeyExposure.Off }

/** Conservative UI ranges; image quality still depends on scene content. */
data class LocalToneTuning(
    val highlightsEv: Float = 2f,
    val sigma: Float = 5f,
    val coarsestMip: Int = 6,
    val boostLocalContrast: Boolean = false
) {
    fun sanitized() = copy(
        highlightsEv = if (highlightsEv.isFinite()) (highlightsEv.coerceIn(.5f, 2f) * 10f).roundToInt() / 10f else 2f,
        sigma = if (sigma.isFinite()) (sigma.coerceIn(3f, 5f) * 10f).roundToInt() / 10f else 5f,
        coarsestMip = coarsestMip.coerceIn(6, 8)
    )
}

/** Local tone mapping shadows range, shared with native local_tone/ExposureFusion.h. */
object LocalToneLimits {
    const val DEFAULT_SHADOWS_EV = 2.0f
    const val MIN_SHADOWS_EV = 1.0f
    const val MAX_SHADOWS_EV = 3.0f
    fun shadowsEv(value: Float): Float =
        if (value.isFinite()) (value.coerceIn(MIN_SHADOWS_EV, MAX_SHADOWS_EV) * 10f).roundToInt() / 10f else DEFAULT_SHADOWS_EV
}

enum class AutoKeyExposure(val label: String, val summary: String, val nativeId: Int) {
    Off("Off", "No lift · renders the boosted exposure as is", 0),
    Natural("Low", "Lifts shadows up to 1 EV", 1),
    Medium("Medium", "Lifts shadows up to 2 EV", 3),
    Strong("Strong", "Lifts shadows up to 3 EV", 2)
}

/** [nativeId] matches tonemap_integration::ColorRenderProfile; ids 4-8 are the LOG profiles. */
enum class ColorRenderProfile(val label: String, val nativeId: Int) {
    RawrBase("RAWR NTRL", 0),
    UserLut("Imported LUT", 1),
    SRgb("sRGB", 2),
    Rec709("Rec.709", 3),
    AgX("AgX", 9);

    /** AgX writes sRGB for stills and Rec.709 for video (native id 10). */
    fun nativeIdFor(video: Boolean): Int = if (this == AgX && video) AGX_REC709_NATIVE_ID else nativeId

    companion object {
        const val AGX_REC709_NATIVE_ID = 10
    }
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
    Amaze("AMaZE"),
    Rcd("RCD"),
    Vng4("VNG4"),
    DualRcdVng4("RCD + VNG4"),
    DualAmazeVng4("AMaZE + VNG4")
}

/** RawTherapee capture sharpening strength: Richardson-Lucy iterations after demosaic. */
enum class CaptureSharpeningLevel(val label: String, val iterations: Int) {
    Low("Low", 2),
    Medium("Medium", 3),
    Sharp("Sharp", 4),
    Sharpest("Sharpest", 6)
}

enum class MultiframeMergeAlgorithm(val nativeId: Int, val label: String) {
    HdrPlusQuality(2, "HDR+ Quality"),
    /** Kernel-regression Bayer merge for equal-exposure bursts, native resolution only (native/multiframe/merge_rexture). */
    Rexture(4, "Rexture")
}

enum class MultiframeBaseFrameMode(val nativeId: Int, val label: String) {
    Middle(0, "Middle"),
    Sharpest(1, "Sharpest")
}

enum class MultiframeNumericParameter {
    MaxFrames,
    HdrPlusStrength,
    HdrPlusTileSize
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

/** Frames one multiframe capture may merge (native capture journal limit). */
const val MAX_BURST_FRAMES = 30

object MultiframeSpecs {
    val maxFrames =
        MultiframeNumericSpec(
            MultiframeNumericParameter.MaxFrames,
            "Maximum Frames",
            "Lower: faster but noisier. Higher: cleaner flats yet slightly softer edges, slower with more motion risk.",
            2f,
            30f,
            6f,
            1f,
            0
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

    val all = listOf(maxFrames, hdrPlusStrength, hdrPlusTileSize)

    fun forParameter(parameter: MultiframeNumericParameter): MultiframeNumericSpec =
        all.first { it.parameter == parameter }
}

data class MultiframeTuning(
    val mergeAlgorithm: MultiframeMergeAlgorithm = MultiframeMergeAlgorithm.Rexture,
    val hdrPlusStrength: Float = 13f,
    val hdrPlusTileSize: Int = 32,
    val maxFrames: Int = 6
) {
    fun value(parameter: MultiframeNumericParameter): Float = when (parameter) {
        MultiframeNumericParameter.MaxFrames -> maxFrames.toFloat()
        MultiframeNumericParameter.HdrPlusStrength -> hdrPlusStrength
        MultiframeNumericParameter.HdrPlusTileSize -> hdrPlusTileSize.toFloat()
    }

    fun withValue(parameter: MultiframeNumericParameter, value: Float): MultiframeTuning = when (parameter) {
        MultiframeNumericParameter.MaxFrames -> copy(maxFrames = value.roundToInt())
        MultiframeNumericParameter.HdrPlusStrength -> copy(hdrPlusStrength = value.roundToInt().toFloat())
        MultiframeNumericParameter.HdrPlusTileSize -> copy(hdrPlusTileSize = if (value < 24f) 16 else 32)
    }

    fun sanitized(defaults: MultiframeTuning = MultiframeTuning()): MultiframeTuning {
        var clean = this
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

    /**
     * Native tuning wire format (native indices 0-21). The native side still reads every slot, but no merge uses them any
     * more: they are sent as the former defaults until the capture journal format is reworked.
     */
    fun nativeValues(): FloatArray = floatArrayOf(
        1f, 5f, 1e-10f, .15f, 5f, .71f, 1f, 2f, 8f, 1f, .15f, 1f, 1f, 3f, .01f, .04f, .08f, 1.5f, 9f, .65f, 4f, 2f
    )

    // Appended after the multiframe chroma-denoise flag (native indices 23-28); the bracket slots are unused.
    fun nativeMergeValues(): FloatArray =
        floatArrayOf(mergeAlgorithm.nativeId.toFloat(), hdrPlusStrength, hdrPlusTileSize.toFloat(), -3f, 0f, 0f)
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
    /** Adaptive shadow-lift strength for photos (when Photo tone mapping is Adaptive). */
    val autoKeyExposure: AutoKeyExposure = AutoKeyExposure.Medium,
    /** Adaptive shadow-lift strength for video (always adaptive). */
    val videoAutoKeyExposure: AutoKeyExposure = AutoKeyExposure.Medium,
    /** Photo tone mapping (JPEG/viewfinder only; video always Adaptive). */
    val photoToneMapping: PhotoToneMapping = PhotoToneMapping.Adaptive,
    /** Local tone mapping: how far shadows may be brightened (EV), noise-limited natively. */
    val localToneShadowsEv: Float = LocalToneLimits.DEFAULT_SHADOWS_EV,
    val localToneTuning: LocalToneTuning = LocalToneTuning(),
    val maxPostGainId: String = "gain.200",
    val autoMinFpsId: String = "fps.12",
    val controlSurfaceStyle: ControlSurfaceStyle = ControlSurfaceStyle.Basic,
    val captureControlLayout: CaptureControlLayout = CaptureControlLayout.Compact,
    val jpegEnabled: Boolean = true,
    val dngEnabled: Boolean = true,
    val dngCompressionId: String = "dng.lossless",
    val gridMode: GridMode = GridMode.Thirds,
    /** Capture-screen DCP shortcut next to the lens selector. */
    val showDcpQuickSelect: Boolean = true,
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
    /** AgX's render exposure (its other display tone fields are unused), shared by its presets. */
    val agxTone: ProfileTone = ProfileTone.Neutral,
    val agxPreset: AgxPreset = AgxPreset.Rawr,
    val videoAgxPreset: AgxPreset = AgxPreset.Rawr,
    /** Saved look origins survive edits; photo and video remember their own selection. */
    val selectedAgxLookId: String? = null,
    val videoAgxLookId: String? = null,
    /** Edited AgX controls per preset; a missing preset uses its defaults. */
    val agxValues: Map<AgxPreset, AgxValues> = emptyMap(),
    /** User-saved AgX looks; applying one writes its values into its base look. */
    val agxLooks: List<AgxLook> = emptyList(),
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
    val multiframeBaseFrameMode: MultiframeBaseFrameMode = MultiframeBaseFrameMode.Sharpest,
    // Chroma-only profiled wavelet on the merged image, fed the burst-fitted
    // noise / frame count. Independent of Image > Denoise (single frame only). Off by default.
    val multiframeChromaDenoise: Boolean = false,
    // Rexture only: render the JPEG from the merged RGB (no re-mosaic, no demosaic setting). The DNG is unaffected.
    val multiframeRextureNativeRgb: Boolean = false,
    val multiframeTuning: MultiframeTuning = MultiframeTuning(),
    val customGpuDriverEnabled: Boolean = false,
    val customGpuDriverName: String? = null,
    val persistZslRingEnabled: Boolean = false,
    val persistZslRingOnShutterEnabled: Boolean = false,
    val demosaicAlgorithm: DemosaicAlgorithm = DemosaicAlgorithm.Amaze,
    val dualAutoContrast: Boolean = true,
    val dualContrastPercent: Float = 20f,
    // Photo FCC steps (stills). Video FCC is an enable with a fixed single step.
    val photoFccSteps: Int = 2,
    val videoFccEnabled: Boolean = false,
    // Photo defringe (stills). Video strengths are independent.
    val photoDefringeEnabled: Boolean = true,
    val photoDefringeStrength: Float = 1f,
    val photoDefringeEdgeThreshold: Float = 0.02f,
    val photoDefringeLumaFloor: Float = 0.08f,
    val videoDefringeEnabled: Boolean = false,
    val videoDefringeStrength: Float = 1f,
    // Photo capture sharpening (render only: JPEG and Renderer, never the DNG).
    val photoCaptureSharpeningEnabled: Boolean = true,
    val photoCaptureSharpeningLevel: CaptureSharpeningLevel = CaptureSharpeningLevel.Low,
    // Photo-pipeline denoise, single source of truth (see DenoiseConfig).
    // Off by default; Wavelet is profiled (sensor noise model).
    val photoDenoise: DenoiseConfig = DenoiseConfig.Off(),
    // Video denoise has its own wavelet primitives.
    val videoDenoiseEnabled: Boolean = false,
    val videoDenoiseStrength: Float = 1f,
    val videoDenoiseDetail: Float = 1f,
    val videoDenoiseLuma: Float = 0.25f,
    val videoDenoiseScales: Int = 7,
    // Photo lens shading (preview + JPEG stills). Video is independent.
    val photoLensShadingEnabled: Boolean = true,
    val videoLensShadingEnabled: Boolean = true,
    // Photo (preview + stills + DNG warp) and video undistort, for lenses
    // whose profile carries a calibration.
    val distortionCorrectionEnabled: Boolean = true,
    val videoDistortionCorrectionEnabled: Boolean = true,
    // Photo highlight reconstruction (stills; live preview uses color
    // propagation). Video has its own method + controls below.
    val photoHighlightEnabled: Boolean = true,
    // 0 = existing propagation; 1 = RawTherapee Coloropp for still renders.
    val photoHighlightMethod: Int = 1,
    val photoHighlightThreshold: Float = 1f,
    val photoHighlightCompression: Float = 45f,
    val videoHighlightEnabled: Boolean = false,
    val videoHighlightMethod: Int = 0,
    val videoHighlightThreshold: Float = 1f,
    val videoHighlightCompression: Float = 100f,
    // UltraHDR (JPEG_R) still output: GPU gain map + MPF/XMP/ISO mux.
    // On by default; legacy SDR JPEG when off.
    val ultraHdrEnabled: Boolean = true,
    // Ignore Camera2 shutter/ISO priority modes: locking SS or ISO always enters
    // full Manual. Off by default.
    val aePriorityDisabled: Boolean = false,
    val rawrAe: RawrAeSettings = RawrAeSettings(),
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
    fun effectiveRenderProfileId(): Int =
        if (isLogActive) videoLogProfile.nativeId else regularRenderProfile().nativeIdFor(isVideo)
    fun regularAgxPreset(): AgxPreset = if (isVideo) videoAgxPreset else agxPreset
    fun agxValuesFor(preset: AgxPreset): AgxValues = agxValues[preset] ?: preset.defaults
    fun activeAgxValues(): AgxValues = agxValuesFor(regularAgxPreset())
    fun isAgxActive(): Boolean = !isLogActive && regularRenderProfile() == ColorRenderProfile.AgX

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
        // AgX skips the display tone controls; only render exposure applies.
        regularRenderProfile() == ColorRenderProfile.AgX -> ProfileTone(renderExposure = agxTone.renderExposure)
        else -> selectedLutProfile()?.tone ?: rawrBaseTone
    }

    fun activeImageTone(): ImageToneState = TonemapCatalog.all.fold(imageTone) { acc, descriptor ->
        acc.withValue(descriptor, activeProfileTone().valueFor(descriptor))
    }

    fun activeProfileLabel(): String = when {
        isLogActive -> videoLogProfile.label
        isAgxActive() -> selectedAgxLook()?.name ?: regularAgxPreset().label
        else -> selectedLutProfile()?.name?.takeIf { it.isNotBlank() } ?: regularRenderProfile().label
    }

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
