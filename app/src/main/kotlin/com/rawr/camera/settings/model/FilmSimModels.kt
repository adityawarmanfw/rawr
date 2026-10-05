package com.rawr.camera.settings.model

// Film simulation (spektra) look. Mirrors spektrafilm_native::FilmLook 1:1;
// field order below matches the JNI float[]/int[] contract documented in
// NativeEngineJni.cpp (setFilmSimLook). Defaults match upstream, except the
// output space: upstream defaults to Rec.709 Gamma 2.4 (video-oriented), but
// stills on a phone panel want sRGB.
data class FilmSimLook(
    // selectors
    val film: Int = 2,
    val paper: Int = 4,
    val inputColorSpace: Int = 15,
    val outputColorSpace: Int = 17,
    val rgbToRawMethod: Int = 2,
    // Workflow: 0=print simulation (negative stocks), 1=scan negative
    // (positive/reversal stocks, which would otherwise print negative).
    val process: Int = 0,
    // ScanNegative only: normalize + invert negative stocks for display.
    // Leave false for positive stocks (already positive).
    val scanNegativeInvert: Boolean = false,
    val filmPushPullMode: Int = 0,
    val printerLightsGang: Boolean = false,
    val printerLightCalibration: Boolean = true,
    val grainEnabled: Boolean = false,
    val grainModel: Int = 0,
    val filmFormat: Int = 4,
    val grainSublayersEnabled: Boolean = true,
    val grainSubLayerCount: Int = 1,
    val grainSeed: Int = 1,
    val grainAnimate: Boolean = false,
    val cameraUvFilterEnabled: Boolean = false,
    val cameraIrFilterEnabled: Boolean = false,
    // floats
    val filmExposureEv: Float = 0f,
    val printExposureEv: Float = 0f,
    val filmPushPullStops: Float = 0f,
    val filmGamma: Float = 1f,
    val printPushPullStops: Float = 0f,
    val printGamma: Float = 1f,
    val printShadowShape: Float = 0f,
    val printHighlightShape: Float = 0f,
    val negativeBleachBypassAmount: Float = 0f,
    val negativeLeucoCyanCoupling: Float = 1f,
    val printBleachBypassAmount: Float = 0f,
    val preflashExposure: Float = 0f,
    val preflashMFilterShift: Float = 0f,
    val preflashYFilterShift: Float = 0f,
    val filterC: Float = 0f,
    val filterMShift: Float = 0f,
    val filterYShift: Float = 0f,
    val printerLightsR: Float = 0f,
    val printerLightsG: Float = 0f,
    val printerLightsB: Float = 0f,
    val enlargerScale: Float = 1f,
    val enlargerOffsetXPercent: Float = 0f,
    val enlargerOffsetYPercent: Float = 0f,
    val grainAmount: Float = 1f,
    val grainSaturation: Float = 1f,
    val grainParticleAreaUm2: Float = 0.1f,
    val grainParticleScaleR: Float = 1.2f,
    val grainParticleScaleG: Float = 1f,
    val grainParticleScaleB: Float = 2.5f,
    val grainParticleScaleLayer0: Float = 6f,
    val grainParticleScaleLayer1: Float = 1f,
    val grainParticleScaleLayer2: Float = 0.4f,
    val grainDensityMinR: Float = 0.04f,
    val grainDensityMinG: Float = 0.05f,
    val grainDensityMinB: Float = 0.06f,
    val grainUniformityR: Float = 0.99f,
    val grainUniformityG: Float = 0.97f,
    val grainUniformityB: Float = 0.98f,
    val grainFinalBlurUm: Float = 11.8f,
    val grainBlurDyeCloudsUm: Float = 1f,
    val grainMicroStructureScale: Float = 0.2f,
    val grainMicroStructureSigmaNm: Float = 30f,
    val cameraUvCutNm: Float = 410f,
    val cameraIrCutNm: Float = 675f,
    val dirCouplersAmount: Float = 0f,
    val dirCouplersDiffusionUm: Float = 20f,
    val dirCouplersDiffusionTailUm: Float = 200f,
    val dirCouplersDiffusionTailWeight: Float = 0.06f,
    val dirCouplersInhibitionSameLayer: Float = 1f,
    val dirCouplersInhibitionInterlayer: Float = 1f,
    val dirCouplersGammaSameLayerR: Float = 0.336f,
    val dirCouplersGammaSameLayerG: Float = 0.319f,
    val dirCouplersGammaSameLayerB: Float = 0.273f,
    val dirCouplersGammaRToG: Float = 0.353f,
    val dirCouplersGammaRToB: Float = 0.302f,
    val dirCouplersGammaGToR: Float = 0.154f,
    val dirCouplersGammaGToB: Float = 0.353f,
    val dirCouplersGammaBToR: Float = 0.168f,
    val dirCouplersGammaBToG: Float = 0.226f,
    val scannerEnabled: Boolean = false,
    val scannerWhiteCorrection: Boolean = false,
    val scannerBlackCorrection: Boolean = false,
    val scannerWhiteLevel: Float = 0.98f,
    val scannerBlackLevel: Float = 0.01f,
    val glarePercent: Float = 0.03f,
    val glareRoughness: Float = 0.7f,
    val glareBlur: Float = 0.5f,
    val scannerMtf50LpMm: Float = 60f,
    val scannerUnsharpRadiusUm: Float = 5f,
    val scannerUnsharpAmount: Float = 0.7f,
    val halationEnabled: Boolean = false,
    val scatterAmount: Float = 1f,
    val scatterScale: Float = 1f,
    val halationAmount: Float = 1f,
    val halationScale: Float = 1f,
    val halationStrengthR: Float = 0.05f,
    val halationStrengthG: Float = 0.015f,
    val halationStrengthB: Float = 0f,
    val halationFirstSigmaUmR: Float = 65f,
    val halationFirstSigmaUmG: Float = 65f,
    val halationFirstSigmaUmB: Float = 65f,
    val halationBoostEv: Float = 0f,
    val halationBoostRange: Float = 0.3f,
    val halationProtectEv: Float = 4f,
    val cameraDiffusionEnabled: Boolean = false,
    val cameraDiffusionFamily: Int = 1,
    val cameraDiffusionStrength: Float = 0.5f,
    val cameraDiffusionSpatialScale: Float = 1f,
    val cameraDiffusionHaloWarmth: Float = 0f,
    val cameraDiffusionCoreIntensity: Float = 1f,
    val cameraDiffusionCoreSize: Float = 1f,
    val cameraDiffusionHaloIntensity: Float = 1f,
    val cameraDiffusionHaloSize: Float = 1f,
    val cameraDiffusionBloomIntensity: Float = 1f,
    val cameraDiffusionBloomSize: Float = 1f,
    val printDiffusionEnabled: Boolean = false,
    val printDiffusionFamily: Int = 1,
    val printDiffusionStrength: Float = 0.5f,
    val printDiffusionSpatialScale: Float = 1f,
    val printDiffusionHaloWarmth: Float = 0f,
    val printDiffusionCoreIntensity: Float = 1f,
    val printDiffusionCoreSize: Float = 1f,
    val printDiffusionHaloIntensity: Float = 1f,
    val printDiffusionHaloSize: Float = 1f,
    val printDiffusionBloomIntensity: Float = 1f,
    val printDiffusionBloomSize: Float = 1f
) {
    // Index contracts mirror NativeEngineJni.setFilmSimLook (98 floats, 27 ints).
    fun toFloatArray(): FloatArray = floatArrayOf(        filmExposureEv, printExposureEv, filmPushPullStops, filmGamma,
        printPushPullStops, printGamma, printShadowShape, printHighlightShape,
        negativeBleachBypassAmount, negativeLeucoCyanCoupling, printBleachBypassAmount,
        preflashExposure, preflashMFilterShift, preflashYFilterShift,
        filterC, filterMShift, filterYShift,
        printerLightsR, printerLightsG, printerLightsB,
        enlargerScale, enlargerOffsetXPercent, enlargerOffsetYPercent,
        grainAmount, grainSaturation, grainParticleAreaUm2,
        grainParticleScaleR, grainParticleScaleG, grainParticleScaleB,
        grainParticleScaleLayer0, grainParticleScaleLayer1, grainParticleScaleLayer2,
        grainDensityMinR, grainDensityMinG, grainDensityMinB,
        grainUniformityR, grainUniformityG, grainUniformityB,
        grainFinalBlurUm, grainBlurDyeCloudsUm,
        grainMicroStructureScale, grainMicroStructureSigmaNm,
        cameraUvCutNm, cameraIrCutNm,
        dirCouplersAmount.coerceIn(0f, 1f), dirCouplersDiffusionUm, dirCouplersDiffusionTailUm,
        dirCouplersDiffusionTailWeight, dirCouplersInhibitionSameLayer,
        dirCouplersInhibitionInterlayer, dirCouplersGammaSameLayerR,
        dirCouplersGammaSameLayerG, dirCouplersGammaSameLayerB,
        dirCouplersGammaRToG, dirCouplersGammaRToB, dirCouplersGammaGToR,
        dirCouplersGammaGToB, dirCouplersGammaBToR, dirCouplersGammaBToG,
        scannerWhiteLevel, scannerBlackLevel, glarePercent,
        glareRoughness, glareBlur, scannerMtf50LpMm,
        scannerUnsharpRadiusUm, scannerUnsharpAmount,
        scatterAmount, scatterScale, halationAmount, halationScale,
        halationStrengthR, halationStrengthG, halationStrengthB,
        halationFirstSigmaUmR, halationFirstSigmaUmG, halationFirstSigmaUmB,
        halationBoostEv, halationBoostRange, halationProtectEv,
        cameraDiffusionStrength, cameraDiffusionSpatialScale,
        cameraDiffusionHaloWarmth, cameraDiffusionCoreIntensity,
        cameraDiffusionCoreSize, cameraDiffusionHaloIntensity,
        cameraDiffusionHaloSize, cameraDiffusionBloomIntensity,
        cameraDiffusionBloomSize,
        printDiffusionStrength, printDiffusionSpatialScale,
        printDiffusionHaloWarmth, printDiffusionCoreIntensity,
        printDiffusionCoreSize, printDiffusionHaloIntensity,
        printDiffusionHaloSize, printDiffusionBloomIntensity,
        printDiffusionBloomSize
    )

    fun toIntArray(): IntArray = intArrayOf(
        film, paper, inputColorSpace, outputColorSpace, rgbToRawMethod,
        filmPushPullMode,
        if (printerLightsGang) 1 else 0,
        if (printerLightCalibration) 1 else 0,
        if (grainEnabled) 1 else 0,
        grainModel, filmFormat,
        if (grainSublayersEnabled) 1 else 0,
        grainSubLayerCount, grainSeed,
        if (grainAnimate) 1 else 0,
        if (cameraUvFilterEnabled) 1 else 0,
        if (cameraIrFilterEnabled) 1 else 0,
        process,
        if (scanNegativeInvert) 1 else 0,
        if (scannerEnabled) 1 else 0,
        if (scannerWhiteCorrection) 1 else 0,
        if (scannerBlackCorrection) 1 else 0,
        if (halationEnabled) 1 else 0,
        if (cameraDiffusionEnabled) 1 else 0,
        cameraDiffusionFamily,
        if (printDiffusionEnabled) 1 else 0,
        printDiffusionFamily
    )
}

