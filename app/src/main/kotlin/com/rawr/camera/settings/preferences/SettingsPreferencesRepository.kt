package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.ProfileTone
import com.rawr.camera.settings.model.VideoLogProfile

import android.content.Context
import androidx.datastore.preferences.core.MutablePreferences
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import com.rawr.camera.model.CaptureControlLayout
import com.rawr.camera.model.ControlSurfaceStyle
import com.rawr.camera.model.ImageToneState
import com.rawr.camera.model.OverlayMode
import com.rawr.camera.model.ScopeType
import com.rawr.camera.model.TonemapControlContract
import com.rawr.camera.settings.model.DenoiseConfig
import com.rawr.camera.settings.model.MultiframeTuning
import com.rawr.camera.settings.model.SettingsValues
import com.rawr.camera.settings.model.VideoBitrateMode
import com.rawr.camera.settings.model.VideoEncoderConfig
import com.rawr.camera.settings.model.toLegacy
import java.io.IOException
import kotlin.math.abs
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.map

private val Context.captureSettingsDataStore by preferencesDataStore(name = "capture_settings")

/** Durable storage for global user preferences only. */
class SettingsPreferencesRepository(context: Context, private val defaults: SettingsValues) {
    private val dataStore = context.applicationContext.captureSettingsDataStore

    val values: Flow<SettingsValues> =
        dataStore.data
            .catch { error ->
                if (error is IOException) emit(emptyPreferences()) else throw error
            }.map(::decode)

    suspend fun save(values: SettingsValues) {
        dataStore.edit { prefs -> encode(prefs, values) }
    }

    private fun decode(p: Preferences): SettingsValues {
        val storedVersion = p[Keys.schemaVersion] ?: LEGACY_SCHEMA_VERSION
        if (storedVersion > CURRENT_SCHEMA_VERSION || storedVersion < LEGACY_SCHEMA_VERSION) return defaults
        val decoded = decodeKnownFields(p)
        return migrateDecodedValues(
            storedVersion,
            decoded,
            hasPersistedSaturation = p[Keys.saturation] != null,
            hasPersistedVibrance = p[Keys.vibrance] != null
        )
    }

    /**
     * Explicit migration boundary for stable IDs/enums persisted across app versions. Version 0 is
     * the pre-versioned v0.7 layout and is structurally identical to v1, so its migration is
     * intentionally identity. Future permanent ID renames/removals belong here, never in runtime
     * capability reconciliation.
     */
    private fun migrateDecodedValues(
        fromVersion: Int,
        decoded: SettingsValues,
        hasPersistedSaturation: Boolean,
        hasPersistedVibrance: Boolean
    ): SettingsValues {
        var version = fromVersion
        var values = decoded
        while (version < CURRENT_SCHEMA_VERSION) {
            values =
                when (version) {
                    0 -> {
                        values
                    }

                    // v0 -> v1: establish schema version; no semantic key changes.
                    1 -> {
                        values.copy(
                            imageTone =
                                values.imageTone.copy(
                                    blacks =
                                        if (values.imageTone.blacks ==
                                            0f
                                        ) {
                                            defaults.imageTone.blacks
                                        } else {
                                            values.imageTone.blacks
                                        },
                                    contrast =
                                        if (values.imageTone.contrast ==
                                            0f
                                        ) {
                                            defaults.imageTone.contrast
                                        } else {
                                            values.imageTone.contrast
                                        },
                                    whites =
                                        if (values.imageTone.whites ==
                                            0.75f
                                        ) {
                                            defaults.imageTone.whites
                                        } else {
                                            values.imageTone.whites
                                        },
                                    saturation =
                                        if (values.imageTone.saturation ==
                                            0f
                                        ) {
                                            defaults.imageTone.saturation
                                        } else {
                                            values.imageTone.saturation
                                        }
                                )
                        )
                    }

                    2 -> {
                        values
                    }

                    // v2 -> v3: add RAW AE boolean; absent key decodes to the existing default (off).
                    3 -> {
                        values.copy(
                            imageTone =
                                values.imageTone.copy(
                                    outputColorSpaceId = "output.srgb",
                                    transferFunctionId = "transfer.srgb",
                                    jpegQuality = values.imageTone.jpegQuality.takeIf { it in 95f..100f } ?: 98f,
                                    jpegChromaSubsamplingId = "jpeg.420"
                                )
                        )
                    }

                    4 -> {
                        values
                    }

                    // v4 -> v5: add durable capture-screen presentation + debug preferences.
                    5 -> {
                        values
                    }

                    // v5 -> v6: add still-capture demosaic selection; absent key defaults to RCD.
                    6 -> {
                        values.copy(
                            imageTone =
                                values.imageTone.copy(
                                    shadows = 0f,
                                    contrast = 0f,
                                    midtones = 0f,
                                    highlights = 0f
                                )
                        )
                    }

                    // v6 -> v7: Tonemap v2 creative controls use conventional -100..+100 semantics.
                    7 -> {
                        values
                    }

                    // v7 -> v8: add Dual auto-threshold + remembered manual threshold.
                    8 -> {
                        values.copy(
                            imageTone =
                                values.imageTone.copy(
                                    blacks =
                                        TonemapControlContract.blackUiFromLegacyNative(
                                            values.imageTone.blacks
                                        ),
                                    whites =
                                        TonemapControlContract.whiteUiFromLegacyNative(
                                            values.imageTone.whites
                                        )
                                )
                        )
                    }

                    // v8 -> v9: expose Blacks/Whites as normalized photographic controls.
                    9 -> {
                        values
                    }

                    // v9 -> v10: add Experimental Zero Copy debug preference; absent key defaults off.
                    10 -> {
                        values
                    }

                    // v10 -> v11: gate persistent diagnostic files; absent key defaults off.
                    11 -> {
                        values.copy(
                            imageTone =
                                migrateV11ColorControls(
                                    values.imageTone,
                                    defaults.imageTone,
                                    hasPersistedSaturation,
                                    hasPersistedVibrance
                                )
                        )
                    }

                    // v11 -> v12: color controls become conventional -100..+100 semantics.
                    12 -> {
                        values
                    }

                    // v12 -> v13: add persisted FCC steps; absent key preserves FCC1 behavior.
                    13 -> {
                        values
                    }

                    // v13 -> v14: add user LUT profiles + lens shading; absent keys preserve defaults.
                    14 -> {
                        values
                    }

                    // v14 -> v15: add app-local GPU driver selection; absent keys default disabled.
                    15 -> {
                        values
                    }

                    // v15 -> v16: add selectable RAW AE strategy; absent key preserves Rawr strategy.
                    16 -> {
                        values
                    }

                    // v16 -> v17: add persisted multiframe tuning; absent keys retain validated defaults.
                    17 -> {
                        values
                    }

                    // v17 -> v18: remove the obsolete Rawr/Hybrid strategy selector.
                    18 -> {
                        values
                    }

                    // v18 -> v19: add opt-in Camera2 noise model; absent key defaults off.
                    19 -> {
                        values
                    }

                    // v19 -> v20: replace the Camera2 toggle with a three-way noise-profile selector.
                    20 -> {
                        values.copy(
                            multiframeTuning = values.multiframeTuning.sanitized(defaults.multiframeTuning)
                        )
                    }

                    // v21 -> v22: bandwidth/coverage knobs plus the researched
                    // operating point. Absent keys take the new defaults.
                    21 -> {
                        values.copy(
                            multiframeTuning = values.multiframeTuning.sanitized(defaults.multiframeTuning)
                        )
                    }

                    // v22 -> v23: TONE is per-profile. Seed RAWR NTRL's tone
                    // from the legacy global tone; LUT profiles start neutral.
                    // Decoded payloads without the new keys already fall back
                    // the same way in decodeKnownFields.
                    22 -> {
                        values.copy(rawrBaseTone = values.profileToneFromLegacyImageTone())
                    }

                    // v23 -> v24: add global multiframe base-frame mode; absent key defaults to Middle.
                    23 -> {
                        values
                    }

                    // v24 -> v25: add persistent render-engine toggle; absent key defaults off.
                    24 -> {
                        values
                    }

                    // v25 -> v26: add capture control layout (Classic rails vs
                    // Compact V2 buttons); absent key defaults to Classic.
                    25 -> {
                        values
                    }

                    // v26 -> v27: Compact layout + Basic surface become the
                    // default; existing installs move to the new defaults one-time.
                    26 -> {
                        values.copy(
                            controlSurfaceStyle = ControlSurfaceStyle.Basic,
                            captureControlLayout = CaptureControlLayout.Compact
                        )
                    }

                    // v27 -> v28: add profiled wavelet denoise toggle + raw
                    // strength/detail; absent keys default off.
                    27 -> {
                        values
                    }

                    // v28 -> v29: add capture mode + video resolution/fps;
                    // absent keys default to Photo / 1080p / 30.
                    28 -> {
                        values
                    }

                    // v29 -> v30: decouple Photo/Video image stages. Photo
                    // inherits the legacy shared values (mapped in
                    // decodeKnownFields); every video enable is forced off
                    // so existing users keep their stills behavior and opt
                    // video stages in explicitly. Video strengths were
                    // seeded from the legacy values at decode time.
                    29 -> {
                        values.copy(
                            videoFccEnabled = false,
                            videoDefringeEnabled = false,
                            videoDenoiseEnabled = false,
                            videoLensShadingEnabled = false,
                            videoHighlightEnabled = false
                        )
                    }

                    // v30 -> v31: multiframe retune. Earlier tuning was found
                    // against noise models that overstated variance ~20-400x
                    // and a merge with partial LK coverage, so saved values no
                    // longer mean what they did. Reset the tuning (keeping the
                    // chosen output resolution); noise is now always burst-fitted.
                    30 -> {
                        values.copy(
                            multiframeTuning =
                                defaults.multiframeTuning.copy(
                                    outputResolution = values.multiframeTuning.outputResolution
                                )
                        )
                    }

                    // v31 -> v32: the JPEG path now demosaics the merged CFA,
                    // which wants narrower merge kernels (kDetail .22 -> .15,
                    // kStretch 3.5 -> 2). Only values still at the old
                    // defaults move; hand-tuned ones are kept.
                    31 -> {
                        val t = values.multiframeTuning
                        values.copy(
                            multiframeTuning =
                                t.copy(
                                    kDetail = if (abs(t.kDetail - .22f) < 1e-4f) .15f else t.kDetail,
                                    kStretch = if (abs(t.kStretch - 3.5f) < 1e-4f) 2f else t.kStretch
                                )
                        )
                    }

                    // v32 -> v33: keep tonemap / post-demosaic engines between
                    // shots by default. Rebuilding them cost ~390 ms per still
                    // (pipeline compile); pixel memory is still freed per shot.
                    32 -> {
                        values.copy(persistentEngineEnabled = true)
                    }

                    33 -> migrateV33RenderProfiles(values)
                    // v34 -> v35: user lens profiles; absent means the device's built-in lenses.
                    34 -> values
                    else -> {
                        return defaults
                    }
                }
            version++
        }
        return values
    }

