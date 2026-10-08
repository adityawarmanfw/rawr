package com.rawr.camera.renderer

import com.rawr.camera.integration.CaptureRecipe
import com.rawr.camera.model.TonemapCatalog
import com.rawr.camera.model.valueFor
import com.rawr.camera.model.withValue
import com.rawr.camera.settings.model.*
import com.rawr.camera.settings.preferences.LutProfileCodec
import org.json.JSONObject

internal object RendererRecipe {
    fun encode(v: SettingsValues): String = JSONObject(CaptureRecipe.encode(v)).apply {
        put("editorProfiles", LutProfileCodec.encode(v.userLutProfiles))
    }.toString()

    fun decode(text: String, library: SettingsValues): SettingsValues {
        if (text.isBlank()) return neutral(library)
        val j = JSONObject(text)
        require(j.optInt("version") == 1) { "Unsupported processing recipe version" }
        val t = j.optJSONObject("tone") ?: JSONObject()
        val f = j.optJSONObject("film") ?: JSONObject()
        val d = neutral(library)
        val tone = TonemapCatalog.all.fold(d.imageTone) { acc, descriptor ->
            acc.withValue(
                descriptor,
                t.optDouble(descriptor.jsonKey, d.imageTone.valueFor(descriptor).toDouble()).toFloat()
            )
        }.copy(
            colorRenderingStrength = t.optDouble("colorRenderingStrength", d.imageTone.colorRenderingStrength.toDouble()).toFloat(),
            wbTemperature = t.optDouble("wbTemperature", d.imageTone.wbTemperature.toDouble()).toFloat(),
            wbTint = t.optDouble("wbTint", d.imageTone.wbTint.toDouble()).toFloat(),
            outputColorSpaceId = t.optString("outputColorSpaceId", d.imageTone.outputColorSpaceId),
            transferFunctionId = t.optString("transferFunctionId", d.imageTone.transferFunctionId),
            jpegQuality = t.optDouble("jpegQuality", d.imageTone.jpegQuality.toDouble()).toFloat(),
            jpegChromaSubsamplingId = t.optString("jpegChromaSubsamplingId", d.imageTone.jpegChromaSubsamplingId)
        )
        val film = d.filmSimLook.copy(
            film = f.optInt("film", d.filmSimLook.film),
            paper = f.optInt("paper", d.filmSimLook.paper),
            inputColorSpace = f.optInt("inputColorSpace", d.filmSimLook.inputColorSpace),
            outputColorSpace = f.optInt("outputColorSpace", d.filmSimLook.outputColorSpace),
            rgbToRawMethod = f.optInt("rgbToRawMethod", d.filmSimLook.rgbToRawMethod),
            process = f.optInt("process", d.filmSimLook.process),
            scanNegativeInvert = f.optBoolean("scanNegativeInvert", d.filmSimLook.scanNegativeInvert),
            filmPushPullMode = f.optInt("filmPushPullMode", d.filmSimLook.filmPushPullMode),
            printerLightsGang = f.optBoolean("printerLightsGang", d.filmSimLook.printerLightsGang),
            printerLightCalibration = f.optBoolean("printerLightCalibration", d.filmSimLook.printerLightCalibration),
            grainEnabled = f.optBoolean("grainEnabled", d.filmSimLook.grainEnabled),
            grainModel = f.optInt("grainModel", d.filmSimLook.grainModel),
            filmFormat = f.optInt("filmFormat", d.filmSimLook.filmFormat),
            grainSublayersEnabled = f.optBoolean("grainSublayersEnabled", d.filmSimLook.grainSublayersEnabled),
            grainSubLayerCount = f.optInt("grainSubLayerCount", d.filmSimLook.grainSubLayerCount),
            grainSeed = f.optInt("grainSeed", d.filmSimLook.grainSeed),
            grainAnimate = f.optBoolean("grainAnimate", d.filmSimLook.grainAnimate),
            cameraUvFilterEnabled = f.optBoolean("cameraUvFilterEnabled", d.filmSimLook.cameraUvFilterEnabled),
            cameraIrFilterEnabled = f.optBoolean("cameraIrFilterEnabled", d.filmSimLook.cameraIrFilterEnabled),
            filmExposureEv = f.optDouble("filmExposureEv", d.filmSimLook.filmExposureEv.toDouble()).toFloat(),
            printExposureEv = f.optDouble("printExposureEv", d.filmSimLook.printExposureEv.toDouble()).toFloat(),
            filmPushPullStops = f.optDouble("filmPushPullStops", d.filmSimLook.filmPushPullStops.toDouble()).toFloat(),
            filmGamma = f.optDouble("filmGamma", d.filmSimLook.filmGamma.toDouble()).toFloat(),
            printPushPullStops = f.optDouble("printPushPullStops", d.filmSimLook.printPushPullStops.toDouble()).toFloat(),
            printGamma = f.optDouble("printGamma", d.filmSimLook.printGamma.toDouble()).toFloat(),
            printShadowShape = f.optDouble("printShadowShape", d.filmSimLook.printShadowShape.toDouble()).toFloat(),
            printHighlightShape = f.optDouble("printHighlightShape", d.filmSimLook.printHighlightShape.toDouble()).toFloat(),
            negativeBleachBypassAmount = f.optDouble("negativeBleachBypassAmount", d.filmSimLook.negativeBleachBypassAmount.toDouble()).toFloat(),
            negativeLeucoCyanCoupling = f.optDouble("negativeLeucoCyanCoupling", d.filmSimLook.negativeLeucoCyanCoupling.toDouble()).toFloat(),
            printBleachBypassAmount = f.optDouble("printBleachBypassAmount", d.filmSimLook.printBleachBypassAmount.toDouble()).toFloat(),
            preflashExposure = f.optDouble("preflashExposure", d.filmSimLook.preflashExposure.toDouble()).toFloat(),
            preflashMFilterShift = f.optDouble("preflashMFilterShift", d.filmSimLook.preflashMFilterShift.toDouble()).toFloat(),
            preflashYFilterShift = f.optDouble("preflashYFilterShift", d.filmSimLook.preflashYFilterShift.toDouble()).toFloat(),
            filterC = f.optDouble("filterC", d.filmSimLook.filterC.toDouble()).toFloat(),
            filterMShift = f.optDouble("filterMShift", d.filmSimLook.filterMShift.toDouble()).toFloat(),
            filterYShift = f.optDouble("filterYShift", d.filmSimLook.filterYShift.toDouble()).toFloat(),
            printerLightsR = f.optDouble("printerLightsR", d.filmSimLook.printerLightsR.toDouble()).toFloat(),
            printerLightsG = f.optDouble("printerLightsG", d.filmSimLook.printerLightsG.toDouble()).toFloat(),
            printerLightsB = f.optDouble("printerLightsB", d.filmSimLook.printerLightsB.toDouble()).toFloat(),
            enlargerScale = f.optDouble("enlargerScale", d.filmSimLook.enlargerScale.toDouble()).toFloat(),
            enlargerOffsetXPercent = f.optDouble("enlargerOffsetXPercent", d.filmSimLook.enlargerOffsetXPercent.toDouble()).toFloat(),
            enlargerOffsetYPercent = f.optDouble("enlargerOffsetYPercent", d.filmSimLook.enlargerOffsetYPercent.toDouble()).toFloat(),
            grainAmount = f.optDouble("grainAmount", d.filmSimLook.grainAmount.toDouble()).toFloat(),
            grainSaturation = f.optDouble("grainSaturation", d.filmSimLook.grainSaturation.toDouble()).toFloat(),
            grainParticleAreaUm2 = f.optDouble("grainParticleAreaUm2", d.filmSimLook.grainParticleAreaUm2.toDouble()).toFloat(),
            grainParticleScaleR = f.optDouble("grainParticleScaleR", d.filmSimLook.grainParticleScaleR.toDouble()).toFloat(),
            grainParticleScaleG = f.optDouble("grainParticleScaleG", d.filmSimLook.grainParticleScaleG.toDouble()).toFloat(),
            grainParticleScaleB = f.optDouble("grainParticleScaleB", d.filmSimLook.grainParticleScaleB.toDouble()).toFloat(),
            grainParticleScaleLayer0 = f.optDouble("grainParticleScaleLayer0", d.filmSimLook.grainParticleScaleLayer0.toDouble()).toFloat(),
            grainParticleScaleLayer1 = f.optDouble("grainParticleScaleLayer1", d.filmSimLook.grainParticleScaleLayer1.toDouble()).toFloat(),
            grainParticleScaleLayer2 = f.optDouble("grainParticleScaleLayer2", d.filmSimLook.grainParticleScaleLayer2.toDouble()).toFloat(),
            grainDensityMinR = f.optDouble("grainDensityMinR", d.filmSimLook.grainDensityMinR.toDouble()).toFloat(),
            grainDensityMinG = f.optDouble("grainDensityMinG", d.filmSimLook.grainDensityMinG.toDouble()).toFloat(),
            grainDensityMinB = f.optDouble("grainDensityMinB", d.filmSimLook.grainDensityMinB.toDouble()).toFloat(),
            grainUniformityR = f.optDouble("grainUniformityR", d.filmSimLook.grainUniformityR.toDouble()).toFloat(),
            grainUniformityG = f.optDouble("grainUniformityG", d.filmSimLook.grainUniformityG.toDouble()).toFloat(),
            grainUniformityB = f.optDouble("grainUniformityB", d.filmSimLook.grainUniformityB.toDouble()).toFloat(),
            grainFinalBlurUm = f.optDouble("grainFinalBlurUm", d.filmSimLook.grainFinalBlurUm.toDouble()).toFloat(),
            grainBlurDyeCloudsUm = f.optDouble("grainBlurDyeCloudsUm", d.filmSimLook.grainBlurDyeCloudsUm.toDouble()).toFloat(),
            grainMicroStructureScale = f.optDouble("grainMicroStructureScale", d.filmSimLook.grainMicroStructureScale.toDouble()).toFloat(),
            grainMicroStructureSigmaNm = f.optDouble("grainMicroStructureSigmaNm", d.filmSimLook.grainMicroStructureSigmaNm.toDouble()).toFloat(),
            cameraUvCutNm = f.optDouble("cameraUvCutNm", d.filmSimLook.cameraUvCutNm.toDouble()).toFloat(),
            cameraIrCutNm = f.optDouble("cameraIrCutNm", d.filmSimLook.cameraIrCutNm.toDouble()).toFloat(),
            dirCouplersAmount = f.optDouble("dirCouplersAmount", d.filmSimLook.dirCouplersAmount.toDouble()).toFloat().coerceIn(0f, 1f),
            dirCouplersDiffusionUm = f.optDouble("dirCouplersDiffusionUm", d.filmSimLook.dirCouplersDiffusionUm.toDouble()).toFloat(),
            dirCouplersDiffusionTailUm = f.optDouble("dirCouplersDiffusionTailUm", d.filmSimLook.dirCouplersDiffusionTailUm.toDouble()).toFloat(),
            dirCouplersDiffusionTailWeight = f.optDouble("dirCouplersDiffusionTailWeight", d.filmSimLook.dirCouplersDiffusionTailWeight.toDouble()).toFloat(),
            dirCouplersInhibitionSameLayer = f.optDouble("dirCouplersInhibitionSameLayer", d.filmSimLook.dirCouplersInhibitionSameLayer.toDouble()).toFloat(),
            dirCouplersInhibitionInterlayer = f.optDouble("dirCouplersInhibitionInterlayer", d.filmSimLook.dirCouplersInhibitionInterlayer.toDouble()).toFloat(),
            dirCouplersGammaSameLayerR = f.optDouble("dirCouplersGammaSameLayerR", d.filmSimLook.dirCouplersGammaSameLayerR.toDouble()).toFloat(),
            dirCouplersGammaSameLayerG = f.optDouble("dirCouplersGammaSameLayerG", d.filmSimLook.dirCouplersGammaSameLayerG.toDouble()).toFloat(),
            dirCouplersGammaSameLayerB = f.optDouble("dirCouplersGammaSameLayerB", d.filmSimLook.dirCouplersGammaSameLayerB.toDouble()).toFloat(),
            dirCouplersGammaRToG = f.optDouble("dirCouplersGammaRToG", d.filmSimLook.dirCouplersGammaRToG.toDouble()).toFloat(),
            dirCouplersGammaRToB = f.optDouble("dirCouplersGammaRToB", d.filmSimLook.dirCouplersGammaRToB.toDouble()).toFloat(),
            dirCouplersGammaGToR = f.optDouble("dirCouplersGammaGToR", d.filmSimLook.dirCouplersGammaGToR.toDouble()).toFloat(),
            dirCouplersGammaGToB = f.optDouble("dirCouplersGammaGToB", d.filmSimLook.dirCouplersGammaGToB.toDouble()).toFloat(),
            dirCouplersGammaBToR = f.optDouble("dirCouplersGammaBToR", d.filmSimLook.dirCouplersGammaBToR.toDouble()).toFloat(),
            dirCouplersGammaBToG = f.optDouble("dirCouplersGammaBToG", d.filmSimLook.dirCouplersGammaBToG.toDouble()).toFloat(),
            scannerEnabled = f.optBoolean("scannerEnabled", d.filmSimLook.scannerEnabled),
            scannerWhiteCorrection = f.optBoolean("scannerWhiteCorrection", d.filmSimLook.scannerWhiteCorrection),
            scannerBlackCorrection = f.optBoolean("scannerBlackCorrection", d.filmSimLook.scannerBlackCorrection),
            scannerWhiteLevel = f.optDouble("scannerWhiteLevel", d.filmSimLook.scannerWhiteLevel.toDouble()).toFloat(),
            scannerBlackLevel = f.optDouble("scannerBlackLevel", d.filmSimLook.scannerBlackLevel.toDouble()).toFloat(),
            glarePercent = f.optDouble("glarePercent", d.filmSimLook.glarePercent.toDouble()).toFloat(),
            glareRoughness = f.optDouble("glareRoughness", d.filmSimLook.glareRoughness.toDouble()).toFloat(),
            glareBlur = f.optDouble("glareBlur", d.filmSimLook.glareBlur.toDouble()).toFloat(),
            scannerMtf50LpMm = f.optDouble("scannerMtf50LpMm", d.filmSimLook.scannerMtf50LpMm.toDouble()).toFloat(),
            scannerUnsharpRadiusUm = f.optDouble("scannerUnsharpRadiusUm", d.filmSimLook.scannerUnsharpRadiusUm.toDouble()).toFloat(),
            scannerUnsharpAmount = f.optDouble("scannerUnsharpAmount", d.filmSimLook.scannerUnsharpAmount.toDouble()).toFloat(),
            halationEnabled = f.optBoolean("halationEnabled", d.filmSimLook.halationEnabled),
            scatterAmount = f.optDouble("scatterAmount", d.filmSimLook.scatterAmount.toDouble()).toFloat(),
            scatterScale = f.optDouble("scatterScale", d.filmSimLook.scatterScale.toDouble()).toFloat(),
            halationAmount = f.optDouble("halationAmount", d.filmSimLook.halationAmount.toDouble()).toFloat(),
            halationScale = f.optDouble("halationScale", d.filmSimLook.halationScale.toDouble()).toFloat(),
            halationStrengthR = f.optDouble("halationStrengthR", d.filmSimLook.halationStrengthR.toDouble()).toFloat(),
            halationStrengthG = f.optDouble("halationStrengthG", d.filmSimLook.halationStrengthG.toDouble()).toFloat(),
            halationStrengthB = f.optDouble("halationStrengthB", d.filmSimLook.halationStrengthB.toDouble()).toFloat(),
            halationFirstSigmaUmR = f.optDouble("halationFirstSigmaUmR", d.filmSimLook.halationFirstSigmaUmR.toDouble()).toFloat(),
            halationFirstSigmaUmG = f.optDouble("halationFirstSigmaUmG", d.filmSimLook.halationFirstSigmaUmG.toDouble()).toFloat(),
            halationFirstSigmaUmB = f.optDouble("halationFirstSigmaUmB", d.filmSimLook.halationFirstSigmaUmB.toDouble()).toFloat(),
            halationBoostEv = f.optDouble("halationBoostEv", d.filmSimLook.halationBoostEv.toDouble()).toFloat(),
            halationBoostRange = f.optDouble("halationBoostRange", d.filmSimLook.halationBoostRange.toDouble()).toFloat(),
            halationProtectEv = f.optDouble("halationProtectEv", d.filmSimLook.halationProtectEv.toDouble()).toFloat(),
            cameraDiffusionEnabled = f.optBoolean("cameraDiffusionEnabled", d.filmSimLook.cameraDiffusionEnabled),
            cameraDiffusionFamily = f.optInt("cameraDiffusionFamily", d.filmSimLook.cameraDiffusionFamily),
            cameraDiffusionStrength = f.optDouble("cameraDiffusionStrength", d.filmSimLook.cameraDiffusionStrength.toDouble()).toFloat(),
            cameraDiffusionSpatialScale = f.optDouble("cameraDiffusionSpatialScale", d.filmSimLook.cameraDiffusionSpatialScale.toDouble()).toFloat(),
            cameraDiffusionHaloWarmth = f.optDouble("cameraDiffusionHaloWarmth", d.filmSimLook.cameraDiffusionHaloWarmth.toDouble()).toFloat(),
            cameraDiffusionCoreIntensity = f.optDouble("cameraDiffusionCoreIntensity", d.filmSimLook.cameraDiffusionCoreIntensity.toDouble()).toFloat(),
            cameraDiffusionCoreSize = f.optDouble("cameraDiffusionCoreSize", d.filmSimLook.cameraDiffusionCoreSize.toDouble()).toFloat(),
            cameraDiffusionHaloIntensity = f.optDouble("cameraDiffusionHaloIntensity", d.filmSimLook.cameraDiffusionHaloIntensity.toDouble()).toFloat(),
            cameraDiffusionHaloSize = f.optDouble("cameraDiffusionHaloSize", d.filmSimLook.cameraDiffusionHaloSize.toDouble()).toFloat(),
            cameraDiffusionBloomIntensity = f.optDouble("cameraDiffusionBloomIntensity", d.filmSimLook.cameraDiffusionBloomIntensity.toDouble()).toFloat(),
            cameraDiffusionBloomSize = f.optDouble("cameraDiffusionBloomSize", d.filmSimLook.cameraDiffusionBloomSize.toDouble()).toFloat(),
            printDiffusionEnabled = f.optBoolean("printDiffusionEnabled", d.filmSimLook.printDiffusionEnabled),
            printDiffusionFamily = f.optInt("printDiffusionFamily", d.filmSimLook.printDiffusionFamily),
            printDiffusionStrength = f.optDouble("printDiffusionStrength", d.filmSimLook.printDiffusionStrength.toDouble()).toFloat(),
            printDiffusionSpatialScale = f.optDouble("printDiffusionSpatialScale", d.filmSimLook.printDiffusionSpatialScale.toDouble()).toFloat(),
            printDiffusionHaloWarmth = f.optDouble("printDiffusionHaloWarmth", d.filmSimLook.printDiffusionHaloWarmth.toDouble()).toFloat(),
            printDiffusionCoreIntensity = f.optDouble("printDiffusionCoreIntensity", d.filmSimLook.printDiffusionCoreIntensity.toDouble()).toFloat(),
            printDiffusionCoreSize = f.optDouble("printDiffusionCoreSize", d.filmSimLook.printDiffusionCoreSize.toDouble()).toFloat(),
            printDiffusionHaloIntensity = f.optDouble("printDiffusionHaloIntensity", d.filmSimLook.printDiffusionHaloIntensity.toDouble()).toFloat(),
            printDiffusionHaloSize = f.optDouble("printDiffusionHaloSize", d.filmSimLook.printDiffusionHaloSize.toDouble()).toFloat(),
            printDiffusionBloomIntensity = f.optDouble("printDiffusionBloomIntensity", d.filmSimLook.printDiffusionBloomIntensity.toDouble()).toFloat(),
            printDiffusionBloomSize = f.optDouble("printDiffusionBloomSize", d.filmSimLook.printDiffusionBloomSize.toDouble()).toFloat()
        )
        require(film.toFloatArray().all { it.isFinite() }) { "Invalid Film Sim recipe" }
        require(listOf(tone.renderExposure, tone.blacks, tone.shadows, tone.contrast, tone.midtones,
            tone.highlights, tone.whites, tone.saturation, tone.vibrance, tone.colorRenderingStrength,
            tone.wbTemperature, tone.wbTint, tone.jpegQuality).all { it.isFinite() }) { "Invalid tone recipe" }
        require(TonemapCatalog.all.all { tone.valueFor(it) in it.minimum..it.maximum } &&
            tone.jpegQuality in 95f..100f &&
            listOf(tone.colorRenderingStrength, tone.wbTemperature, tone.wbTint).all { it in -100f..100f }) {
            "Tone recipe is outside supported ranges"
        }
        val profileTone = ProfileTone(tone.renderExposure, tone.blacks, tone.shadows, tone.contrast,
            tone.midtones, tone.highlights, tone.whites, tone.saturation, tone.vibrance)
        val profiles = if (j.has("editorProfiles")) LutProfileCodec.decode(j.getString("editorProfiles")) else library.userLutProfiles
        val selected = j.optString("importedProfileId").takeUnless { it.isBlank() || it == "null" }
        val embedded = j.optJSONObject("importedProfile")?.let { p ->
            val stages = p.getJSONArray("stages")
            ImportedLutProfile(p.getString("id"), p.getString("name"), List(stages.length()) { i ->
                val stage = stages.getJSONObject(i)
                ImportedLutStage("stage-$i", stage.getString("name"), stage.getString("relativePath"))
            }, LutGamut.valueOf(p.getString("inputGamut")), LutTransfer.valueOf(p.getString("inputTransfer")),
                LutGamut.valueOf(p.getString("outputGamut")), LutTransfer.valueOf(p.getString("outputTransfer")),
                AfterLutAction.valueOf(p.getString("afterLut")), profileTone)
        }
        val profile = when {
            j.optString("colorRenderProfile") == ColorRenderProfile.SRgb.name -> ColorRenderProfile.SRgb
            selected != null -> ColorRenderProfile.UserLut
            else -> ColorRenderProfile.RawrBase
        }
        return d.copy(imageTone = tone,
            colorRenderProfile = profile,
            srgbTone = if (profile == ColorRenderProfile.SRgb) profileTone else d.srgbTone,
            rawrBaseTone = if (profile == ColorRenderProfile.RawrBase) profileTone else d.rawrBaseTone,
            filmSimEnabled = j.optBoolean("filmSimEnabled"), filmSimLook = film,
            selectedUserLutProfileId = selected.takeIf { profile == ColorRenderProfile.UserLut },
            userLutProfiles = (profiles.filterNot { it.id == embedded?.id } + listOfNotNull(embedded)).map {
                if (it.id == selected) it.copy(tone = profileTone) else it
            },
            photoLensShadingEnabled = j.optBoolean("lensShadingCorrectionEnabled", true),
            distortionCorrectionEnabled = j.optBoolean("distortionCorrectionEnabled", true),
            photoHighlightEnabled = j.optBoolean("highlightReconstructionEnabled", true),
            photoHighlightMethod = j.optInt("highlightReconstructionMethod", 0).coerceIn(0, 1),
            photoHighlightThreshold = j.optDouble("highlightThreshold", 1.0).toFloat().coerceIn(0.5f, 2f),
            photoHighlightCompression = j.optDouble("highlightCompression", 100.0).toFloat().coerceIn(0f, 300f),
            ultraHdrEnabled = j.optBoolean("ultraHdrEnabled", d.ultraHdrEnabled),
            demosaicAlgorithm = DemosaicAlgorithm.valueOf(j.optString("demosaicAlgorithm", "Rcd")),
            dualAutoContrast = j.optBoolean("dualAutoContrast", true),
            dualContrastPercent = j.optDouble("dualContrastPercent", 20.0).toFloat().coerceIn(0f, 100f),
            quadfixEnabled = j.optBoolean("quadfixEnabled", false),
            quadfixFastMedian = j.optBoolean("quadfixFastMedian", false),
            photoDefringeEdgeThreshold = j.optDouble("defringeEdgeThreshold", .02).toFloat(),
            photoDefringeLumaFloor = j.optDouble("defringeLumaFloor", .08).toFloat(),
            photoFccSteps = j.optInt("fccSteps", 1), photoDefringeEnabled = j.optBoolean("defringeEnabled", true),
            photoDefringeStrength = j.optDouble("defringeStrength", 1.0).toFloat(),
            photoDenoise = DenoiseConfig.fromLegacy(
                enabled = j.optBoolean("denoiseEnabled", false),
                method = j.optInt("denoiseMethod", 0),
                waveletStrength = j.optDouble("denoiseStrength", 1.0).toFloat(),
                waveletDetail = j.optDouble("denoiseDetail", 1.0).toFloat(),
                waveletLuma = j.optDouble("denoiseLuma", 0.25).toFloat(),
                waveletScales = j.optInt("denoiseScales", 7),
                rawMode = j.optInt("galoshRawMode", 0),
                rawStrength = j.optDouble("galoshStrength", 1.0).toFloat(),
                rawLuma = j.optDouble("galoshLuma", 1.0).toFloat(),
                rawChroma = j.optDouble("galoshChroma", 1.0).toFloat(),
                yuvMode = j.optInt("galoshYuvMode", 0),
                yuvStrengthY = j.optDouble("galoshYuvStrengthY", 1.0).toFloat(),
                yuvStrengthC = j.optDouble("galoshYuvStrengthC", 1.0).toFloat()
            ))
    }
    fun neutral(library: SettingsValues): SettingsValues = SettingsCatalog.initialState().values.copy(
        userLutProfiles = library.userLutProfiles, filmPresets = library.filmPresets,
        selectedUserLutProfileId = null, filmSimEnabled = false, rawrBaseTone = ProfileTone.Neutral,
        photoLensShadingEnabled = true, distortionCorrectionEnabled = true,
        saveLocationId = library.saveLocationId)
}