/** Human-readable film block for JPEG EXIF description. Built at shutter
 * time; native splices it in place of the renderer-profile + tonemap blocks
 * when the shot rendered through film (film bypasses the profile/LUT).
 * Dash-per-line style matches the rest of the EXIF description. */
fun FilmSimLook.describeFilm(): String {
    fun ev(value: Float): String {
        val rounded = kotlin.math.round(value * 100) / 100.0
        return (if (rounded >= 0) "+" else "") + rounded
    }
    val stock = FilmStocks.films.getOrElse(film) { "?" }
    val paper = FilmStocks.papers.getOrElse(paper) { "?" }
    val workflow = FilmStocks.processes.getOrElse(process) { "?" }
    val method = FilmStocks.spectralMethods.getOrElse(rgbToRawMethod) { "?" }
    val lines = mutableListOf(
        "Film simulation:",
        "- Stock: $stock",
        "- Paper: $paper",
        "- Process: $workflow",
        "- Method: $method",
        "- Film EV: ${ev(filmExposureEv)}",
        "- Print EV: ${ev(printExposureEv)}",
        "- Grain: " + if (grainEnabled) "on" else "off"
    )
    if (dirCouplersAmount > 0) {
        lines.add("- DIR couplers: " + (kotlin.math.round(dirCouplersAmount * 100) / 100.0))
    }
    if (scannerEnabled) lines.add("- Scanner: on")
    if (halationEnabled) lines.add("- Halation: on")
    if (cameraDiffusionEnabled) lines.add("- Diffusion: on")
    if (printDiffusionEnabled) lines.add("- Print diffusion: on")
    return lines.joinToString("\n")
}