    private fun decodeKnownFields(p: Preferences): SettingsValues {
        val d = defaults
        val tone = d.imageTone
        // v23 keys; absent (pre-v23 installs) → seed RAWR NTRL from the
        // legacy global tone so existing looks are preserved.
        val hasRawrTone = p[Keys.rawrToneRenderExposure] != null
        val legacyTone =
            ImageToneState(
                renderExposure = p[Keys.renderExposure] ?: tone.renderExposure,
                blacks = p[Keys.blacks] ?: tone.blacks,
                shadows = p[Keys.shadows] ?: tone.shadows,
                contrast = p[Keys.contrast] ?: tone.contrast,
                midtones = p[Keys.midtones] ?: tone.midtones,
                highlights = p[Keys.highlights] ?: tone.highlights,
                whites = p[Keys.whites] ?: tone.whites,
                saturation = p[Keys.saturation] ?: tone.saturation,
                vibrance = p[Keys.vibrance] ?: tone.vibrance,
                colorRenderingStrength = p[Keys.colorRenderingStrength] ?: tone.colorRenderingStrength,
                wbTemperature = p[Keys.wbTemperature] ?: tone.wbTemperature,
                wbTint = p[Keys.wbTint] ?: tone.wbTint,
                outputColorSpaceId = p[Keys.outputColorSpaceId] ?: tone.outputColorSpaceId,
                transferFunctionId = p[Keys.transferFunctionId] ?: tone.transferFunctionId,
                jpegQuality = p[Keys.jpegQuality] ?: tone.jpegQuality,
                jpegChromaSubsamplingId = p[Keys.jpegChromaSubsamplingId] ?: tone.jpegChromaSubsamplingId
            )
        val rawrBaseTone =
            if (hasRawrTone) {
                com.rawr.camera.settings.model.ProfileTone(
                    renderExposure = p[Keys.rawrToneRenderExposure] ?: d.rawrBaseTone.renderExposure,
                    blacks = p[Keys.rawrToneBlacks] ?: d.rawrBaseTone.blacks,
                    shadows = p[Keys.rawrToneShadows] ?: d.rawrBaseTone.shadows,
                    contrast = p[Keys.rawrToneContrast] ?: d.rawrBaseTone.contrast,
                    midtones = p[Keys.rawrToneMidtones] ?: d.rawrBaseTone.midtones,
                    highlights = p[Keys.rawrToneHighlights] ?: d.rawrBaseTone.highlights,
                    whites = p[Keys.rawrToneWhites] ?: d.rawrBaseTone.whites,
                    saturation = p[Keys.rawrToneSaturation] ?: d.rawrBaseTone.saturation,
                    vibrance = p[Keys.rawrToneVibrance] ?: d.rawrBaseTone.vibrance
                )
            } else {
                com.rawr.camera.settings.model.ProfileTone(
                    renderExposure = legacyTone.renderExposure,
                    blacks = legacyTone.blacks,
                    shadows = legacyTone.shadows,
                    contrast = legacyTone.contrast,
                    midtones = legacyTone.midtones,
                    highlights = legacyTone.highlights,
                    whites = legacyTone.whites,
                    saturation = legacyTone.saturation,
                    vibrance = legacyTone.vibrance
                )
            }
        return SettingsValues(
            saveLocationId = p[Keys.saveLocationId] ?: d.saveLocationId,
            falseColorPresetId = p[Keys.falseColorPresetId] ?: d.falseColorPresetId,
            peakingSensitivityId = p[Keys.peakingSensitivityId] ?: d.peakingSensitivityId,
            imageTone = legacyTone,
            selfTimer = enumOrDefault(p[Keys.selfTimer], d.selfTimer),
            locationTagging = p[Keys.locationTagging] ?: d.locationTagging,
            oisEnabledPreference = p[Keys.ois] ?: d.oisEnabledPreference,
            antiFlicker = enumOrDefault(p[Keys.antiFlicker], d.antiFlicker),
            exposureStep = enumOrDefault(p[Keys.exposureStep], d.exposureStep),
            highlightProtection = enumOrDefault(p[Keys.highlightProtection], d.highlightProtection),
            maxPostGainId = p[Keys.maxPostGainId] ?: d.maxPostGainId,
            autoMinFpsId = p[Keys.autoMinFpsId] ?: d.autoMinFpsId,
            controlSurfaceStyle = enumOrDefault(p[Keys.controlSurfaceStyle], d.controlSurfaceStyle),
            captureControlLayout = enumOrDefault(p[Keys.captureControlLayout], d.captureControlLayout),
            jpegEnabled = p[Keys.jpegEnabled] ?: d.jpegEnabled,
            dngEnabled = (p[Keys.dngEnabled] ?: true) || p[Keys.jpegEnabled] == false,
            dngCompressionId = p[Keys.dngCompressionId]?.takeIf { it == "dng.uncompressed" } ?: "dng.lossless",
            saveBaseDng = p[Keys.saveBaseDng] ?: true,
            gridMode = enumOrDefault(p[Keys.gridMode], d.gridMode),
            armedOverlays = decodeArmedOverlays(p, d.armedOverlays),
            falseColorManual = p[Keys.falseColorManual] ?: d.falseColorManual,
            activeScopes = decodeScopes(p[Keys.activeScopes], d.activeScopes),
            waveformMode = enumOrDefault(p[Keys.waveformMode], d.waveformMode),
            colorRenderProfile = enumOrDefault(p[Keys.colorRenderProfile], d.colorRenderProfile),
            rec709Tone = ProfileTone(
                renderExposure = p[Keys.rec709ToneRenderExposure] ?: 0f,
                blacks = p[Keys.rec709ToneBlacks] ?: 0f,
                shadows = p[Keys.rec709ToneShadows] ?: 0f,
                contrast = p[Keys.rec709ToneContrast] ?: 0f,
                midtones = p[Keys.rec709ToneMidtones] ?: 0f,
                highlights = p[Keys.rec709ToneHighlights] ?: 0f,
                whites = p[Keys.rec709ToneWhites] ?: 0f,
                saturation = p[Keys.rec709ToneSaturation] ?: 0f,
                vibrance = p[Keys.rec709ToneVibrance] ?: 0f
            ),
            srgbTone = ProfileTone(
                renderExposure = p[Keys.srgbToneRenderExposure] ?: 0f,
                blacks = p[Keys.srgbToneBlacks] ?: 0f,
                shadows = p[Keys.srgbToneShadows] ?: 0f,
                contrast = p[Keys.srgbToneContrast] ?: 0f,
                midtones = p[Keys.srgbToneMidtones] ?: 0f,
                highlights = p[Keys.srgbToneHighlights] ?: 0f,
                whites = p[Keys.srgbToneWhites] ?: 0f,
                saturation = p[Keys.srgbToneSaturation] ?: 0f,
                vibrance = p[Keys.srgbToneVibrance] ?: 0f
            ),
            videoColorRenderProfile = enumOrDefault(p[Keys.videoColorRenderProfile], d.videoColorRenderProfile),
            videoUserLutProfileId = p[Keys.videoUserLutProfileId]?.takeIf { it.isNotEmpty() },
            videoLogEnabled = p[Keys.videoLogEnabled] ?: false,
            videoLogProfile = enumOrDefault(p[Keys.videoLogProfile], VideoLogProfile.LogC3),
            selectedUserLutProfileId = p[Keys.selectedUserLutProfileId],
            userLutProfiles = LutProfileCodec.decode(p[Keys.userLutProfiles]),
            lensProfiles = LensProfileCodec.decode(p[Keys.lensProfiles])?.takeIf { it.isNotEmpty() },
            rawrBaseTone = rawrBaseTone,
            pipelineDiagnosticsEnabled = p[Keys.pipelineDiagnosticsEnabled] ?: d.pipelineDiagnosticsEnabled,
            experimentalZeroCopyEnabled = p[Keys.experimentalZeroCopyEnabled] ?: d.experimentalZeroCopyEnabled,
            experimentalMultiframeEnabled = p[Keys.experimentalMultiframeEnabled] ?: d.experimentalMultiframeEnabled,
            persistentEngineEnabled = p[Keys.persistentEngineEnabled] ?: d.persistentEngineEnabled,
            filmSimEnabled = p[Keys.filmSimEnabled] ?: d.filmSimEnabled,
            filmPreviewDivisor = (p[Keys.filmPreviewDivisor] ?: d.filmPreviewDivisor).coerceIn(2, 4),
            filmSimLook = FilmSimCodec.decodeLook(p[Keys.filmSimLook]) ?: d.filmSimLook,
            selectedFilmPresetId = p[Keys.selectedFilmPresetId],
            filmPresets = FilmSimCodec.decodePresets(p[Keys.filmPresets]),
            multiframeBaseFrameMode = enumOrDefault(p[Keys.multiframeBaseFrameMode], d.multiframeBaseFrameMode),
            multiframeChromaDenoise = p[Keys.multiframeChromaDenoise] ?: d.multiframeChromaDenoise,
            multiframeTuning =
                MultiframeTuning(
                    outputResolution =
                        enumOrDefault(
                            p[Keys.multiframeOutputResolution],
                            d.multiframeTuning.outputResolution
                        ),
                    mergeAlgorithm =
                        enumOrDefault(p[Keys.multiframeMergeAlgorithm], d.multiframeTuning.mergeAlgorithm),
                    hdrPlusStrength = p[Keys.multiframeHdrPlusStrength] ?: d.multiframeTuning.hdrPlusStrength,
                    maxFrames = p[Keys.multiframeMaxFrames] ?: d.multiframeTuning.maxFrames,
                    lkIterations = p[Keys.multiframeLkIterations] ?: d.multiframeTuning.lkIterations,
                    hessianEpsilonExponent =
                        p[Keys.multiframeHessianExponent] ?: d.multiframeTuning.hessianEpsilonExponent,
                    kDetail = p[Keys.multiframeKDetail] ?: d.multiframeTuning.kDetail,
                    kDenoise = p[Keys.multiframeKDenoise] ?: d.multiframeTuning.kDenoise,
                    dThreshold = p[Keys.multiframeDThreshold] ?: d.multiframeTuning.dThreshold,
                    dTransition = p[Keys.multiframeDTransition] ?: d.multiframeTuning.dTransition,
                    kStretch = p[Keys.multiframeKStretch] ?: d.multiframeTuning.kStretch,
                    kShrink = p[Keys.multiframeKShrink] ?: d.multiframeTuning.kShrink,
                    robustnessT = p[Keys.multiframeRobustnessT] ?: d.multiframeTuning.robustnessT,
                    robustnessS1 = p[Keys.multiframeRobustnessS1] ?: d.multiframeTuning.robustnessS1,
                    robustnessS2 = p[Keys.multiframeRobustnessS2] ?: d.multiframeTuning.robustnessS2,
                    motionThreshold = p[Keys.multiframeMotionThreshold] ?: d.multiframeTuning.motionThreshold,
                    flatSigma = p[Keys.multiframeFlatSigma] ?: d.multiframeTuning.flatSigma,
                    detailFloorSigma = p[Keys.multiframeDetailFloor] ?: d.multiframeTuning.detailFloorSigma,
                    scaleBandwidthGain = p[Keys.multiframeScaleGain] ?: d.multiframeTuning.scaleBandwidthGain,
                    coverageNeffLo = p[Keys.multiframeCoverageNeffLo] ?: d.multiframeTuning.coverageNeffLo,
                    coverageNeffHi = p[Keys.multiframeCoverageNeffHi] ?: d.multiframeTuning.coverageNeffHi,
                    coverageMassLo = p[Keys.multiframeCoverageMassLo] ?: d.multiframeTuning.coverageMassLo,
                    coverageMassHi = p[Keys.multiframeCoverageMassHi] ?: d.multiframeTuning.coverageMassHi,
                    fallbackChroma = p[Keys.multiframeFallbackChroma] ?: d.multiframeTuning.fallbackChroma,
                    fallbackLuma = p[Keys.multiframeFallbackLuma] ?: d.multiframeTuning.fallbackLuma
                ).sanitized(d.multiframeTuning),
            customGpuDriverEnabled = p[Keys.customGpuDriverEnabled] ?: d.customGpuDriverEnabled,
            customGpuDriverName = p[Keys.customGpuDriverName] ?: d.customGpuDriverName,
            persistentDiagnosticsEnabled = p[Keys.persistentDiagnosticsEnabled] ?: d.persistentDiagnosticsEnabled,
            persistZslRingEnabled = p[Keys.persistZslRingEnabled] ?: d.persistZslRingEnabled,
            persistZslRingOnShutterEnabled = p[Keys.persistZslRingOnShutterEnabled] ?: d.persistZslRingOnShutterEnabled,
            internalTraceCaptureEnabled = p[Keys.internalTraceCaptureEnabled] ?: d.internalTraceCaptureEnabled,
            internalTraceRetainedRows =
                (p[Keys.internalTraceRetainedRows] ?: d.internalTraceRetainedRows).takeIf {
                    it in
                        setOf(4096, 8192, 16384, 32768)
                }
                    ?: 8192,
            demosaicAlgorithm = enumOrDefault(p[Keys.demosaicAlgorithm], d.demosaicAlgorithm),
            dualAutoContrast = p[Keys.dualAutoContrast] ?: d.dualAutoContrast,
            dualContrastPercent = (p[Keys.dualContrastPercent] ?: d.dualContrastPercent).coerceIn(0f, 100f),
            quadfixEnabled = p[Keys.quadfixEnabled] ?: d.quadfixEnabled,
            quadfixFastMedian = p[Keys.quadfixFastMedian] ?: d.quadfixFastMedian,
            // v30 split: photo reads the new key, falling back to the
            // legacy shared key for pre-v30 installs. Video strengths
            // seed from the legacy value so enabling video later starts
            // from the user's stills operating point; enables default
            // off (forced off once more in the v29 -> v30 migration).
            photoFccSteps = (p[Keys.photoFccSteps] ?: p[Keys.fccSteps] ?: d.photoFccSteps).coerceIn(1, 8),
            videoFccEnabled = p[Keys.videoFccEnabled] ?: d.videoFccEnabled,
            videoFccSteps = (p[Keys.videoFccSteps] ?: p[Keys.fccSteps] ?: d.videoFccSteps).coerceIn(1, 8),
            photoDefringeEnabled = p[Keys.photoDefringeEnabled] ?: p[Keys.defringeEnabled] ?: d.photoDefringeEnabled,
            photoDefringeStrength = (p[Keys.photoDefringeStrength] ?: p[Keys.defringeStrength] ?: d.photoDefringeStrength).coerceIn(0f, 1f),
            photoDefringeEdgeThreshold =
                (p[Keys.photoDefringeEdgeThreshold] ?: p[Keys.defringeEdgeThreshold] ?: d.photoDefringeEdgeThreshold).coerceIn(0.005f, 0.2f),
            photoDefringeLumaFloor = (p[Keys.photoDefringeLumaFloor] ?: p[Keys.defringeLumaFloor] ?: d.photoDefringeLumaFloor).coerceIn(0f, 0.5f),
            videoDefringeEnabled = p[Keys.videoDefringeEnabled] ?: d.videoDefringeEnabled,
            videoDefringeStrength = (p[Keys.videoDefringeStrength] ?: p[Keys.defringeStrength] ?: d.videoDefringeStrength).coerceIn(0f, 1f),
            videoDefringeEdgeThreshold =
                (p[Keys.videoDefringeEdgeThreshold] ?: p[Keys.defringeEdgeThreshold] ?: d.videoDefringeEdgeThreshold).coerceIn(0.005f, 0.2f),
            videoDefringeLumaFloor = (p[Keys.videoDefringeLumaFloor] ?: p[Keys.defringeLumaFloor] ?: d.videoDefringeLumaFloor).coerceIn(0f, 0.5f),
            // Photo denoise keeps the stable eleven-key schema; video
            // denoise is three new wavelet-only keys (no Galosh on video).
            photoDenoise = run {
                val fallback = d.photoDenoise.toLegacy()
                DenoiseConfig.fromLegacy(
                    enabled = p[Keys.denoiseEnabled] ?: fallback.enabled,
                    method = p[Keys.denoiseMethod] ?: if ((p[Keys.galoshRawMode] ?: 0) != 0 ||
                        (p[Keys.galoshYuvMode] ?: 0) != 0
                    ) 1 else fallback.method,
                    waveletStrength = (p[Keys.denoiseStrength] ?: fallback.waveletStrength).coerceIn(0f, 8f),
                    waveletDetail = (p[Keys.denoiseDetail] ?: fallback.waveletDetail).coerceIn(0f, 1.8f),
                    waveletLuma = (p[Keys.denoiseLuma] ?: fallback.waveletLuma).coerceIn(0f, 1f),
                    waveletScales = (p[Keys.denoiseScales] ?: fallback.waveletScales).coerceIn(1, 7),
                    rawMode = (p[Keys.galoshRawMode] ?: fallback.rawMode).coerceIn(0, 2),
                    rawStrength = (p[Keys.galoshStrength] ?: fallback.rawStrength).coerceIn(0f, 8f),
                    rawLuma = (p[Keys.galoshLuma] ?: fallback.rawLuma).coerceIn(0f, 8f),
                    rawChroma = (p[Keys.galoshChroma] ?: fallback.rawChroma).coerceIn(0f, 8f),
                    yuvMode = (p[Keys.galoshYuvMode] ?: fallback.yuvMode).coerceIn(0, 2),
                    yuvStrengthY = (p[Keys.galoshYuvStrengthY] ?: fallback.yuvStrengthY).coerceIn(0f, 8f),
                    yuvStrengthC = (p[Keys.galoshYuvStrengthC] ?: fallback.yuvStrengthC).coerceIn(0f, 8f)
                )
            },
            videoDenoiseEnabled = p[Keys.videoDenoiseEnabled] ?: d.videoDenoiseEnabled,
            videoDenoiseStrength = (p[Keys.videoDenoiseStrength] ?: p[Keys.denoiseStrength] ?: d.videoDenoiseStrength).coerceIn(0f, 8f),
            videoDenoiseDetail = (p[Keys.videoDenoiseDetail] ?: p[Keys.denoiseDetail] ?: d.videoDenoiseDetail).coerceIn(0f, 1.8f),
            videoDenoiseLuma = (p[Keys.videoDenoiseLuma] ?: p[Keys.denoiseLuma] ?: d.videoDenoiseLuma).coerceIn(0f, 1f),
            videoDenoiseScales = (p[Keys.videoDenoiseScales] ?: p[Keys.denoiseScales] ?: d.videoDenoiseScales).coerceIn(1, 7),
            captureModeId = p[Keys.captureModeId] ?: d.captureModeId,
            videoResolutionId = p[Keys.videoResolutionId] ?: d.videoResolutionId,
            videoFps = p[Keys.videoFps] ?: d.videoFps,
            videoEncoder = VideoEncoderConfig(
                bitDepth = p[Keys.videoBitDepth] ?: d.videoEncoder.bitDepth,
                bitrate1080pMbps = p[Keys.videoBitrate1080pMbps] ?: d.videoEncoder.bitrate1080pMbps,
                bitrate4kMbps = p[Keys.videoBitrate4kMbps] ?: d.videoEncoder.bitrate4kMbps,
                bitrateOpenGateMbps = p[Keys.videoBitrateOpenGateMbps] ?: d.videoEncoder.bitrateOpenGateMbps,
                bitrateMode = p[Keys.videoBitrateMode]?.let(VideoBitrateMode::of) ?: d.videoEncoder.bitrateMode,
                keyframeSeconds = p[Keys.videoKeyframeSeconds] ?: d.videoEncoder.keyframeSeconds,
                maxBFrames = p[Keys.videoMaxBFrames] ?: d.videoEncoder.maxBFrames,
                audioChannels = p[Keys.videoAudioChannels] ?: d.videoEncoder.audioChannels,
                audioBitrateKbps = p[Keys.videoAudioBitrateKbps] ?: d.videoEncoder.audioBitrateKbps
            ).sanitized(),
            photoLensShadingEnabled = p[Keys.photoLensShading] ?: p[Keys.lensShadingCorrection] ?: d.photoLensShadingEnabled,
            videoLensShadingEnabled = p[Keys.videoLensShading] ?: d.videoLensShadingEnabled,
            distortionCorrectionEnabled = p[Keys.distortionCorrection] ?: d.distortionCorrectionEnabled,
            photoHighlightEnabled = p[Keys.photoHighlightEnabled] ?: p[Keys.highlightReconstruction] ?: d.photoHighlightEnabled,
            photoHighlightMethod = (p[Keys.photoHighlightMethod] ?: p[Keys.highlightMethod] ?: d.photoHighlightMethod).coerceIn(0, 1),
            photoHighlightThreshold = (p[Keys.photoHighlightThreshold] ?: p[Keys.highlightThreshold] ?: d.photoHighlightThreshold).coerceIn(0.5f, 2f),
            photoHighlightCompression = (p[Keys.photoHighlightCompression] ?: p[Keys.highlightCompression] ?: d.photoHighlightCompression).coerceIn(0f, 300f),
            videoHighlightEnabled = p[Keys.videoHighlightEnabled] ?: d.videoHighlightEnabled,
            videoHighlightMethod = (p[Keys.videoHighlightMethod] ?: p[Keys.highlightMethod] ?: d.videoHighlightMethod).coerceIn(0, 1),
            videoHighlightThreshold = (p[Keys.videoHighlightThreshold] ?: p[Keys.highlightThreshold] ?: d.videoHighlightThreshold).coerceIn(0.5f, 2f),
            videoHighlightCompression = (p[Keys.videoHighlightCompression] ?: p[Keys.highlightCompression] ?: d.videoHighlightCompression).coerceIn(0f, 300f),
            ultraHdrEnabled = p[Keys.ultraHdr] ?: d.ultraHdrEnabled
        )
    }

