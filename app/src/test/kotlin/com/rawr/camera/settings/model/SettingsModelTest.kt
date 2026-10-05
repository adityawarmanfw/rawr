package com.rawr.camera.settings.model

import com.rawr.camera.model.ControlSurfaceStyle
import com.rawr.camera.model.GridMode
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.architecture.PersistentSettingsController
import com.rawr.camera.settings.fixtures.SettingsFixtures
import com.rawr.camera.settings.preferences.FilmSimCodec
import com.rawr.camera.settings.model.FilmSimDetail
import com.rawr.camera.settings.model.FilmSimDiscreteField
import com.rawr.camera.settings.model.FilmSimSection
import com.rawr.camera.settings.model.SettingsDestination
import com.rawr.camera.settings.ui.numericStepIndex
import com.rawr.camera.video.toVideoImageSettings
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class SettingsModelTest {
    @Test fun settingsHierarchyContainsRedesignedSections() {
        assertTrue(
            SettingsSection.entries.map { it.title }.containsAll(
                listOf(
                    "Capture",
                    "Exposure",
                    "Lens",
                    "OIS",
                    "Image",
                    "Tone & Color",
                    "Demosaic",
                    "JPEG",
                    "Display & Control",
                    "Monitoring",
                    "Control Style",
                    "Storage",
                    "Debug",
                    "About"
                )
            )
        )
    }

    @Test fun unsupportedOisPreservesGlobalPreference() {
        val controller = PersistentSettingsController(SettingsFixtures.initialState(fullySupported = false))
        val before = controller.state.value.values
        controller.dispatch(SetOisPreference(false))
        assertEquals(before.oisEnabledPreference, controller.state.value.values.oisEnabledPreference)
    }

    @Test fun imageToneResetsAreScoped() {
        val controller =
            PersistentSettingsController(
                initialState = SettingsFixtures.initialState(),
                defaults = SettingsFixtures.initialState().values
            )
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.Shadows, 2f))
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.Saturation, 1.5f))
        controller.dispatch(ConfirmReset(ResetTarget.ExposureTonality))
        // TONE is per-profile: RAWR NTRL is active by default.
        assertEquals(0f, controller.state.value.values.rawrBaseTone.shadows)
        assertEquals(0f, controller.state.value.values.activeImageTone().shadows)
        assertEquals(1.5f, controller.state.value.values.rawrBaseTone.saturation)
    }

    @Test fun dngCompressionDefaultsToLossless() {
        val controller = PersistentSettingsController()
        assertEquals("dng.lossless", controller.state.value.values.dngCompressionId)
    }

    @Test fun dngCompressionChoiceUpdatesPreferenceAndPublishes() {
        var persisted: SettingsValues? = null
        val controller = PersistentSettingsController(onValuesChanged = { persisted = it })
        controller.dispatch(SetChoice(ChoiceSelectorKind.DngCompression, "dng.uncompressed"))
        assertEquals("dng.uncompressed", controller.state.value.values.dngCompressionId)
        assertEquals("dng.uncompressed", persisted?.dngCompressionId)
        controller.dispatch(SetChoice(ChoiceSelectorKind.DngCompression, "dng.lossless"))
        assertEquals("dng.lossless", controller.state.value.values.dngCompressionId)
    }

    @Test fun invalidDngCompressionChoiceIsRejectedWithoutMutatingPreference() {
        var writes = 0
        val controller = PersistentSettingsController(onValuesChanged = { writes++ })
        controller.dispatch(SetChoice(ChoiceSelectorKind.DngCompression, "dng.invalid"))
        assertEquals("dng.lossless", controller.state.value.values.dngCompressionId)
        assertEquals(0, writes)
    }

    @Test fun dngCompressionSelectorReturnsToDngSection() {
        val controller = PersistentSettingsController()
        controller.dispatch(OpenSection(SettingsSection.Dng))
        controller.dispatch(OpenSelector(ChoiceSelectorKind.DngCompression))
        controller.dispatch(SetChoice(ChoiceSelectorKind.DngCompression, "dng.uncompressed"))
        assertEquals(
            SettingsDestination.Section(SettingsSection.Dng),
            controller.state.value.presentation.destination
        )
    }

    @Test fun selectorUsesStableCandidateIdentity() {        val controller = PersistentSettingsController()
        controller.dispatch(SetChoice(ChoiceSelectorKind.MaxPostGain, "gain.200"))
        assertEquals("gain.200", controller.state.value.values.maxPostGainId)
        assertEquals("gain.200", controller.state.value.runtime.effective.maxPostGainId)
        controller.dispatch(SetChoice(ChoiceSelectorKind.AutoMinFps, "fps.12"))
        assertEquals("fps.12", controller.state.value.values.autoMinFpsId)
        assertEquals("fps.12", controller.state.value.runtime.effective.autoMinFpsId)
    }

    @Test fun capabilityReplacementPreservesUnsupportedPreferencesAndInvalidatesEffectiveValues() {
        val controller =
            PersistentSettingsController(
                initialState = SettingsFixtures.initialState(),
                defaults = SettingsFixtures.initialState().values
            )
        controller.dispatch(SetChoice(ChoiceSelectorKind.SaveLocation, "storage.pictures_raw"))
        controller.dispatch(SetChoice(ChoiceSelectorKind.FalseColorPreset, "monitor.falsecolor.skin"))
        controller.dispatch(SetChoice(ChoiceSelectorKind.PeakingSensitivity, "peaking.high"))
        controller.dispatch(SetChoice(ChoiceSelectorKind.MaxPostGain, "gain.200"))
        controller.dispatch(SetChoice(ChoiceSelectorKind.AutoMinFps, "fps.12"))
        val preferred = controller.state.value.values

        val reduced =
            SettingsFixtures.capabilities(fullySupported = false).copy(
                saveLocationChoices = listOf(ChoiceCandidate("storage.dcim_camera", "Internal storage / DCIM/Camera")),
                falseColorPresets =
                    listOf(
                        FalseColorPresetCandidate("monitor.falsecolor.standard", "Standard", "Fixture")
                    ),
                peakingSensitivityChoices = listOf(ChoiceCandidate("peaking.normal", "Normal")),
                outputColorSpaces = listOf(ChoiceCandidate("output.srgb", "sRGB")),
                transferFunctions = listOf(ChoiceCandidate("transfer.srgb", "sRGB")),
                maxPostGainChoices = listOf(ChoiceCandidate("gain.100", "1x")),
                autoMinFpsChoices = listOf(ChoiceCandidate("fps.30", "30 fps"))
            )
        controller.dispatch(ApplySettingsCapabilities(reduced))

        assertEquals(preferred, controller.state.value.values)
        with(controller.state.value.runtime.effective) {
            assertNull(saveLocationId)
            assertNull(falseColorPresetId)
            assertNull(peakingSensitivityId)
            assertEquals("output.srgb", outputColorSpaceId)
            assertEquals("transfer.srgb", transferFunctionId)
            assertNull(maxPostGainId)
            assertNull(autoMinFpsId)
        }
    }

    @Test fun outputResetRestoresOutputOnly() {
        val controller = PersistentSettingsController()
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.Shadows, 2f))
        controller.dispatch(SetChoice(ChoiceSelectorKind.JpegChromaSubsampling, "jpeg.444"))
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.JpegQuality, 95f))
        controller.dispatch(ConfirmReset(ResetTarget.Output))
        // Per-profile tone survives an output-only reset.
        assertEquals(2f, controller.state.value.values.rawrBaseTone.shadows)
        assertEquals(2f, controller.state.value.values.activeImageTone().shadows)
        assertEquals(
            SettingsFixtures.DEFAULT_OUTPUT_COLOR_SPACE_ID,
            controller.state.value.values.imageTone.outputColorSpaceId
        )
        assertEquals("transfer.srgb", controller.state.value.values.imageTone.transferFunctionId)
        assertEquals("jpeg.420", controller.state.value.values.imageTone.jpegChromaSubsamplingId)
        assertEquals(98f, controller.state.value.values.imageTone.jpegQuality)
    }

    @Test fun duplicateCapabilityIdsAreRejected() {
        val c = SettingsFixtures.capabilities()
        assertFailsWith<IllegalArgumentException> {
            c.copy(maxPostGainChoices = listOf(ChoiceCandidate("dup", "A"), ChoiceCandidate("dup", "B")))
        }
    }

    @Test fun invalidNumericApplicationValuesAreRejectedWithoutMutatingPreference() {
        val controller = PersistentSettingsController()
        val beforeTone = controller.state.value.values.imageTone
        val beforeRawr = controller.state.value.values.rawrBaseTone
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.Shadows, Float.NaN))
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.Shadows, 1000f))
        assertEquals(beforeTone, controller.state.value.values.imageTone)
        assertEquals(beforeRawr, controller.state.value.values.rawrBaseTone)
    }

    @Test fun backendApplicationStatusDoesNotRewritePreference() {
        val controller = PersistentSettingsController()
        val preferred = controller.state.value.values.oisEnabledPreference
        controller.dispatch(
            ReportOisApplication(
                controller.state.value.runtime.capabilityContextGeneration,
                SettingApplicationState(false, SettingApplicationStatus.Failed)
            )
        )
        assertEquals(preferred, controller.state.value.values.oisEnabledPreference)
        assertEquals(SettingApplicationStatus.Failed, controller.state.value.runtime.application.ois.status)
        assertEquals(false, controller.state.value.runtime.application.ois.effectiveValue)
    }

    @Test fun controlStyleIsGlobalPreferenceAndPublishesValuesChange() {
        var persisted: SettingsValues? = null
        val controller = PersistentSettingsController(onValuesChanged = { persisted = it })
        assertEquals(ControlSurfaceStyle.Basic, controller.state.value.values.controlSurfaceStyle)
        controller.dispatch(SetControlSurfaceStyle(ControlSurfaceStyle.Frosted))
        assertEquals(ControlSurfaceStyle.Frosted, controller.state.value.values.controlSurfaceStyle)
        assertEquals(ControlSurfaceStyle.Frosted, persisted?.controlSurfaceStyle)
    }

    @Test fun restoringPersistedValuesDoesNotWriteBackAndRejectsInvalidNumericData() {
        var writes = 0
        val controller = PersistentSettingsController(onValuesChanged = { writes++ })
        val persisted =
            controller.state.value.values.copy(
                locationTagging = true,
                imageTone =
                    controller.state.value.values.imageTone
                        .copy(shadows = Float.NaN)
            )
        controller.dispatch(RestorePersistentSettings(persisted))
        assertEquals(true, controller.state.value.values.locationTagging)
        assertEquals(ImageToneSpecs.shadows.defaultValue, controller.state.value.values.imageTone.shadows)
        assertEquals(0, writes)
    }
    @Test fun newFilmSectionsOpenAsSubpages() {
        val initial = SettingsFixtures.initialState()
        val dir = SettingsReducer.reduce(initial, OpenFilmSimSubPage(FilmSimSection.DirCouplers))
        assertEquals(SettingsDestination.FilmSimSubPage(FilmSimSection.DirCouplers), dir.presentation.destination)
        val diffusion = SettingsReducer.reduce(initial, OpenFilmSimSubPage(FilmSimSection.Diffusion))
        assertEquals(SettingsDestination.FilmSimSubPage(FilmSimSection.Diffusion), diffusion.presentation.destination)
        val output = SettingsReducer.reduce(initial, OpenFilmSimSubPage(FilmSimSection.Output))
        assertEquals(SettingsDestination.FilmSimSubPage(FilmSimSection.Output), output.presentation.destination)
    }

    @Test fun grainEmulsionDetailBackWalksTheTemporalStack() {
        val initial = SettingsFixtures.initialState()
        val grain = SettingsReducer.reduce(initial, OpenFilmSimSubPage(FilmSimSection.Grain))
        assertEquals(SettingsDestination.FilmSimSubPage(FilmSimSection.Grain), grain.presentation.destination)
        val emulsion = SettingsReducer.reduce(grain, OpenFilmSimDetail(FilmSimDetail.GrainEmulsion))
        assertEquals(
            SettingsDestination.FilmSimDetailPage(FilmSimDetail.GrainEmulsion),
            emulsion.presentation.destination
        )
        val texture = SettingsReducer.reduce(emulsion, OpenFilmSimDetail(FilmSimDetail.GrainTexture))
        assertEquals(
            SettingsDestination.FilmSimDetailPage(FilmSimDetail.GrainTexture),
            texture.presentation.destination
        )
        // Back returns to where you came from (emulsion), not the logical parent (Grain).
        val backToEmulsion = SettingsReducer.reduce(texture, NavigateBack)
        assertEquals(
            SettingsDestination.FilmSimDetailPage(FilmSimDetail.GrainEmulsion),
            backToEmulsion.presentation.destination
        )
        val backToGrain = SettingsReducer.reduce(backToEmulsion, NavigateBack)
        assertEquals(SettingsDestination.FilmSimSubPage(FilmSimSection.Grain), backToGrain.presentation.destination)
    }

    @Test fun backNavigationPopsExactlyOneSettingsLevel() {
        val initial = SettingsFixtures.initialState()
        val capture = SettingsReducer.reduce(initial, OpenSection(SettingsSection.Capture))
        val section = SettingsReducer.reduce(capture, OpenSection(SettingsSection.Exposure))
        val selector = SettingsReducer.reduce(section, OpenSelector(ChoiceSelectorKind.MaxPostGain))
        val backToSection = SettingsReducer.reduce(selector, NavigateBack)
        assertEquals(SettingsDestination.Section(SettingsSection.Exposure), backToSection.presentation.destination)
        val backToCapture = SettingsReducer.reduce(backToSection, NavigateBack)
        assertEquals(
            SettingsDestination.Section(SettingsSection.Capture),
            backToCapture.presentation.destination
        )
        val backToHome = SettingsReducer.reduce(backToCapture, NavigateBack)
        assertEquals(SettingsDestination.Home, backToHome.presentation.destination)
    }

    @Test fun graduatedSectionsFallBackToNewParents() {
        val initial = SettingsFixtures.initialState()
        val multiframe =
            initial.copy(
                presentation =
                    initial.presentation.copy(
                        destination = SettingsDestination.Section(SettingsSection.Multiframe),
                        backStack = emptyList()
                    )
            )
        assertEquals(
            SettingsDestination.Section(SettingsSection.Capture),
            SettingsReducer.reduce(multiframe, NavigateBack).presentation.destination
        )
        val filmSim =
            initial.copy(
                presentation =
                    initial.presentation.copy(
                        destination = SettingsDestination.Section(SettingsSection.FilmSim),
                        backStack = emptyList()
                    )
            )
        assertEquals(
            SettingsDestination.Section(SettingsSection.Image),
            SettingsReducer.reduce(filmSim, NavigateBack).presentation.destination
        )
        val exposure =
            initial.copy(
                presentation =
                    initial.presentation.copy(
                        destination = SettingsDestination.Section(SettingsSection.Exposure),
                        backStack = emptyList()
                    )
            )
        assertEquals(
            SettingsDestination.Section(SettingsSection.Capture),
            SettingsReducer.reduce(exposure, NavigateBack).presentation.destination
        )
    }

    @Test fun numericSliderDetentIndexTracksSemanticStep() {
        val spec = ImageToneSpecs.shadows
        assertEquals(0, numericStepIndex(spec, spec.minimum))
        assertEquals(100, numericStepIndex(spec, 0f))
        assertEquals(101, numericStepIndex(spec, 1f))
        assertEquals(200, numericStepIndex(spec, spec.maximum))
    }

    @Test fun capabilityReplacementResetsSupportedOisToPending() {
        val controller = PersistentSettingsController(SettingsFixtures.initialState(fullySupported = true))
        val generationA = controller.state.value.runtime.capabilityContextGeneration
        controller.dispatch(
            ReportOisApplication(generationA, SettingApplicationState(true, SettingApplicationStatus.Applied))
        )
        val preferred = controller.state.value.values

        controller.dispatch(ApplySettingsCapabilities(SettingsFixtures.capabilities(fullySupported = true)))

        assertEquals(preferred, controller.state.value.values)
        assertEquals(SettingApplicationStatus.Pending, controller.state.value.runtime.application.ois.status)
        assertNull(controller.state.value.runtime.application.ois.effectiveValue)
        assertTrue(controller.state.value.runtime.capabilityContextGeneration > generationA)
    }

    @Test fun capabilityReplacementTransitionsUnsupportedToSupportedPending() {
        val controller = PersistentSettingsController(SettingsFixtures.initialState(fullySupported = false))
        assertEquals(SettingApplicationStatus.Unsupported, controller.state.value.runtime.application.ois.status)

        controller.dispatch(ApplySettingsCapabilities(SettingsFixtures.capabilities(fullySupported = true)))

        assertEquals(SettingApplicationStatus.Pending, controller.state.value.runtime.application.ois.status)
    }

    @Test fun capabilityReplacementTransitionsSupportedToUnsupported() {
        val controller = PersistentSettingsController(SettingsFixtures.initialState(fullySupported = true))
        val preferred = controller.state.value.values

        controller.dispatch(ApplySettingsCapabilities(SettingsFixtures.capabilities(fullySupported = false)))

        assertEquals(preferred, controller.state.value.values)
        assertEquals(SettingApplicationStatus.Unsupported, controller.state.value.runtime.application.ois.status)
        assertNull(controller.state.value.runtime.application.ois.effectiveValue)
    }

    @Test fun staleBackendAcknowledgementFromPreviousCapabilityContextIsIgnored() {
        val controller = PersistentSettingsController(SettingsFixtures.initialState(fullySupported = true))
        val generationA = controller.state.value.runtime.capabilityContextGeneration
        controller.dispatch(ApplySettingsCapabilities(SettingsFixtures.capabilities(fullySupported = true)))
        val generationB = controller.state.value.runtime.capabilityContextGeneration
        assertTrue(generationB > generationA)
        assertEquals(SettingApplicationStatus.Pending, controller.state.value.runtime.application.ois.status)

        controller.dispatch(
            ReportOisApplication(generationA, SettingApplicationState(true, SettingApplicationStatus.Applied))
        )
        assertEquals(SettingApplicationStatus.Pending, controller.state.value.runtime.application.ois.status)

        controller.dispatch(
            ReportOisApplication(generationB, SettingApplicationState(true, SettingApplicationStatus.Applied))
        )
        assertEquals(SettingApplicationStatus.Applied, controller.state.value.runtime.application.ois.status)
        assertEquals(true, controller.state.value.runtime.application.ois.effectiveValue)
    }

    @Test fun gridModeIsOwnedByPersistentSettings() {
        val controller = PersistentSettingsController()
        controller.dispatch(SetGridMode(GridMode.Cross))
        assertEquals(GridMode.Cross, controller.state.value.values.gridMode)
    }

    @Test fun multiframeTunablesAreBoundedAndResettableToCatalogDefaults() {
        val controller = PersistentSettingsController()
        controller.dispatch(SetMultiframeNumericValue(MultiframeNumericParameter.KDetail, .08f))
        assertEquals(.08f, controller.state.value.values.multiframeTuning.kDetail)

        controller.dispatch(SetMultiframeNumericValue(MultiframeNumericParameter.KDetail, 1f))
        assertEquals(.08f, controller.state.value.values.multiframeTuning.kDetail)

        controller.dispatch(
            SetMultiframeNumericValue(
                MultiframeNumericParameter.KDetail,
                MultiframeSpecs.kDetail.defaultValue
            )
        )
        assertEquals(MultiframeTuning().kDetail, controller.state.value.values.multiframeTuning.kDetail)
    }

    @Test fun photoDefringeDefaultsToEnabledWhileVideoStaysOff() {
        var persisted: SettingsValues? = null
        val controller = PersistentSettingsController(onValuesChanged = { persisted = it })
        assertTrue(controller.state.value.values.photoDefringeEnabled)
        assertEquals(1f, controller.state.value.values.photoDefringeStrength)
        assertEquals(0.02f, controller.state.value.values.photoDefringeEdgeThreshold)
        assertEquals(0.08f, controller.state.value.values.photoDefringeLumaFloor)
        assertFalse(controller.state.value.values.videoDefringeEnabled)

        controller.dispatch(SetPhotoDefringeEnabled(false))
        assertFalse(controller.state.value.values.photoDefringeEnabled)
        assertEquals(false, persisted?.photoDefringeEnabled)
        // Photo toggle must not touch the video side.
        assertFalse(controller.state.value.values.videoDefringeEnabled)
    }

    @Test fun photoAndVideoFccStepsAreIndependent() {
        val controller = PersistentSettingsController()
        controller.dispatch(SetPhotoFccSteps(4))
        controller.dispatch(SetVideoFccSteps(6))

        val values = controller.state.value.values
        assertEquals(4, values.photoFccSteps)
        assertEquals(6, values.videoFccSteps)
        // Video FCC defaults off: recordings skip FCC regardless of steps.
        assertEquals(0, values.toVideoImageSettings().fccSteps)

        controller.dispatch(SetVideoFccEnabled(true))
        assertEquals(6, controller.state.value.values.toVideoImageSettings().fccSteps)
        assertEquals(4, controller.state.value.values.photoFccSteps)
    }

    @Test fun photoAndVideoDefringeTunablesAreBoundedAndIndependent() {
        val controller = PersistentSettingsController()
        controller.dispatch(SetPhotoDefringeStrength(2f))
        assertEquals(1f, controller.state.value.values.photoDefringeStrength)
        controller.dispatch(SetPhotoDefringeStrength(-1f))
        assertEquals(0f, controller.state.value.values.photoDefringeStrength)

        controller.dispatch(SetPhotoDefringeEdgeThreshold(1f))
        assertEquals(0.2f, controller.state.value.values.photoDefringeEdgeThreshold)
        controller.dispatch(SetPhotoDefringeEdgeThreshold(0f))
        assertEquals(0.005f, controller.state.value.values.photoDefringeEdgeThreshold)

        controller.dispatch(SetPhotoDefringeLumaFloor(1f))
        assertEquals(0.5f, controller.state.value.values.photoDefringeLumaFloor)
        controller.dispatch(SetPhotoDefringeLumaFloor(-1f))
        assertEquals(0f, controller.state.value.values.photoDefringeLumaFloor)

        controller.dispatch(SetVideoDefringeStrength(2f))
        assertEquals(1f, controller.state.value.values.videoDefringeStrength)
        // Photo strength untouched by the video edit.
        assertEquals(0f, controller.state.value.values.photoDefringeStrength)
    }

    @Test fun videoImageSettingsFollowVideoFieldsOnly() {
        val controller = PersistentSettingsController()
        // Photo edits must not leak into the video pipeline.
        controller.dispatch(SetPhotoHighlightThreshold(2f))
        controller.dispatch(SetPhotoDenoiseEnabled(true))
        controller.dispatch(SetPhotoDenoiseStrength(7f))
        controller.dispatch(SetPhotoDenoiseLuma(0.9f))
        controller.dispatch(SetPhotoDenoiseScales(3))
        var video = controller.state.value.values.toVideoImageSettings()
        assertEquals(1f, video.highlightThreshold)
        assertEquals(0f, video.waveletDenoiseStrength)
        assertEquals(0.25f, video.waveletDenoiseLuma)
        assertEquals(7, video.waveletDenoiseScales)

        controller.dispatch(SetVideoHighlightEnabled(true))
        controller.dispatch(SetVideoHighlightThreshold(1.5f))
        controller.dispatch(SetVideoDenoiseEnabled(true))
        controller.dispatch(SetVideoDenoiseStrength(3f))
        controller.dispatch(SetVideoDenoiseLuma(0.6f))
        controller.dispatch(SetVideoDenoiseScales(5))
        video = controller.state.value.values.toVideoImageSettings()
        assertTrue(video.highlightEnabled)
        assertEquals(1.5f, video.highlightThreshold)
        assertEquals(3f, video.waveletDenoiseStrength)
        assertEquals(0.6f, video.waveletDenoiseLuma)
        assertEquals(5, video.waveletDenoiseScales)
    }

    @Test fun waveletLumaAndScalesClampAndReachNativeSpec() {
        val controller = PersistentSettingsController()
        controller.dispatch(SetPhotoDenoiseEnabled(true))
        controller.dispatch(SetPhotoDenoiseLuma(2f))
        controller.dispatch(SetPhotoDenoiseScales(42))
        var resolved = (controller.state.value.values.photoDenoise as DenoiseConfig.Wavelet)
            .resolve(StillPath.SINGLE)
        assertEquals(1f, resolved.waveletLuma)
        assertEquals(7, resolved.waveletScales)
        assertEquals(5, resolved.toModesArray().size)
        assertEquals(8, resolved.toStrengthsArray().size)
        assertEquals(1f, resolved.toStrengthsArray()[7])
        assertEquals(7, resolved.toModesArray()[4])
        controller.dispatch(SetPhotoDenoiseLuma(0.5f))
        controller.dispatch(SetPhotoDenoiseScales(5))
        resolved = (controller.state.value.values.photoDenoise as DenoiseConfig.Wavelet)
            .resolve(StillPath.SINGLE)
        assertEquals(0.5f, resolved.toStrengthsArray()[7])
        assertEquals(5, resolved.toModesArray()[4])
    }

    @Test fun multiframeBaseFrameModeDefaultsToMiddleAndIsPersistable() {
        var persisted: SettingsValues? = null
        val controller = PersistentSettingsController(onValuesChanged = { persisted = it })
        assertEquals(MultiframeBaseFrameMode.Middle, controller.state.value.values.multiframeBaseFrameMode)

        controller.dispatch(SetMultiframeBaseFrameMode(MultiframeBaseFrameMode.Sharpest))

        assertEquals(MultiframeBaseFrameMode.Sharpest, controller.state.value.values.multiframeBaseFrameMode)
        assertEquals(MultiframeBaseFrameMode.Sharpest, persisted?.multiframeBaseFrameMode)
        assertEquals(0, MultiframeBaseFrameMode.Middle.nativeId)
        assertEquals(1, MultiframeBaseFrameMode.Sharpest.nativeId)
    }

    @Test fun multiframeResolutionUsesMpPresetsAndNativeContract() {
        val tuning = MultiframeTuning(outputResolution = MultiframeOutputResolution.Mp20)
        val native = tuning.nativeValues()
        assertEquals(22, native.size)
        assertEquals(MultiframeOutputResolution.Mp20.outputScale, native[0])
        assertEquals(5f, native[1])
        assertEquals(1e-10f, native[2])
        assertTrue(MultiframeOutputResolution.entries.all { it.label.contains("MP") })
    }

    @Test fun multiframeMergeAlgorithmIsPersistableAndAppendedToNativeContract() {
        var persisted: SettingsValues? = null
        val controller = PersistentSettingsController(onValuesChanged = { persisted = it })
        assertEquals(MultiframeMergeAlgorithm.Wronski, controller.state.value.values.multiframeTuning.mergeAlgorithm)

        controller.dispatch(SetMultiframeMergeAlgorithm(MultiframeMergeAlgorithm.HdrPlus))
        controller.dispatch(SetMultiframeNumericValue(MultiframeNumericParameter.HdrPlusStrength, 18f))
        controller.dispatch(SetMultiframeNumericValue(MultiframeNumericParameter.HdrPlusStrength, 40f))

        val tuning = controller.state.value.values.multiframeTuning
        assertEquals(MultiframeMergeAlgorithm.HdrPlus, persisted?.multiframeTuning?.mergeAlgorithm)
        assertEquals(18f, tuning.hdrPlusStrength)
        // Native layout: 22 tuning values, chroma flag, then algorithm id + strength.
        assertEquals(22, tuning.nativeValues().size)
        assertEquals(listOf(1f, 18f, 32f), tuning.nativeMergeValues().toList())
        controller.dispatch(SetMultiframeNumericValue(MultiframeNumericParameter.HdrPlusTileSize, 16f))
        controller.dispatch(SetMultiframeNumericValue(MultiframeNumericParameter.HdrPlusTileSize, 64f))
        assertEquals(16, controller.state.value.values.multiframeTuning.hdrPlusTileSize)
        assertEquals(0, MultiframeMergeAlgorithm.Wronski.nativeId)
        assertEquals(2, MultiframeMergeAlgorithm.HdrPlusQuality.nativeId)
        controller.dispatch(SetMultiframeMergeAlgorithm(MultiframeMergeAlgorithm.HdrPlusQuality))
        assertEquals(2f, controller.state.value.values.multiframeTuning.nativeMergeValues()[0])
        assertEquals(13f, MultiframeTuning().copy(hdrPlusStrength = 0f).sanitized().hdrPlusStrength)
    }

    @Test fun multiframeDefaultsFollowResearchedOperatingPoint() {
        val defaults = MultiframeTuning()
        assertEquals(.15f, defaults.kDetail)
        assertEquals(2f, defaults.kStretch)
        assertEquals(1f, defaults.flatSigma)
        assertEquals(.15f, defaults.detailFloorSigma)
        assertEquals(1f, defaults.scaleBandwidthGain)
        assertEquals(16, defaults.maxFrames)
        assertEquals(4f, defaults.fallbackChroma)
        assertEquals(2f, defaults.fallbackLuma)
        val params = MultiframeSpecs.all.map { it.parameter }
        assertTrue(params.contains(MultiframeNumericParameter.FlatSigma))
    }

    @Test fun multiframeTuningBankIsSingle() {
        val controller = PersistentSettingsController()
        assertEquals(.15f, controller.state.value.values.multiframeTuning.kDetail)

        controller.dispatch(SetMultiframeNumericValue(MultiframeNumericParameter.KDetail, .31f))

        assertEquals(.31f, controller.state.value.values.multiframeTuning.kDetail)
        val native = controller.state.value.values.multiframeTuning.nativeValues()
        assertEquals(22, native.size)
    }

    @Test fun everyMultiframeTunableExplainsLowerAndHigherImageEffects() {
        MultiframeSpecs.all.forEach { spec ->
            assertTrue(spec.supportingText.contains("Lower:"))
            assertTrue(spec.supportingText.contains("Higher:"))
        }
    }

    @Test fun invalidPersistedMultiframeTuningFallsBackFieldByField() {
        val restored =
            MultiframeTuning(kDetail = Float.NaN, maxFrames = 99, robustnessT = .2f)
                .sanitized()
        assertEquals(MultiframeTuning().kDetail, restored.kDetail)
        assertEquals(MultiframeTuning().maxFrames, restored.maxFrames)
        assertEquals(.2f, restored.robustnessT)
    }

    @Test fun multiframeDetailSanitizationUsesSingleRange() {
        // Below the slider minimum resets to the default operating point.
        assertEquals(.15f, MultiframeTuning(kDetail = .07f).sanitized().kDetail)
        assertEquals(
            .31f,
            MultiframeTuning(kDetail = .31f).sanitized().kDetail
        )
    }

    @Test fun multiframeDefaultsMatchOfflineTunedIAndFloorKDetailAtSafeMinimum() {
        val v2 = MultiframeTuning()
        assertEquals(16, v2.maxFrames)
        assertEquals(.15f, v2.kDetail)
        assertEquals(5f, v2.kDenoise)
        assertEquals(.71f, v2.dThreshold)
        assertEquals(1f, v2.dTransition)
        assertEquals(2f, v2.kStretch)
        assertEquals(8f, v2.kShrink)
        // Operating point re-tuned 2026-10-01 against burst-fitted noise and
        // full-coverage LK (offline replay: flat grain, edge 10-90 width vs
        // base frame, motion bursts); kernels narrowed 2026-10-02 for the
        // merged-CFA demosaic (texture MTF vs base frame). Below the kDetail
        // min it sanitizes to the default.
        val floored =
            v2.copy(kDetail = .07f).sanitized(v2)
        assertEquals(.15f, floored.kDetail)
        val atFloor =
            v2.copy(kDetail = .08f).sanitized(v2)
        assertEquals(.08f, atFloor.kDetail)

        // Extended kDenoise range admits strong denoise.
        val denoised =
            v2.copy(kDenoise = 7f).sanitized(v2)
        assertEquals(7f, denoised.kDenoise)

        // Specs agree with the native JNI guard: researched values accepted.
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.KDetail).accepts(.08f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.KDenoise).accepts(7f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.DThreshold).accepts(.25f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.DThreshold).accepts(.20f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.DTransition).accepts(.3f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.KShrink).accepts(8f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.KShrink).accepts(10f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.FlatSigma).accepts(.5f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.DetailFloorSigma).accepts(0f))
        assertEquals(16f, MultiframeSpecs.forParameter(MultiframeNumericParameter.MaxFrames).defaultValue)
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.DThreshold).accepts(.71f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.FallbackChroma).accepts(4f))
        assertTrue(MultiframeSpecs.forParameter(MultiframeNumericParameter.FallbackLuma).accepts(0f))
    }

    @Test fun removingLastStageFromSelectedProfileFallsBackToRawrBase() {
        val controller = PersistentSettingsController()
        val stage =
            ImportedLutStage(
                id = "stage-1",
                fileName = "look.cube",
                relativePath = "lut_profiles/files/stage-1.cube"
            )
        controller.dispatch(ImportLutStage(targetProfileId = null, stage = stage))
        val profileId = controller.state.value.values.selectedUserLutProfileId
        assertEquals(stage.id, profileId)
        assertEquals(ColorRenderProfile.UserLut, controller.state.value.values.colorRenderProfile)

        controller.dispatch(RemoveUserLutStage(profileId!!, stage.id))
        assertEquals(ColorRenderProfile.RawrBase, controller.state.value.values.colorRenderProfile)
        assertNull(controller.state.value.values.selectedUserLutProfileId)
    }

    @Test fun toneEditsApplyToActiveProfileOnly() {
        val controller = PersistentSettingsController()
        // RAWR NTRL active: edits land on rawrBaseTone.
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.Shadows, 10f))
        assertEquals(10f, controller.state.value.values.rawrBaseTone.shadows)
        assertEquals(10f, controller.state.value.values.activeImageTone().shadows)

        // Import a LUT profile (selected + neutral tone), edit while active.
        val stage =
            ImportedLutStage(
                id = "stage-1",
                fileName = "look.cube",
                relativePath = "lut_profiles/files/stage-1.cube"
            )
        controller.dispatch(ImportLutStage(targetProfileId = null, stage = stage))
        val profileId = controller.state.value.values.selectedUserLutProfileId!!
        assertEquals(0f, controller.state.value.values.activeImageTone().shadows)
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.Shadows, -20f))
        val lutTone = controller.state.value.values.userLutProfiles.single { it.id == profileId }.tone
        assertEquals(-20f, lutTone.shadows)
        assertEquals(-20f, controller.state.value.values.activeImageTone().shadows)
        // RAWR NTRL tone is untouched.
        assertEquals(10f, controller.state.value.values.rawrBaseTone.shadows)

        // Switching back to RAWR NTRL restores its tone.
        controller.dispatch(SetColorRenderProfile(ColorRenderProfile.RawrBase))
        assertEquals(10f, controller.state.value.values.activeImageTone().shadows)
    }

    @Test fun globalToneParamsStayGlobalAcrossProfiles() {        val controller = PersistentSettingsController()
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.JpegQuality, 95f))
        assertEquals(95f, controller.state.value.values.imageTone.jpegQuality)
        val stage =
            ImportedLutStage(
                id = "stage-1",
                fileName = "look.cube",
                relativePath = "lut_profiles/files/stage-1.cube"
            )
        controller.dispatch(ImportLutStage(targetProfileId = null, stage = stage))
        assertEquals(95f, controller.state.value.values.imageTone.jpegQuality)
        assertEquals(95f, controller.state.value.values.activeImageTone().jpegQuality)
    }

    @Test fun renderProfileSyncKeyIgnoresToneButTracksStructure() {
        val controller = PersistentSettingsController()
        val before = controller.state.value.values.renderProfileSyncKey()
        // Tone-only edits must not retrigger the expensive native profile switch.
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.Shadows, 15f))
        assertEquals(before, controller.state.value.values.renderProfileSyncKey())

        val stage =
            ImportedLutStage(
                id = "stage-1",
                fileName = "look.cube",
                relativePath = "lut_profiles/files/stage-1.cube"
            )
        controller.dispatch(ImportLutStage(targetProfileId = null, stage = stage))
        val lutKey = controller.state.value.values.renderProfileSyncKey()
        controller.dispatch(SetNumericValue(ImageToneNumericParameter.Shadows, -15f))
        assertEquals(lutKey, controller.state.value.values.renderProfileSyncKey())

        // Structure edits still retrigger the native rebuild.
        controller.dispatch(SetUserLutAfterAction(stage.id, AfterLutAction.ConvertToJpegSrgb))
        assertTrue(controller.state.value.values.renderProfileSyncKey() != lutKey)
    }

    @Test fun filmDescriptionUsesDashPerLineExifStyle() {
        val look = FilmSimLook(film = 14, paper = 5, dirCouplersAmount = 0.88f)
        val lines = look.describeFilm().lines()
        assertEquals("Film simulation:", lines.first())
        assertTrue(lines.any { it == "- Stock: C200" })
        assertTrue(lines.any { it == "- Paper: Crystal Archive II" })
        assertTrue(lines.any { it == "- DIR couplers: 0.88" })
        assertTrue(lines.none { it.contains("·") })
        val minimal = FilmSimLook().describeFilm()
        assertTrue(minimal.lines().any { it.startsWith("- Grain: ") })
    }

    @Test fun dirAmountUsesZeroToOneAcrossStocksAndOlderLooks() {
        val spec = FilmSimSpecs.forParameter(FilmSimNumericParameter.DirCouplersAmount)
        assertEquals(0f, spec.minimum)
        assertEquals(1f, spec.maximum)
        for (film in listOf(2, 16, 18)) {
            val old = FilmSimLook(film = film, dirCouplersAmount = 1.7f)
            assertEquals(1f, old.toFloatArray()[44])
            assertEquals(1f, old.withNumeric(
                SetFilmSimNumericValue(FilmSimNumericParameter.DirCouplersAmount, 2f)
            ).dirCouplersAmount)
            val historicalFloats = FilmSimLook(film = film).toFloatArray()
            historicalFloats[44] = 1.7f
            val historical = historicalFloats.joinToString(",") + "|" +
                FilmSimLook(film = film).toIntArray().joinToString(",")
            assertEquals(1f, FilmSimCodec.decodeLook(historical)?.dirCouplersAmount)
        }
    }

    @Test fun positiveStocksSelectDirectScanAndLeaveManualProcessOverride() {
        val controller = PersistentSettingsController()
        for (film in FilmStocks.positiveFilmIndices) {
            controller.dispatch(SetFilmSimDiscreteValue(FilmSimDiscreteField.Film, film))
            val look = controller.state.value.values.filmSimLook
            assertEquals(1, look.process)
            assertFalse(look.scanNegativeInvert)
        }
        controller.dispatch(SetFilmSimDiscreteValue(FilmSimDiscreteField.Process, 0))
        assertEquals(0, controller.state.value.values.filmSimLook.process)
    }
}