/** Discrete (choice) fields of [FilmSimLook]. UI option lists come from [FilmSimCatalog]. */
enum class FilmSimDiscreteField {
    Film,
    Paper,
    InputColorSpace,
    OutputColorSpace,
    SpectralMethod,
    PushPullMode,
    FilmFormat,
    GrainSubLayerCount,
    Process,
    CameraDiffusionFamily,
    PrintDiffusionFamily,
    GrainModel,
}

/** Boolean flags of [FilmSimLook]. */
enum class FilmSimFlag {
    PrinterLightsGang,
    PrinterLightCalibration,
    GrainEnabled,
    GrainSublayersEnabled,
    GrainAnimate,
    CameraUvEnabled,
    CameraIrEnabled,
    ScanNegativeInvert,
    ScannerEnabled,
    ScannerWhiteCorrection,
    ScannerBlackCorrection,
    HalationEnabled,
    CameraDiffusionEnabled,
    PrintDiffusionEnabled
}

enum class FilmSimSection(val title: String) {
    Film("Film"),
    DirCouplers("DIR Couplers"),
    Print("Print"),
    Filters("Filters"),
    Diffusion("Diffusion"),
    Grain("Grain"),
    Halation("Halation"),
    Scanner("Scanner"),
    Output("Output"),
    Presets("Presets")
}

/** Third-level drill pages behind toggle+submenu / submenu rows. UI-only routing. */
enum class FilmSimDetail(val title: String, val parent: FilmSimSection) {
    GrainEmulsion("Emulsion", FilmSimSection.Grain),
    GrainTexture("Texture", FilmSimSection.Grain),
    HalationCoupling("Halation coupling", FilmSimSection.Halation),
    GlareShaping("Glare shaping", FilmSimSection.Scanner)
}