    private fun encode(p: MutablePreferences, v: SettingsValues) {
        p[Keys.schemaVersion] = CURRENT_SCHEMA_VERSION
        p[Keys.saveLocationId] = v.saveLocationId
        p[Keys.falseColorPresetId] = v.falseColorPresetId
        p[Keys.peakingSensitivityId] = v.peakingSensitivityId
        // Legacy tone keys mirror RAWR NTRL so pre-v23 readers/downgrades
        // keep a sensible global tone. v23 readers prefer profile_tone_rawr_*.
        p[Keys.renderExposure] = v.rawrBaseTone.renderExposure
        p[Keys.blacks] = v.rawrBaseTone.blacks
        p[Keys.shadows] = v.rawrBaseTone.shadows
        p[Keys.contrast] = v.rawrBaseTone.contrast
        p[Keys.midtones] = v.rawrBaseTone.midtones
        p[Keys.highlights] = v.rawrBaseTone.highlights
        p[Keys.whites] = v.rawrBaseTone.whites
        p[Keys.saturation] = v.rawrBaseTone.saturation
        p[Keys.vibrance] = v.rawrBaseTone.vibrance
        p[Keys.colorRenderingStrength] = v.imageTone.colorRenderingStrength
        p[Keys.wbTemperature] = v.imageTone.wbTemperature
        p[Keys.wbTint] = v.imageTone.wbTint
        p[Keys.outputColorSpaceId] = v.imageTone.outputColorSpaceId
        p[Keys.transferFunctionId] = v.imageTone.transferFunctionId
        p[Keys.jpegQuality] = v.imageTone.jpegQuality
        p[Keys.jpegChromaSubsamplingId] = v.imageTone.jpegChromaSubsamplingId
        p[Keys.selfTimer] = v.selfTimer.name
        p[Keys.locationTagging] = v.locationTagging
        p[Keys.ois] = v.oisEnabledPreference
        p[Keys.antiFlicker] = v.antiFlicker.name
        p[Keys.exposureStep] = v.exposureStep.name
        p[Keys.highlightProtection] = v.highlightProtection.name
        p[Keys.maxPostGainId] = v.maxPostGainId
        p[Keys.autoMinFpsId] = v.autoMinFpsId
        p[Keys.controlSurfaceStyle] = v.controlSurfaceStyle.name
        p[Keys.captureControlLayout] = v.captureControlLayout.name
        p[Keys.jpegEnabled] = v.jpegEnabled
        p[Keys.dngEnabled] = v.dngEnabled || !v.jpegEnabled
        p[Keys.dngCompressionId] = v.dngCompressionId
        p[Keys.saveBaseDng] = v.saveBaseDng
        p[Keys.gridMode] = v.gridMode.name
        if (v.armedOverlays.isEmpty()) p.remove(Keys.armedOverlays)
        else p[Keys.armedOverlays] = v.armedOverlays.map { it.name }.distinct().sorted().joinToString(",")
        p[Keys.falseColorManual] = v.falseColorManual
        p[Keys.activeScopes] = v.activeScopes.distinct().joinToString(",") { it.name }
        p[Keys.waveformMode] = v.waveformMode.name
        p[Keys.videoColorRenderProfile] = v.videoColorRenderProfile.name
        p[Keys.videoUserLutProfileId] = v.videoUserLutProfileId.orEmpty()
        p[Keys.videoLogEnabled] = v.videoLogEnabled
        p[Keys.videoLogProfile] = v.videoLogProfile.name
        p[Keys.colorRenderProfile] = v.colorRenderProfile.name
        p[Keys.rec709ToneRenderExposure] = v.rec709Tone.renderExposure
        p[Keys.rec709ToneBlacks] = v.rec709Tone.blacks
        p[Keys.rec709ToneShadows] = v.rec709Tone.shadows
        p[Keys.rec709ToneContrast] = v.rec709Tone.contrast
        p[Keys.rec709ToneMidtones] = v.rec709Tone.midtones
        p[Keys.rec709ToneHighlights] = v.rec709Tone.highlights
        p[Keys.rec709ToneWhites] = v.rec709Tone.whites
        p[Keys.rec709ToneSaturation] = v.rec709Tone.saturation
        p[Keys.rec709ToneVibrance] = v.rec709Tone.vibrance
        p[Keys.srgbToneRenderExposure] = v.srgbTone.renderExposure
        p[Keys.srgbToneBlacks] = v.srgbTone.blacks
        p[Keys.srgbToneShadows] = v.srgbTone.shadows
        p[Keys.srgbToneContrast] = v.srgbTone.contrast
        p[Keys.srgbToneMidtones] = v.srgbTone.midtones
        p[Keys.srgbToneHighlights] = v.srgbTone.highlights
        p[Keys.srgbToneWhites] = v.srgbTone.whites
        p[Keys.srgbToneSaturation] = v.srgbTone.saturation
        p[Keys.srgbToneVibrance] = v.srgbTone.vibrance
        if (v.selectedUserLutProfileId ==
            null
        ) {
            p.remove(Keys.selectedUserLutProfileId)
        } else {
            p[Keys.selectedUserLutProfileId] =
                v.selectedUserLutProfileId
        }
        p[Keys.userLutProfiles] = LutProfileCodec.encode(v.userLutProfiles)
        val lensProfiles = v.lensProfiles
        if (lensProfiles == null) p.remove(Keys.lensProfiles) else p[Keys.lensProfiles] = LensProfileCodec.encode(lensProfiles)
        p[Keys.rawrToneRenderExposure] = v.rawrBaseTone.renderExposure
        p[Keys.rawrToneBlacks] = v.rawrBaseTone.blacks
        p[Keys.rawrToneShadows] = v.rawrBaseTone.shadows
        p[Keys.rawrToneContrast] = v.rawrBaseTone.contrast
        p[Keys.rawrToneMidtones] = v.rawrBaseTone.midtones
        p[Keys.rawrToneHighlights] = v.rawrBaseTone.highlights
        p[Keys.rawrToneWhites] = v.rawrBaseTone.whites
        p[Keys.rawrToneSaturation] = v.rawrBaseTone.saturation
        p[Keys.rawrToneVibrance] = v.rawrBaseTone.vibrance
        p[Keys.pipelineDiagnosticsEnabled] = v.pipelineDiagnosticsEnabled
        p[Keys.experimentalZeroCopyEnabled] = v.experimentalZeroCopyEnabled
        p[Keys.experimentalMultiframeEnabled] = v.experimentalMultiframeEnabled
        p[Keys.persistentEngineEnabled] = v.persistentEngineEnabled
        p[Keys.filmSimEnabled] = v.filmSimEnabled
        p[Keys.filmPreviewDivisor] = v.filmPreviewDivisor.coerceIn(2, 4)
        p[Keys.filmSimLook] = FilmSimCodec.encodeLook(v.filmSimLook)
        if (v.selectedFilmPresetId == null) {
            p.remove(Keys.selectedFilmPresetId)
        } else {
            p[Keys.selectedFilmPresetId] = v.selectedFilmPresetId
        }
        p[Keys.filmPresets] = FilmSimCodec.encodePresets(v.filmPresets)
        p[Keys.multiframeBaseFrameMode] = v.multiframeBaseFrameMode.name
        p[Keys.multiframeChromaDenoise] = v.multiframeChromaDenoise
        // Noise-profile selection was removed (always burst-fitted).
        p.remove(Keys.multiframeNoiseProfile)
        p.remove(Keys.camera2NoiseModelEnabled)
        p[Keys.multiframeOutputResolution] = v.multiframeTuning.outputResolution.name
        p[Keys.multiframeMergeAlgorithm] = v.multiframeTuning.mergeAlgorithm.name
        p[Keys.multiframeHdrPlusStrength] = v.multiframeTuning.hdrPlusStrength
        p[Keys.multiframeMaxFrames] = v.multiframeTuning.maxFrames
        p[Keys.multiframeLkIterations] = v.multiframeTuning.lkIterations
        p[Keys.multiframeHessianExponent] = v.multiframeTuning.hessianEpsilonExponent
        p[Keys.multiframeKDetail] = v.multiframeTuning.kDetail
        p[Keys.multiframeKDenoise] = v.multiframeTuning.kDenoise
        p[Keys.multiframeDThreshold] = v.multiframeTuning.dThreshold
        p[Keys.multiframeDTransition] = v.multiframeTuning.dTransition
        p[Keys.multiframeKStretch] = v.multiframeTuning.kStretch
        p[Keys.multiframeKShrink] = v.multiframeTuning.kShrink
        p[Keys.multiframeRobustnessT] = v.multiframeTuning.robustnessT
        p[Keys.multiframeRobustnessS1] = v.multiframeTuning.robustnessS1
        p[Keys.multiframeRobustnessS2] = v.multiframeTuning.robustnessS2
        p[Keys.multiframeMotionThreshold] = v.multiframeTuning.motionThreshold
        p[Keys.multiframeFlatSigma] = v.multiframeTuning.flatSigma
        p[Keys.multiframeDetailFloor] = v.multiframeTuning.detailFloorSigma
        p[Keys.multiframeScaleGain] = v.multiframeTuning.scaleBandwidthGain
        p[Keys.multiframeCoverageNeffLo] = v.multiframeTuning.coverageNeffLo
        p[Keys.multiframeCoverageNeffHi] = v.multiframeTuning.coverageNeffHi
        p[Keys.multiframeCoverageMassLo] = v.multiframeTuning.coverageMassLo
        p[Keys.multiframeCoverageMassHi] = v.multiframeTuning.coverageMassHi
        p[Keys.multiframeFallbackChroma] = v.multiframeTuning.fallbackChroma
        p[Keys.multiframeFallbackLuma] = v.multiframeTuning.fallbackLuma
        p[Keys.customGpuDriverEnabled] = v.customGpuDriverEnabled
        if (v.customGpuDriverName ==
            null
        ) {
            p.remove(Keys.customGpuDriverName)
        } else {
            p[Keys.customGpuDriverName] = v.customGpuDriverName
        }
        p[Keys.persistentDiagnosticsEnabled] = v.persistentDiagnosticsEnabled
        p[Keys.persistZslRingEnabled] = v.persistZslRingEnabled
        p[Keys.persistZslRingOnShutterEnabled] = v.persistZslRingOnShutterEnabled
        p[Keys.internalTraceCaptureEnabled] = v.internalTraceCaptureEnabled
        p[Keys.internalTraceRetainedRows] = v.internalTraceRetainedRows
        p[Keys.demosaicAlgorithm] = v.demosaicAlgorithm.name
        p[Keys.dualAutoContrast] = v.dualAutoContrast
        p[Keys.dualContrastPercent] = v.dualContrastPercent.coerceIn(0f, 100f)
        p[Keys.quadfixEnabled] = v.quadfixEnabled
        p[Keys.quadfixFastMedian] = v.quadfixFastMedian
        p[Keys.photoFccSteps] = v.photoFccSteps.coerceIn(1, 8)
        p[Keys.videoFccEnabled] = v.videoFccEnabled
        p[Keys.videoFccSteps] = v.videoFccSteps.coerceIn(1, 8)
        p[Keys.photoDefringeEnabled] = v.photoDefringeEnabled
        p[Keys.photoDefringeStrength] = v.photoDefringeStrength.coerceIn(0f, 1f)
        p[Keys.photoDefringeEdgeThreshold] = v.photoDefringeEdgeThreshold.coerceIn(0.005f, 0.2f)
        p[Keys.photoDefringeLumaFloor] = v.photoDefringeLumaFloor.coerceIn(0f, 0.5f)
        p[Keys.videoDefringeEnabled] = v.videoDefringeEnabled
        p[Keys.videoDefringeStrength] = v.videoDefringeStrength.coerceIn(0f, 1f)
        p[Keys.videoDefringeEdgeThreshold] = v.videoDefringeEdgeThreshold.coerceIn(0.005f, 0.2f)
        p[Keys.videoDefringeLumaFloor] = v.videoDefringeLumaFloor.coerceIn(0f, 0.5f)
        // Photo denoise persists as the same eleven keys (stable DataStore
        // schema); the sealed value decomposes at the boundary.
        val legacyDenoise = v.photoDenoise.toLegacy()
        p[Keys.denoiseEnabled] = legacyDenoise.enabled
        p[Keys.denoiseStrength] = legacyDenoise.waveletStrength.coerceIn(0f, 8f)
        p[Keys.denoiseDetail] = legacyDenoise.waveletDetail.coerceIn(0f, 1.8f)
        p[Keys.denoiseLuma] = legacyDenoise.waveletLuma.coerceIn(0f, 1f)
        p[Keys.denoiseScales] = legacyDenoise.waveletScales.coerceIn(1, 7)
        p[Keys.denoiseMethod] = legacyDenoise.method.coerceIn(0, 1)
        p[Keys.galoshRawMode] = legacyDenoise.rawMode.coerceIn(0, 2)
        p[Keys.galoshStrength] = legacyDenoise.rawStrength.coerceIn(0f, 8f)
        p[Keys.galoshLuma] = legacyDenoise.rawLuma.coerceIn(0f, 8f)
        p[Keys.galoshChroma] = legacyDenoise.rawChroma.coerceIn(0f, 8f)
        p[Keys.galoshYuvMode] = legacyDenoise.yuvMode.coerceIn(0, 2)
        p[Keys.galoshYuvStrengthY] = legacyDenoise.yuvStrengthY.coerceIn(0f, 8f)
        p[Keys.galoshYuvStrengthC] = legacyDenoise.yuvStrengthC.coerceIn(0f, 8f)
        p[Keys.videoDenoiseEnabled] = v.videoDenoiseEnabled
        p[Keys.videoDenoiseStrength] = v.videoDenoiseStrength.coerceIn(0f, 8f)
        p[Keys.videoDenoiseDetail] = v.videoDenoiseDetail.coerceIn(0f, 1.8f)
        p[Keys.videoDenoiseLuma] = v.videoDenoiseLuma.coerceIn(0f, 1f)
        p[Keys.videoDenoiseScales] = v.videoDenoiseScales.coerceIn(1, 7)
        p[Keys.captureModeId] = v.captureModeId
        p[Keys.videoResolutionId] = v.videoResolutionId
        p[Keys.videoFps] = v.videoFps
        val encoder = v.videoEncoder.sanitized()
        p[Keys.videoBitDepth] = encoder.bitDepth
        p[Keys.videoBitrate1080pMbps] = encoder.bitrate1080pMbps
        p[Keys.videoBitrate4kMbps] = encoder.bitrate4kMbps
        p[Keys.videoBitrateOpenGateMbps] = encoder.bitrateOpenGateMbps
        p[Keys.videoBitrateMode] = encoder.bitrateMode.wireValue
        p[Keys.videoKeyframeSeconds] = encoder.keyframeSeconds
        p[Keys.videoMaxBFrames] = encoder.maxBFrames
        p[Keys.videoAudioChannels] = encoder.audioChannels
        p[Keys.videoAudioBitrateKbps] = encoder.audioBitrateKbps
        p[Keys.photoLensShading] = v.photoLensShadingEnabled
        p[Keys.videoLensShading] = v.videoLensShadingEnabled
        p[Keys.distortionCorrection] = v.distortionCorrectionEnabled
        p[Keys.photoHighlightEnabled] = v.photoHighlightEnabled
        p[Keys.photoHighlightMethod] = v.photoHighlightMethod
        p[Keys.photoHighlightThreshold] = v.photoHighlightThreshold
        p[Keys.photoHighlightCompression] = v.photoHighlightCompression
        p[Keys.videoHighlightEnabled] = v.videoHighlightEnabled
        p[Keys.videoHighlightMethod] = v.videoHighlightMethod
        p[Keys.videoHighlightThreshold] = v.videoHighlightThreshold
        p[Keys.videoHighlightCompression] = v.videoHighlightCompression
        p[Keys.ultraHdr] = v.ultraHdrEnabled
        // Drop pre-v30 shared keys once the split values are written.
        p.remove(Keys.fccSteps)
        p.remove(Keys.defringeEnabled)
        p.remove(Keys.defringeStrength)
        p.remove(Keys.defringeEdgeThreshold)
        p.remove(Keys.defringeLumaFloor)
        p.remove(Keys.lensShadingCorrection)
        p.remove(Keys.highlightReconstruction)
        p.remove(Keys.highlightMethod)
        p.remove(Keys.highlightThreshold)
        p.remove(Keys.highlightCompression)
    }

