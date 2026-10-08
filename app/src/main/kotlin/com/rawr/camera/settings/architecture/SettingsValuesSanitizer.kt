package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

/** Validates persisted preference values without changing navigation or runtime state. */
internal fun sanitizeSettingsValues(persisted: SettingsValues, defaults: SettingsValues, capabilities: SettingsCapabilities): SettingsValues {
    val tone = persisted.imageTone

    fun valid(parameter: ImageToneNumericParameter, value: Float, fallback: Float): Float =
        if (parameter.reviewSpec().accepts(value)) value else fallback

    fun validProfileTone(raw: ProfileTone, fallback: ProfileTone): ProfileTone = ProfileTone(
        renderExposure = valid(ImageToneNumericParameter.RenderExposure, raw.renderExposure, fallback.renderExposure),
        blacks = valid(ImageToneNumericParameter.BlackToe, raw.blacks, fallback.blacks),
        shadows = valid(ImageToneNumericParameter.Shadows, raw.shadows, fallback.shadows),
        contrast = valid(ImageToneNumericParameter.Contrast, raw.contrast, fallback.contrast),
        midtones = valid(ImageToneNumericParameter.MidtonePivot, raw.midtones, fallback.midtones),
        highlights = valid(ImageToneNumericParameter.Highlights, raw.highlights, fallback.highlights),
        whites = valid(ImageToneNumericParameter.ShoulderWhitePoint, raw.whites, fallback.whites),
        saturation = valid(ImageToneNumericParameter.Saturation, raw.saturation, fallback.saturation),
        vibrance = valid(ImageToneNumericParameter.Vibrance, raw.vibrance, fallback.vibrance)
    )

    val cleanTone =
        tone.copy(
            renderExposure =
                valid(
                    ImageToneNumericParameter.RenderExposure,
                    tone.renderExposure,
                    defaults.imageTone.renderExposure
                ),
            blacks = valid(ImageToneNumericParameter.BlackToe, tone.blacks, defaults.imageTone.blacks),
            shadows = valid(ImageToneNumericParameter.Shadows, tone.shadows, defaults.imageTone.shadows),
            contrast = valid(ImageToneNumericParameter.Contrast, tone.contrast, defaults.imageTone.contrast),
            midtones =
                valid(
                    ImageToneNumericParameter.MidtonePivot,
                    tone.midtones,
                    defaults.imageTone.midtones
                ),
            highlights =
                valid(
                    ImageToneNumericParameter.Highlights,
                    tone.highlights,
                    defaults.imageTone.highlights
                ),
            whites =
                valid(
                    ImageToneNumericParameter.ShoulderWhitePoint,
                    tone.whites,
                    defaults.imageTone.whites
                ),
            saturation =
                valid(
                    ImageToneNumericParameter.Saturation,
                    tone.saturation,
                    defaults.imageTone.saturation
                ),
            vibrance = valid(ImageToneNumericParameter.Vibrance, tone.vibrance, defaults.imageTone.vibrance),
            colorRenderingStrength =
                valid(
                    ImageToneNumericParameter.ColorRenderingStrength,
                    tone.colorRenderingStrength,
                    defaults.imageTone.colorRenderingStrength
                ),
            wbTemperature =
                valid(
                    ImageToneNumericParameter.WbTemperature,
                    tone.wbTemperature,
                    defaults.imageTone.wbTemperature
                ),
            wbTint = valid(ImageToneNumericParameter.WbTint, tone.wbTint, defaults.imageTone.wbTint),
            jpegQuality =
                valid(
                    ImageToneNumericParameter.JpegQuality,
                    tone.jpegQuality,
                    defaults.imageTone.jpegQuality
                ),
            outputColorSpaceId = defaults.imageTone.outputColorSpaceId,
            transferFunctionId = defaults.imageTone.transferFunctionId,
            jpegChromaSubsamplingId =
                tone.jpegChromaSubsamplingId.takeIf { id ->
                    capabilities.jpegChromaSubsamplingChoices.any {
                        it.id ==
                            id
                    }
                }
                    ?: defaults.imageTone.jpegChromaSubsamplingId
        )
    val cleanRawrBaseInitial = validProfileTone(persisted.rawrBaseTone, defaults.rawrBaseTone)
    // Pre-per-profile-tone payloads carry Neutral rawrBaseTone but real
    // values in the legacy global tone: adopt them for RAWR NTRL once.
    val cleanRawrBase =
        if (cleanRawrBaseInitial == ProfileTone.Neutral) {
            val legacy = ProfileTone(
                renderExposure = cleanTone.renderExposure,
                blacks = cleanTone.blacks,
                shadows = cleanTone.shadows,
                contrast = cleanTone.contrast,
                midtones = cleanTone.midtones,
                highlights = cleanTone.highlights,
                whites = cleanTone.whites,
                saturation = cleanTone.saturation,
                vibrance = cleanTone.vibrance
            )
            if (legacy != ProfileTone.Neutral) legacy else cleanRawrBaseInitial
        } else {
            cleanRawrBaseInitial
        }
    val cleanProfiles = persisted.userLutProfiles.map { profile ->
        profile.copy(tone = validProfileTone(profile.tone, ProfileTone.Neutral))
    }
    val clean =
        persisted.copy(
            imageTone = cleanTone,
            rawrBaseTone = cleanRawrBase,
            srgbTone = validProfileTone(persisted.srgbTone, ProfileTone.Neutral),
            rec709Tone = validProfileTone(persisted.rec709Tone, ProfileTone.Neutral),
            colorRenderProfile = persisted.colorRenderProfile.takeUnless { it == ColorRenderProfile.Rec709 } ?: ColorRenderProfile.RawrBase,
            videoColorRenderProfile = persisted.videoColorRenderProfile.takeUnless { it == ColorRenderProfile.SRgb } ?: ColorRenderProfile.RawrBase,
            userLutProfiles = cleanProfiles,
            multiframeTuning = persisted.multiframeTuning.sanitized(defaults.multiframeTuning),
            photoFccSteps = persisted.photoFccSteps.coerceIn(1, 8),
            photoDefringeStrength = persisted.photoDefringeStrength.coerceIn(0f, 1f),
            photoDefringeEdgeThreshold = persisted.photoDefringeEdgeThreshold.coerceIn(0.005f, 0.2f),
            photoDefringeLumaFloor = persisted.photoDefringeLumaFloor.coerceIn(0f, 0.5f),
            videoDefringeStrength = persisted.videoDefringeStrength.coerceIn(0f, 1f),
            videoDenoiseStrength = persisted.videoDenoiseStrength.coerceIn(0f, 8f),
            videoDenoiseDetail = persisted.videoDenoiseDetail.coerceIn(0f, 1.8f),
            photoHighlightThreshold = persisted.photoHighlightThreshold.coerceIn(0.5f, 2f),
            photoHighlightCompression = persisted.photoHighlightCompression.coerceIn(0f, 300f),
            videoHighlightThreshold = persisted.videoHighlightThreshold.coerceIn(0.5f, 2f),
            videoHighlightCompression = persisted.videoHighlightCompression.coerceIn(0f, 300f),
            // Denoise needs no sanitize entry: DenoiseConfig is valid by
            // construction (Galosh requires a lane). Legacy states are
            // repaired once at the prefs-decode and recipe-parse
            // boundaries via DenoiseConfig.fromLegacy.
        )
    return clean
}