enum class FilmSimNumericParameter {
    FilmExposureEv,
    PrintExposureEv,
    FilmPushPullStops,
    FilmGamma,
    PrintPushPullStops,
    PrintGamma,
    PrintShadowShape,
    PrintHighlightShape,
    NegativeBleachBypassAmount,
    NegativeLeucoCyanCoupling,
    PrintBleachBypassAmount,
    PreflashExposure,
    PreflashMFilterShift,
    PreflashYFilterShift,
    FilterC,
    FilterMShift,
    FilterYShift,
    PrinterLightsR,
    PrinterLightsG,
    PrinterLightsB,
    EnlargerScale,
    EnlargerOffsetXPercent,
    EnlargerOffsetYPercent,
    GrainAmount,
    GrainSaturation,
    GrainParticleAreaUm2,
    GrainParticleScaleR,
    GrainParticleScaleG,
    GrainParticleScaleB,
    GrainParticleScaleLayer0,
    GrainParticleScaleLayer1,
    GrainParticleScaleLayer2,
    GrainDensityMinR,
    GrainDensityMinG,
    GrainDensityMinB,
    GrainUniformityR,
    GrainUniformityG,
    GrainUniformityB,
    GrainFinalBlurUm,
    GrainBlurDyeCloudsUm,
    GrainMicroStructureScale,
    GrainMicroStructureSigmaNm,
    CameraUvCutNm,
    CameraIrCutNm,
    GrainSeed,
    DirCouplersAmount,
    DirCouplersDiffusionUm,
    DirCouplersDiffusionTailUm,
    DirCouplersDiffusionTailWeight,
    DirCouplersInhibitionSameLayer,
    DirCouplersInhibitionInterlayer,
    DirCouplersGammaSameLayerR,
    DirCouplersGammaSameLayerG,
    DirCouplersGammaSameLayerB,
    DirCouplersGammaRToG,
    DirCouplersGammaRToB,
    DirCouplersGammaGToR,
    DirCouplersGammaGToB,
    DirCouplersGammaBToR,
    DirCouplersGammaBToG,
    ScannerWhiteLevel,
    ScannerBlackLevel,
    GlarePercent,
    GlareRoughness,
    GlareBlur,
    ScannerMtf50LpMm,
    ScannerUnsharpRadiusUm,
    ScannerUnsharpAmount,
    ScatterAmount,
    ScatterScale,
    HalationAmount,
    HalationScale,
    HalationStrengthR,
    HalationStrengthG,
    HalationStrengthB,
    HalationFirstSigmaUmR,
    HalationFirstSigmaUmG,
    HalationFirstSigmaUmB,
    HalationBoostEv,
    HalationBoostRange,
    HalationProtectEv,
    CameraDiffusionStrength,
    CameraDiffusionSpatialScale,
    CameraDiffusionHaloWarmth,
    CameraDiffusionCoreIntensity,
    CameraDiffusionCoreSize,
    CameraDiffusionHaloIntensity,
    CameraDiffusionHaloSize,
    CameraDiffusionBloomIntensity,
    CameraDiffusionBloomSize,
    PrintDiffusionStrength,
    PrintDiffusionSpatialScale,
    PrintDiffusionHaloWarmth,
    PrintDiffusionCoreIntensity,
    PrintDiffusionCoreSize,
    PrintDiffusionHaloIntensity,
    PrintDiffusionHaloSize,
    PrintDiffusionBloomIntensity,
    PrintDiffusionBloomSize
}