    private inline fun <reified T : Enum<T>> enumOrDefault(raw: String?, default: T): T =
        raw?.let { value -> enumValues<T>().firstOrNull { it.name == value } } ?: default

    private fun decodeArmedOverlays(p: Preferences, default: Set<OverlayMode>): Set<OverlayMode> {
        val raw = p[Keys.armedOverlays]
        if (raw != null) {
            if (raw.isBlank()) return emptySet()
            return raw.split(',').mapNotNull { token ->
                OverlayMode.entries.firstOrNull { it.name == token && it != OverlayMode.FalseColor }
            }.toSet()
        }
        // Legacy single-overlay migration (pre auto-gate builds).
        val legacy = p[Keys.overlayMode] ?: return default
        val mapped = if (legacy == "RawShadows") OverlayMode.TonemapShadows.name else legacy
        return OverlayMode.entries.firstOrNull { it.name == mapped && it != OverlayMode.FalseColor }
            ?.let { setOf(it) } ?: default
    }

    private fun decodeScopes(raw: String?, default: List<ScopeType>): List<ScopeType> {
        if (raw == null) return default
        if (raw.isBlank()) return emptyList()
        return raw
            .split(',')
            .mapNotNull { token ->
                ScopeType.entries.firstOrNull { it.name == token }
            }.distinct()
            .take(3)
    }