data class FilmSimNumericSpec(
    val parameter: FilmSimNumericParameter,
    val section: FilmSimSection,
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

object FilmSimSpecs {
    val all: List<FilmSimNumericSpec> = listOf(
        FilmSimNumericSpec(FilmSimNumericParameter.FilmExposureEv, FilmSimSection.Film, "Film exposure",
            "Negative exposure; overexpose then print down for pastel density", -5f, 5f, 0f, 0.1f, 1, " EV"),
        FilmSimNumericSpec(FilmSimNumericParameter.FilmPushPullStops, FilmSimSection.Film, "Push / pull",
            "Development time shift in stops", -2f, 2f, 0f, 0.1f, 1, " st"),
        FilmSimNumericSpec(FilmSimNumericParameter.FilmGamma, FilmSimSection.Film, "Development gamma",
            "Negative contrast multiplier", 0.2f, 2f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.NegativeBleachBypassAmount, FilmSimSection.Film,
            "Negative bleach bypass", "Retained-silver desaturation and contrast", 0f, 1f, 0f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.NegativeLeucoCyanCoupling, FilmSimSection.Film,
            "Leuco cyan coupling", "Bleach-bypass color coupling strength", 0f, 2f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintExposureEv, FilmSimSection.Print, "Print exposure",
            "Enlarger exposure through the negative", -5f, 5f, 0f, 0.1f, 1, " EV"),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintPushPullStops, FilmSimSection.Print, "Print push / pull",
            "Paper development shift in stops", -2f, 2f, 0f, 0.1f, 1, " st"),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintGamma, FilmSimSection.Print, "Print contrast",
            "Paper contrast multiplier", 0.2f, 2f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintShadowShape, FilmSimSection.Print, "Print shadows",
            "Paper toe shaping", -1f, 1f, 0f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintHighlightShape, FilmSimSection.Print, "Print highlights",
            "Paper shoulder shaping", -1f, 1f, 0f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintBleachBypassAmount, FilmSimSection.Print,
            "Print bleach bypass", "Retained silver in the print", 0f, 1f, 0f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.FilterC, FilmSimSection.Print, "Filter C",
            "Enlarger cyan filtration", -50f, 50f, 0f, 0.5f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.FilterMShift, FilmSimSection.Print, "Filtration M",
            "Enlarger magenta filtration in CC units", -50f, 50f, 0f, 0.5f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.FilterYShift, FilmSimSection.Print, "Filtration Y",
            "Enlarger yellow filtration in CC units", -50f, 50f, 0f, 0.5f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.PreflashExposure, FilmSimSection.Print, "Preflash",
            "Paper fogging exposure; lifts shadows", 0f, 2f, 0f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PreflashMFilterShift, FilmSimSection.Print, "Preflash M",
            "Preflash magenta filtration", -50f, 50f, 0f, 0.5f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.PreflashYFilterShift, FilmSimSection.Print, "Preflash Y",
            "Preflash yellow filtration", -50f, 50f, 0f, 0.5f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.PrinterLightsR, FilmSimSection.Print, "Printer light R",
            "Additive printer point, red", -10f, 10f, 0f, 0.1f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.PrinterLightsG, FilmSimSection.Print, "Printer light G",
            "Additive printer point, green", -10f, 10f, 0f, 0.1f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.PrinterLightsB, FilmSimSection.Print, "Printer light B",
            "Additive printer point, blue", -10f, 10f, 0f, 0.1f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.EnlargerScale, FilmSimSection.Print, "Enlarger scale",
            "Print magnification", 1f, 4f, 1f, 0.05f, 2, "x"),
        FilmSimNumericSpec(FilmSimNumericParameter.EnlargerOffsetXPercent, FilmSimSection.Print, "Enlarger offset X",
            "Print cropping offset", -25f, 25f, 0f, 0.5f, 1, "%"),
        FilmSimNumericSpec(FilmSimNumericParameter.EnlargerOffsetYPercent, FilmSimSection.Print, "Enlarger offset Y",
            "Print cropping offset", -25f, 25f, 0f, 0.5f, 1, "%"),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainAmount, FilmSimSection.Grain, "Grain strength",
            "Overall grain strength", 0f, 3f, 1f, 0.05f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainSaturation, FilmSimSection.Grain, "Grain saturation",
            "Chroma vs monochrome grain", 0f, 1f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainParticleAreaUm2, FilmSimSection.Grain, "Granularity",
            "Mean grain particle area", 0.02f, 1f, 0.1f, 0.01f, 2, " um2"),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainParticleScaleR, FilmSimSection.Grain, "Particle scale R",
            "Relative red particle size", 0.2f, 4f, 1.2f, 0.05f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainParticleScaleG, FilmSimSection.Grain, "Particle scale G",
            "Relative green particle size", 0.2f, 4f, 1f, 0.05f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainParticleScaleB, FilmSimSection.Grain, "Particle scale B",
            "Relative blue particle size", 0.2f, 4f, 2.5f, 0.05f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainParticleScaleLayer0, FilmSimSection.Grain, "Layer scale coarse",
            "Coarse sub-layer particle scale", 0.1f, 12f, 6f, 0.1f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainParticleScaleLayer1, FilmSimSection.Grain, "Layer scale mid",
            "Mid sub-layer particle scale", 0.1f, 12f, 1f, 0.1f, 1),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainParticleScaleLayer2, FilmSimSection.Grain, "Layer scale fine",
            "Fine sub-layer particle scale", 0.1f, 12f, 0.4f, 0.05f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainDensityMinR, FilmSimSection.Grain, "Density floor R",
            "Minimum red dye density", 0f, 0.3f, 0.04f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainDensityMinG, FilmSimSection.Grain, "Density floor G",
            "Minimum green dye density", 0f, 0.3f, 0.05f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainDensityMinB, FilmSimSection.Grain, "Density floor B",
            "Minimum blue dye density", 0f, 0.3f, 0.06f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainUniformityR, FilmSimSection.Grain, "Uniformity R",
            "Red grain clumping", 0.5f, 1f, 0.99f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainUniformityG, FilmSimSection.Grain, "Uniformity G",
            "Green grain clumping", 0.5f, 1f, 0.97f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainUniformityB, FilmSimSection.Grain, "Uniformity B",
            "Blue grain clumping", 0.5f, 1f, 0.98f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainFinalBlurUm, FilmSimSection.Grain, "Grain blur",
            "Scan softness over grain", 0f, 30f, 11.8f, 0.1f, 1, " um"),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainBlurDyeCloudsUm, FilmSimSection.Grain, "Dye-cloud size",
            "Dye-cloud diffusion width", 0f, 5f, 1f, 0.05f, 2, " um"),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainMicroStructureScale, FilmSimSection.Grain, "Micro structure",
            "Fine dye microstructure strength", 0f, 1f, 0.2f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainMicroStructureSigmaNm, FilmSimSection.Grain, "Micro sigma",
            "Microstructure correlation width", 0f, 100f, 30f, 1f, 0, " nm"),
        FilmSimNumericSpec(FilmSimNumericParameter.GrainSeed, FilmSimSection.Grain, "Grain seed",
            "Random pattern selector", 0f, 100f, 1f, 1f, 0),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraUvCutNm, FilmSimSection.Filters, "UV cut",
            "Ultraviolet cut-on wavelength", 300f, 450f, 410f, 1f, 0, " nm"),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraIrCutNm, FilmSimSection.Filters, "IR cut",
            "Infrared cut-off wavelength", 600f, 750f, 675f, 1f, 0, " nm"),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersAmount, FilmSimSection.DirCouplers, "DIR couplers",
            "Interlayer inhibition strength; 0 disables the DIR pass", 0f, 1f, 0f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersDiffusionUm, FilmSimSection.DirCouplers, "Inhibitor spread",
            "Inhibitor diffusion radius", 0f, 100f, 20f, 0.5f, 1, " um"),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersDiffusionTailUm, FilmSimSection.DirCouplers, "Spread tail",
            "Long-range tail diffusion radius", 0f, 500f, 200f, 1f, 0, " um"),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersDiffusionTailWeight, FilmSimSection.DirCouplers, "Tail weight",
            "Long-range tail contribution", 0f, 1f, 0.06f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersInhibitionSameLayer, FilmSimSection.DirCouplers, "Same-layer inhibition",
            "Self-inhibition strength", 0f, 2f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersInhibitionInterlayer, FilmSimSection.DirCouplers, "Interlayer inhibition",
            "Cross-layer inhibition strength", 0f, 2f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersGammaSameLayerR, FilmSimSection.DirCouplers, "DIR gamma R-R",
            "Red self-coupling calibration", 0f, 1f, 0.336f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersGammaSameLayerG, FilmSimSection.DirCouplers, "DIR gamma G-G",
            "Green self-coupling calibration", 0f, 1f, 0.319f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersGammaSameLayerB, FilmSimSection.DirCouplers, "DIR gamma B-B",
            "Blue self-coupling calibration", 0f, 1f, 0.273f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersGammaRToG, FilmSimSection.DirCouplers, "DIR gamma R-G",
            "Red to green coupling calibration", 0f, 1f, 0.353f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersGammaRToB, FilmSimSection.DirCouplers, "DIR gamma R-B",
            "Red to blue coupling calibration", 0f, 1f, 0.302f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersGammaGToR, FilmSimSection.DirCouplers, "DIR gamma G-R",
            "Green to red coupling calibration", 0f, 1f, 0.154f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersGammaGToB, FilmSimSection.DirCouplers, "DIR gamma G-B",
            "Green to blue coupling calibration", 0f, 1f, 0.353f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersGammaBToR, FilmSimSection.DirCouplers, "DIR gamma B-R",
            "Blue to red coupling calibration", 0f, 1f, 0.168f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.DirCouplersGammaBToG, FilmSimSection.DirCouplers, "DIR gamma B-G",
            "Blue to green coupling calibration", 0f, 1f, 0.226f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.ScannerWhiteLevel, FilmSimSection.Scanner, "White level",
            "Scanner white reference", 0.5f, 1.2f, 0.98f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.ScannerBlackLevel, FilmSimSection.Scanner, "Black level",
            "Scanner black reference", 0f, 0.1f, 0.01f, 0.001f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.GlarePercent, FilmSimSection.Scanner, "Viewing glare",
            "Print glare amount (print finals)", 0f, 0.5f, 0.03f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.GlareRoughness, FilmSimSection.Scanner, "Glare roughness",
            "Glare variation", 0f, 2f, 0.7f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.GlareBlur, FilmSimSection.Scanner, "Glare blur",
            "Glare spread in pixels", 0f, 5f, 0.5f, 0.05f, 2, " px"),
        FilmSimNumericSpec(FilmSimNumericParameter.ScannerMtf50LpMm, FilmSimSection.Scanner, "Scanner blur",
            "Scanner MTF blur; 0 disables", 0f, 200f, 60f, 1f, 0, " lp/mm"),
        FilmSimNumericSpec(FilmSimNumericParameter.ScannerUnsharpRadiusUm, FilmSimSection.Scanner, "Scanner sharpness",
            "Unsharp mask radius; 0 disables", 0f, 50f, 5f, 0.5f, 1, " um"),
        FilmSimNumericSpec(FilmSimNumericParameter.ScannerUnsharpAmount, FilmSimSection.Scanner, "Sharpen strength",
            "Unsharp mask strength; 0 disables", 0f, 3f, 0.7f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.ScatterAmount, FilmSimSection.Halation, "Scatter amount",
            "Near-emulsion scatter strength; 0 disables scatter", 0f, 2f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.ScatterScale, FilmSimSection.Halation, "Scatter size",
            "Near-emulsion scatter radius scale", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationAmount, FilmSimSection.Halation, "Halation amount",
            "Film-base bounce strength; 0 disables bounce", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationScale, FilmSimSection.Halation, "Halation size",
            "Film-base bounce radius scale", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationStrengthR, FilmSimSection.Halation, "Halation strength R",
            "Red bounce coupling; default uses the stock preset", 0f, 1f, 0.05f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationStrengthG, FilmSimSection.Halation, "Halation strength G",
            "Green bounce coupling; default uses the stock preset", 0f, 1f, 0.015f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationStrengthB, FilmSimSection.Halation, "Halation strength B",
            "Blue bounce coupling; default uses the stock preset", 0f, 1f, 0f, 0.005f, 3),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationFirstSigmaUmR, FilmSimSection.Halation, "Halation sigma R",
            "Red first-bounce radius; stock-driven when available", 1f, 200f, 65f, 1f, 0, " um"),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationFirstSigmaUmG, FilmSimSection.Halation, "Halation sigma G",
            "Green first-bounce radius; stock-driven when available", 1f, 200f, 65f, 1f, 0, " um"),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationFirstSigmaUmB, FilmSimSection.Halation, "Halation sigma B",
            "Blue first-bounce radius; stock-driven when available", 1f, 200f, 65f, 1f, 0, " um"),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationBoostEv, FilmSimSection.Halation, "Highlight boost",
            "Extra highlight exposure into scatter; 0 disables", 0f, 20f, 0f, 0.1f, 1, " EV"),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationBoostRange, FilmSimSection.Halation, "Boost range",
            "How quickly highlight boost ramps above protection", 0f, 1f, 0.3f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.HalationProtectEv, FilmSimSection.Halation, "Boost protect",
            "Stops above midgray shielded from boost", 0f, 10f, 4f, 0.1f, 1, " EV"),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraDiffusionStrength, FilmSimSection.Diffusion, "Diffusion strength",
            "Filter glow strength; 0 disables", 0f, 2f, 0.5f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraDiffusionSpatialScale, FilmSimSection.Diffusion, "Diffusion size",
            "Glow radius scale", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraDiffusionHaloWarmth, FilmSimSection.Diffusion, "Halo warmth",
            "Warm/cool tilt of the halo", -1.5f, 1.5f, 0f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraDiffusionCoreIntensity, FilmSimSection.Diffusion, "Core intensity",
            "Near-glow contribution", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraDiffusionCoreSize, FilmSimSection.Diffusion, "Core size",
            "Near-glow radius multiplier", 0.1f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraDiffusionHaloIntensity, FilmSimSection.Diffusion, "Halo intensity",
            "Mid-glow contribution", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraDiffusionHaloSize, FilmSimSection.Diffusion, "Halo size",
            "Mid-glow radius multiplier", 0.1f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraDiffusionBloomIntensity, FilmSimSection.Diffusion, "Bloom intensity",
            "Wide-glow contribution", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.CameraDiffusionBloomSize, FilmSimSection.Diffusion, "Bloom size",
            "Wide-glow radius multiplier", 0.1f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintDiffusionStrength, FilmSimSection.Diffusion, "Print diffusion",
            "Enlarger glow strength; 0 disables (print workflow only)", 0f, 2f, 0.5f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintDiffusionSpatialScale, FilmSimSection.Diffusion, "Print diffusion scale",
            "Enlarger glow radius scale", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintDiffusionHaloWarmth, FilmSimSection.Diffusion, "Print halo warmth",
            "Warm/cool tilt of the enlarger halo", -1.5f, 1.5f, 0f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintDiffusionCoreIntensity, FilmSimSection.Diffusion, "Print core intensity",
            "Enlarger near-glow contribution", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintDiffusionCoreSize, FilmSimSection.Diffusion, "Print core size",
            "Enlarger near-glow radius multiplier", 0.1f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintDiffusionHaloIntensity, FilmSimSection.Diffusion, "Print halo intensity",
            "Enlarger mid-glow contribution", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintDiffusionHaloSize, FilmSimSection.Diffusion, "Print halo size",
            "Enlarger mid-glow radius multiplier", 0.1f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintDiffusionBloomIntensity, FilmSimSection.Diffusion, "Print bloom intensity",
            "Enlarger wide-glow contribution", 0f, 4f, 1f, 0.01f, 2),
        FilmSimNumericSpec(FilmSimNumericParameter.PrintDiffusionBloomSize, FilmSimSection.Diffusion, "Print bloom size",
            "Enlarger wide-glow radius multiplier", 0.1f, 4f, 1f, 0.01f, 2)
    )

    fun forParameter(parameter: FilmSimNumericParameter): FilmSimNumericSpec =
        all.first { it.parameter == parameter }
}

// Stock catalogs. Index order MUST match the native profile tables
// (ofx_stock_lists.py FILMS/PAPERS order).
object FilmStocks {
    val films: List<String> = listOf(
        "Ektar 100", "Portra 160", "Portra 400", "Portra 800", "Portra 800 +1",
        "Portra 800 +2", "Gold 200", "Ultramax 400", "Vision3 50D", "Vision3 250D",
        "Verita 200D", "Vision3 200T", "Vision3 500T", "Pro 400H", "C200", "Xtra 400",
        "Ektachrome 100", "Kodachrome 64", "Velvia 100", "Provia 100F"
    )
    val papers: List<String> = listOf(
        "Endura Premier", "Ultra Endura", "Ektacolor Edge", "Supra Endura",
        "Portra Endura", "Crystal Archive II", "2383 Print", "2393 Print"
    )
    val spectralMethods: List<String> = listOf("Hanatos 2025", "Mallett 2019", "Hanatos 2026")
    // Reversal (positive) stocks: Ektachrome 100, Kodachrome 64, Velvia 100,
    // Provia 100F. Must match profile `type` fields (checked in review).
    val positiveFilmIndices: Set<Int> = setOf(16, 17, 18, 19)
    val processes: List<String> = listOf("Print simulation", "Scan negative")
    val pushPullModes: List<String> = listOf("Standard", "Experimental")
    val filmFormats: List<String> = listOf(
        "Standard 8", "Super 8", "Standard 16", "Super 16",
        "Standard 35", "Super 35", "Standard 65", "IMAX 70"
    )
    val diffusionFamilies: List<String> = listOf(
        "Glimmerglass", "Black Pro-Mist", "Pro-Mist", "CineBloom"
    )
    val grainModels: List<String> = listOf("Preview", "Production")
    val colorSpaces: List<String> = listOf(
        "ARRI LogC4", "ARRI LogC3 EI800", "BMDFilm Gen5", "DaVinci Intermediate",
        "RED Log3G10", "S-Log3 S-Gamut3", "S-Log3 Cine", "Canon Log2", "Canon Log3",
        "V-Log", "ACES2065-1", "ACEScg", "ACEScct", "ACEScc",
        "Linear Rec.2020", "Linear Rec.709", "Linear P3-D65",
        "sRGB", "Display P3", "ProPhoto RGB", "Adobe RGB", "DCI-P3",
        "P3-D65 G2.2", "P3-D65 G2.6", "Rec.709 G2.2", "Rec.709 G2.4"
    )
    // Preview-verified output spaces. The phone panel expects a
    // display-referred encode, so only the sRGB family suits the preview.
    // (Input is always Linear Rec.709: the film buffer is linear sRGB, so no
    // input choice is offered.)
    val previewSupportedOutputSpaces: Set<Int> = setOf(17, 18, 24, 25)
}