    private companion object {
        const val LEGACY_SCHEMA_VERSION = 0
        const val CURRENT_SCHEMA_VERSION = 35
    }

    private object Keys {
        val schemaVersion = intPreferencesKey("settings_schema_version")
        val saveLocationId = stringPreferencesKey("save_location_id")
        val falseColorPresetId = stringPreferencesKey("false_color_preset_id")
        val peakingSensitivityId = stringPreferencesKey("peaking_sensitivity_id")
        val renderExposure = floatPreferencesKey("tone_render_exposure")
        val blacks = floatPreferencesKey("tone_black_toe")
        val shadows = floatPreferencesKey("tone_shadows")
        val contrast = floatPreferencesKey("tone_contrast")
        val midtones = floatPreferencesKey("tone_midtone_pivot")
        val highlights = floatPreferencesKey("tone_highlights")
        val whites = floatPreferencesKey("tone_shoulder_white_point")
        val saturation = floatPreferencesKey("tone_saturation")
        val vibrance = floatPreferencesKey("tone_vibrance")
        val colorRenderingStrength = floatPreferencesKey("tone_color_rendering_strength")
        val wbTemperature = floatPreferencesKey("tone_wb_temperature")
        val wbTint = floatPreferencesKey("tone_wb_tint")
        val outputColorSpaceId = stringPreferencesKey("output_color_space_id")
        val transferFunctionId = stringPreferencesKey("transfer_function_id")
        val jpegQuality = floatPreferencesKey("jpeg_quality")
        val jpegChromaSubsamplingId = stringPreferencesKey("jpeg_chroma_subsampling_id")
        val selfTimer = stringPreferencesKey("self_timer")
        val locationTagging = booleanPreferencesKey("location_tagging")
        val ois = booleanPreferencesKey("ois_enabled")
        val antiFlicker = stringPreferencesKey("anti_flicker")
        val exposureStep = stringPreferencesKey("exposure_step")
        val highlightProtection = stringPreferencesKey("highlight_protection")
        val maxPostGainId = stringPreferencesKey("max_post_gain_id")
        val autoMinFpsId = stringPreferencesKey("auto_min_fps_id")
        val controlSurfaceStyle = stringPreferencesKey("control_surface_style")
        val captureControlLayout = stringPreferencesKey("capture_control_layout")
        val jpegEnabled = booleanPreferencesKey("capture_jpeg_enabled")
        val dngEnabled = booleanPreferencesKey("capture_dng_enabled")
        val dngCompressionId = stringPreferencesKey("capture_dng_compression_id")
        val saveBaseDng = booleanPreferencesKey("capture_save_base_dng")
        val gridMode = stringPreferencesKey("capture_grid_mode")
        val overlayMode = stringPreferencesKey("capture_overlay_mode")
        val armedOverlays = stringPreferencesKey("capture_armed_overlays")
        val falseColorManual = booleanPreferencesKey("capture_false_color_manual")
        val activeScopes = stringPreferencesKey("capture_active_scopes")
        val waveformMode = stringPreferencesKey("capture_waveform_mode")
        val srgbToneRenderExposure = floatPreferencesKey("srgbTone_renderExposure")
        val srgbToneBlacks = floatPreferencesKey("srgbTone_blacks")
        val srgbToneShadows = floatPreferencesKey("srgbTone_shadows")
        val srgbToneContrast = floatPreferencesKey("srgbTone_contrast")
        val srgbToneMidtones = floatPreferencesKey("srgbTone_midtones")
        val srgbToneHighlights = floatPreferencesKey("srgbTone_highlights")
        val srgbToneWhites = floatPreferencesKey("srgbTone_whites")
        val srgbToneSaturation = floatPreferencesKey("srgbTone_saturation")
        val srgbToneVibrance = floatPreferencesKey("srgbTone_vibrance")
        val rec709ToneRenderExposure = floatPreferencesKey("rec709Tone_renderExposure")
        val rec709ToneBlacks = floatPreferencesKey("rec709Tone_blacks")
        val rec709ToneShadows = floatPreferencesKey("rec709Tone_shadows")
        val rec709ToneContrast = floatPreferencesKey("rec709Tone_contrast")
        val rec709ToneMidtones = floatPreferencesKey("rec709Tone_midtones")
        val rec709ToneHighlights = floatPreferencesKey("rec709Tone_highlights")
        val rec709ToneWhites = floatPreferencesKey("rec709Tone_whites")
        val rec709ToneSaturation = floatPreferencesKey("rec709Tone_saturation")
        val rec709ToneVibrance = floatPreferencesKey("rec709Tone_vibrance")
        val videoColorRenderProfile = stringPreferencesKey("video_color_render_profile")
        val videoUserLutProfileId = stringPreferencesKey("video_user_lut_profile_id")
        val videoLogEnabled = booleanPreferencesKey("video_log_enabled")
        val videoLogProfile = stringPreferencesKey("video_log_profile")
        val colorRenderProfile = stringPreferencesKey("color_render_profile")
        val selectedUserLutProfileId = stringPreferencesKey("selected_user_lut_profile_id")
        val userLutProfiles = stringPreferencesKey("user_lut_profiles")
        val lensProfiles = stringPreferencesKey("lens_profiles")
        val rawrToneRenderExposure = floatPreferencesKey("profile_tone_rawr_render_exposure")
        val rawrToneBlacks = floatPreferencesKey("profile_tone_rawr_blacks")
        val rawrToneShadows = floatPreferencesKey("profile_tone_rawr_shadows")
        val rawrToneContrast = floatPreferencesKey("profile_tone_rawr_contrast")
        val rawrToneMidtones = floatPreferencesKey("profile_tone_rawr_midtones")
        val rawrToneHighlights = floatPreferencesKey("profile_tone_rawr_highlights")
        val rawrToneWhites = floatPreferencesKey("profile_tone_rawr_whites")
        val rawrToneSaturation = floatPreferencesKey("profile_tone_rawr_saturation")
        val rawrToneVibrance = floatPreferencesKey("profile_tone_rawr_vibrance")
        val pipelineDiagnosticsEnabled = booleanPreferencesKey("debug_pipeline_diagnostics_enabled")
        val experimentalZeroCopyEnabled = booleanPreferencesKey("debug_experimental_zero_copy_enabled")
        val experimentalMultiframeEnabled = booleanPreferencesKey("experimental_multiframe_enabled")
        val persistentEngineEnabled = booleanPreferencesKey("persistent_engine_enabled")
        val filmSimEnabled = booleanPreferencesKey("film_sim_enabled")
        val filmPreviewDivisor = intPreferencesKey("film_preview_divisor")
        val filmSimLook = stringPreferencesKey("film_sim_look")
        val selectedFilmPresetId = stringPreferencesKey("selected_film_preset_id")
        val filmPresets = stringPreferencesKey("film_presets")
        val multiframeBaseFrameMode = stringPreferencesKey("multiframe_base_frame_mode")
        val multiframeChromaDenoise = booleanPreferencesKey("multiframe_chroma_denoise")
        val camera2NoiseModelEnabled = booleanPreferencesKey("camera2_noise_model_enabled")
        val multiframeNoiseProfile = stringPreferencesKey("multiframe_noise_profile")
        val multiframeOutputResolution = stringPreferencesKey("multiframe_output_resolution")
        val multiframeMergeAlgorithm = stringPreferencesKey("multiframe_merge_algorithm")
        val multiframeHdrPlusStrength = floatPreferencesKey("multiframe_hdrplus_strength")
        val multiframeMaxFrames = intPreferencesKey("multiframe_max_frames")
        val multiframeLkIterations = intPreferencesKey("multiframe_lk_iterations")
        val multiframeHessianExponent = intPreferencesKey("multiframe_hessian_exponent")
        val multiframeKDetail = floatPreferencesKey("multiframe_k_detail")
        val multiframeKDenoise = floatPreferencesKey("multiframe_k_denoise")
        val multiframeDThreshold = floatPreferencesKey("multiframe_d_threshold")
        val multiframeDTransition = floatPreferencesKey("multiframe_d_transition")
        val multiframeKStretch = floatPreferencesKey("multiframe_k_stretch")
        val multiframeKShrink = floatPreferencesKey("multiframe_k_shrink")
        val multiframeRobustnessT = floatPreferencesKey("multiframe_robustness_t")
        val multiframeRobustnessS1 = floatPreferencesKey("multiframe_robustness_s1")
        val multiframeRobustnessS2 = floatPreferencesKey("multiframe_robustness_s2")
        val multiframeMotionThreshold = floatPreferencesKey("multiframe_motion_threshold")
        val multiframeFlatSigma = floatPreferencesKey("multiframe_flat_sigma")
        val multiframeDetailFloor = floatPreferencesKey("multiframe_detail_floor")
        val multiframeScaleGain = floatPreferencesKey("multiframe_scale_gain")
        val multiframeCoverageNeffLo = floatPreferencesKey("multiframe_coverage_neff_lo")
        val multiframeCoverageNeffHi = floatPreferencesKey("multiframe_coverage_neff_hi")
        val multiframeCoverageMassLo = floatPreferencesKey("multiframe_coverage_mass_lo")
        val multiframeCoverageMassHi = floatPreferencesKey("multiframe_coverage_mass_hi")
        val multiframeFallbackChroma = floatPreferencesKey("multiframe_fallback_chroma")
        val multiframeFallbackLuma = floatPreferencesKey("multiframe_fallback_luma")
        val customGpuDriverEnabled = booleanPreferencesKey("debug_custom_gpu_driver_enabled")
        val customGpuDriverName = stringPreferencesKey("debug_custom_gpu_driver_name")
        val persistentDiagnosticsEnabled = booleanPreferencesKey("debug_persistent_diagnostics_enabled")
        val persistZslRingEnabled = booleanPreferencesKey("debug_persist_zsl_ring_enabled")
        val persistZslRingOnShutterEnabled = booleanPreferencesKey("debug_persist_zsl_ring_on_shutter_enabled")
        val internalTraceCaptureEnabled = booleanPreferencesKey("debug_internal_trace_capture_enabled")
        val internalTraceRetainedRows = intPreferencesKey("debug_internal_trace_retained_rows")
        val demosaicAlgorithm = stringPreferencesKey("capture_demosaic_algorithm")
        val dualAutoContrast = booleanPreferencesKey("dual_auto_contrast")
        val dualContrastPercent = floatPreferencesKey("dual_contrast_percent")
        val quadfixEnabled = booleanPreferencesKey("quadfix_enabled")
        val quadfixFastMedian = booleanPreferencesKey("quadfix_fast_median")
        val photoFccSteps = intPreferencesKey("photo_fcc_steps")
        val videoFccEnabled = booleanPreferencesKey("video_fcc_enabled")
        val videoFccSteps = intPreferencesKey("video_fcc_steps")
        val photoDefringeEnabled = booleanPreferencesKey("photo_defringe_enabled")
        val photoDefringeStrength = floatPreferencesKey("photo_defringe_strength")
        val photoDefringeEdgeThreshold = floatPreferencesKey("photo_defringe_edge_threshold")
        val photoDefringeLumaFloor = floatPreferencesKey("photo_defringe_luma_floor")
        val videoDefringeEnabled = booleanPreferencesKey("video_defringe_enabled")
        val videoDefringeStrength = floatPreferencesKey("video_defringe_strength")
        val videoDefringeEdgeThreshold = floatPreferencesKey("video_defringe_edge_threshold")
        val videoDefringeLumaFloor = floatPreferencesKey("video_defringe_luma_floor")
        // Pre-v30 shared keys. Read once as migration fallback, removed on save.
        val fccSteps = intPreferencesKey("fcc_steps")
        val defringeEnabled = booleanPreferencesKey("defringe_enabled")
        val defringeStrength = floatPreferencesKey("defringe_strength")
        val defringeEdgeThreshold = floatPreferencesKey("defringe_edge_threshold")
        val defringeLumaFloor = floatPreferencesKey("defringe_luma_floor")
        val denoiseEnabled = booleanPreferencesKey("denoise_enabled")
        val denoiseStrength = floatPreferencesKey("denoise_strength")
        val denoiseDetail = floatPreferencesKey("denoise_detail")
        val denoiseLuma = floatPreferencesKey("denoise_luma")
        val denoiseScales = intPreferencesKey("denoise_scales")
        val denoiseMethod = intPreferencesKey("denoise_method")
        val galoshRawMode = intPreferencesKey("galosh_raw_mode")
        val galoshStrength = floatPreferencesKey("galosh_strength")
        val galoshLuma = floatPreferencesKey("galosh_luma")
        val galoshChroma = floatPreferencesKey("galosh_chroma")
        val galoshYuvMode = intPreferencesKey("galosh_yuv_mode")
        val galoshYuvStrengthY = floatPreferencesKey("galosh_yuv_strength_y")
        val galoshYuvStrengthC = floatPreferencesKey("galosh_yuv_strength_c")
        val videoDenoiseEnabled = booleanPreferencesKey("video_denoise_enabled")
        val videoDenoiseStrength = floatPreferencesKey("video_denoise_strength")
        val videoDenoiseDetail = floatPreferencesKey("video_denoise_detail")
        val videoDenoiseLuma = floatPreferencesKey("video_denoise_luma")
        val videoDenoiseScales = intPreferencesKey("video_denoise_scales")
        val captureModeId = stringPreferencesKey("capture_mode_id")
        val videoResolutionId = stringPreferencesKey("video_resolution_id")
        val videoFps = intPreferencesKey("video_fps")
        val videoBitDepth = intPreferencesKey("video_bit_depth")
        val videoBitrate1080pMbps = intPreferencesKey("video_bitrate_1080p_mbps")
        val videoBitrate4kMbps = intPreferencesKey("video_bitrate_4k_mbps")
        val videoBitrateOpenGateMbps = intPreferencesKey("video_bitrate_open_gate_mbps")
        val videoBitrateMode = intPreferencesKey("video_bitrate_mode")
        val videoKeyframeSeconds = intPreferencesKey("video_keyframe_seconds")
        val videoMaxBFrames = intPreferencesKey("video_max_b_frames")
        val videoAudioChannels = intPreferencesKey("video_audio_channels")
        val videoAudioBitrateKbps = intPreferencesKey("video_audio_bitrate_kbps")
        val photoLensShading = booleanPreferencesKey("photo_lens_shading")
        val videoLensShading = booleanPreferencesKey("video_lens_shading")
        val distortionCorrection = booleanPreferencesKey("distortion_correction")
        val photoHighlightEnabled = booleanPreferencesKey("photo_highlight_enabled")
        val photoHighlightMethod = intPreferencesKey("photo_highlight_method")
        val photoHighlightThreshold = floatPreferencesKey("photo_highlight_threshold")
        val photoHighlightCompression = floatPreferencesKey("photo_highlight_compression")
        val videoHighlightEnabled = booleanPreferencesKey("video_highlight_enabled")
        val videoHighlightMethod = intPreferencesKey("video_highlight_method")
        val videoHighlightThreshold = floatPreferencesKey("video_highlight_threshold")
        val videoHighlightCompression = floatPreferencesKey("video_highlight_compression")
        // Pre-v30 shared keys. Read once as migration fallback, removed on save.
        val lensShadingCorrection = booleanPreferencesKey("lens_shading_correction")
        val highlightReconstruction = booleanPreferencesKey("capture_highlight_reconstruction_enabled")
        val highlightMethod = intPreferencesKey("capture_highlight_method")
        val highlightThreshold = floatPreferencesKey("capture_highlight_threshold")
        val highlightCompression = floatPreferencesKey("capture_highlight_compression")
        val ultraHdr = booleanPreferencesKey("capture_ultra_hdr_enabled")
    }
}