data class FilmPreset(
    val id: String,
    val name: String,
    val look: FilmSimLook
)

/**
 * Explicit preset state: [SettingsValues.selectedFilmPresetId] is the origin
 * preset (never cleared on edit). Dirty = current look differs from origin.
 * Null id = Custom (no origin).
 */
fun SettingsValues.selectedFilmPreset(): FilmPreset? {
    val id = selectedFilmPresetId ?: return null
    return (FilmFactoryPresets.all + filmPresets).firstOrNull { it.id == id }
}

fun SettingsValues.isFilmPresetModified(): Boolean {
    val base = selectedFilmPreset() ?: return false
    return filmSimLook != base.look
}

fun SettingsValues.isUserFilmPreset(id: String): Boolean =
    filmPresets.any { it.id == id }
object FilmFactoryPresets {
    // Shared base: no grain, halation, diffusion or filters; DIR couplers on
    // (inter-layer colour inhibition) without their spatial spread.
    val baseLook: FilmSimLook = FilmSimLook(dirCouplersAmount = 1f, dirCouplersDiffusionUm = 0f)

    val all: List<FilmPreset> = listOf(
        FilmPreset("portra400", "Portra 400 Natural", baseLook),
        FilmPreset(
            "gold200", "Gold 200 Warm",
            baseLook.copy(film = 6, filmExposureEv = 0.3f, printHighlightShape = 0.2f)
        ),
        FilmPreset(
            "ektachrome", "Ektachrome Clean",
            baseLook.copy(film = 16, rgbToRawMethod = 2, printGamma = 1.1f, process = 1)
        ),
        FilmPreset(
            "velvia", "Velvia Vivid",
            baseLook.copy(film = 18, filmExposureEv = -0.2f, printGamma = 1.15f, process = 1)
        )
    )
}