/** Process-wide global settings owner. Renderer recipes never submit edits here. */
object SettingsPreferencesStore {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    @Volatile private var instance: com.rawr.camera.settings.architecture.SettingsEditor? = null

    @Synchronized
    fun editor(context: Context, defaults: SettingsValues): com.rawr.camera.settings.architecture.SettingsEditor {
        instance?.let { return it }
        val repository = SettingsPreferencesRepository(context, defaults)
        val lutFiles = LutProfileFileStore(context.applicationContext)
        val gpuFiles = GpuDriverFileStore(context.applicationContext)
        var profiles: List<com.rawr.camera.settings.model.ImportedLutProfile>? = null
        var gpuEnabled: Boolean? = null
        return com.rawr.camera.settings.architecture.SettingsEditor(
            defaults, repository.values, repository::save, scope,
            onChanged = { values ->
                // Metadata must exist before backend consumers see a newly imported profile.
                if (profiles != values.userLutProfiles) {
                    lutFiles.sync(values.userLutProfiles, profiles.orEmpty())
                    profiles = values.userLutProfiles
                }
                if (gpuEnabled != values.customGpuDriverEnabled) {
                    gpuFiles.setEnabled(values.customGpuDriverEnabled)
                    gpuEnabled = values.customGpuDriverEnabled
                }
                com.rawr.camera.integration.LiveTonemapState.publish(values.activeImageTone())
            }
        ).also { instance = it }
    }
}
