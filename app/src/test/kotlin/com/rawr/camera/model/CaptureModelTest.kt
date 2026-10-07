package com.rawr.camera.model

import com.rawr.camera.architecture.ActivateSpotAe
import com.rawr.camera.architecture.ApplyCameraCapabilities
import com.rawr.camera.architecture.CancelSelfTimer
import com.rawr.camera.architecture.CaptureReducer
import com.rawr.camera.architecture.CaptureTransitions
import com.rawr.camera.architecture.ClearAutofocus
import com.rawr.camera.architecture.CycleSelfTimer
import com.rawr.camera.architecture.FinishSpotAeMove
import com.rawr.camera.architecture.FocusAt
import com.rawr.camera.architecture.MoveSpotAe
import com.rawr.camera.architecture.OpenFocusSelector
import com.rawr.camera.architecture.RestoreCapturePreferences
import com.rawr.camera.architecture.SelectLens
import com.rawr.camera.architecture.SetCaptureLayout
import com.rawr.camera.architecture.SetCaptureMode
import com.rawr.camera.architecture.SetCaptureSelfTimer
import com.rawr.camera.architecture.StartSelfTimerCountdown
import com.rawr.camera.architecture.TickSelfTimer
import com.rawr.camera.architecture.ToggleFilmSim
import com.rawr.camera.architecture.ToggleMultiframe
import com.rawr.camera.architecture.ToggleFalseColor
import com.rawr.camera.architecture.ToggleOverlayArmed
import com.rawr.camera.architecture.SetControlSurfaceStyle
import com.rawr.camera.architecture.SetExposureCandidate
import com.rawr.camera.architecture.SetFocusMode
import com.rawr.camera.architecture.SetManualFocus
import com.rawr.camera.architecture.ToggleScope
import com.rawr.camera.architecture.ToggleScopeExpanded
import com.rawr.camera.architecture.TriggerCapture
import com.rawr.camera.fixtures.CaptureFixtures
import com.rawr.camera.fixtures.CaptureSimulationTimings
import com.rawr.camera.fixtures.InMemoryCaptureScreenController
import com.rawr.camera.model.CaptureControlLayout
import com.rawr.camera.model.WhiteBalanceMode
import com.rawr.camera.settings.model.SelfTimer
import com.rawr.camera.ui.classifyPhysicalOrientation
import com.rawr.camera.ui.physicalAngleToDisplayRotation
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest

@OptIn(ExperimentalCoroutinesApi::class)
class CaptureModelTest {
    private val capabilities = CaptureFixtures.baseline()

    @Test
    fun exposureOwnersMatchLockedModes() {
        assertEquals(listOf(ExposureParameter.Ev), state(ExposureMode.Auto).exposureOwners())
        assertEquals(listOf(ExposureParameter.Shutter, ExposureParameter.Iso), state(ExposureMode.Manual).exposureOwners())
        assertEquals(listOf(ExposureParameter.Shutter, ExposureParameter.Ev), state(ExposureMode.ShutterPriority).exposureOwners())
        assertEquals(listOf(ExposureParameter.Iso, ExposureParameter.Ev), state(ExposureMode.IsoPriority).exposureOwners())
    }

    @Test
    fun anchorLabelsRemainAroundCenteredCurrent() {
        assertEquals(
            ExposureLabels("100", "200", "400"),
            capabilities.iso.labelsAround(capabilities.iso.candidates[6].id)
        )
        assertEquals(
            ExposureLabels("200", "280", "400"),
            capabilities.iso.labelsAround(capabilities.iso.candidates[7].id)
        )
    }

    @Test
    fun evCompensationFixtureUsesOneTenthEvSteps() {
        val labels = capabilities.ev.candidates.map(ExposureCandidate::displayLabel)
        assertEquals(61, labels.size)
        assertEquals("-3.0", labels.first())
        assertEquals("+3.0", labels.last())
        assertEquals("+0.0", capabilities.ev.initialCandidate.displayLabel)

        val zeroIndex = capabilities.ev.indexOf(capabilities.ev.initialCandidateId)
        assertEquals(listOf("-0.2", "-0.1", "+0.0", "+0.1", "+0.2"), labels.subList(zeroIndex - 2, zeroIndex + 3))
    }

    @Test
    fun evFineStepsKeepWholeStopAnchorLabels() {
        val plusPointThree = capabilities.ev.candidates.first { it.displayLabel == "+0.3" }
        assertEquals(ExposureLabels("+0.0", "+0.3", "+1.0"), capabilities.ev.labelsAround(plusPointThree.id))

        val minusPointThree = capabilities.ev.candidates.first { it.displayLabel == "-0.3" }
        assertEquals(ExposureLabels("-1.0", "-0.3", "+0.0"), capabilities.ev.labelsAround(minusPointThree.id))
    }

    @Test
    fun deterministicScopeSlotsMatchSpec() {
        assertEquals(listOf(ScopeSlot(.6611f, .025f)), scopeSlots(Orientation.Portrait, 1))
        assertEquals(
            listOf(ScopeSlot(.0278f, .025f), ScopeSlot(.6611f, .025f)),
            scopeSlots(Orientation.Portrait, 2)
        )
        assertEquals(3, scopeSlots(Orientation.Portrait, 3).size)
        assertEquals(listOf(ScopeSlot(.689f, .067f)), scopeSlots(Orientation.Landscape, 1))
        assertEquals(3, scopeSlots(Orientation.Landscape, 3).size)
    }

    @Test
    fun applicationTransitionRejectsUnknownLensId() {
        val before = CaptureUiState(capabilities)
        val after = CaptureTransitions.selectLens(before, "missing")
        assertEquals(before.selectedLensId, after.selectedLensId)
    }

    @Test
    fun applicationTransitionClampsManualFocusToCapabilityRangeOnlyInMf() {
        val before = CaptureUiState(capabilities).copy(focus = FocusUiState(mode = FocusMode.Mf))
        val after = CaptureTransitions.setManualFocus(before, 5f)
        assertEquals(capabilities.manualFocus.maxNormalized, after.focus.mfNormalized)
    }

    @Test
    fun applicationTransitionIgnoresManualFocusOutsideMf() {
        val before = CaptureUiState(capabilities)
        val after = CaptureTransitions.setManualFocus(before, .25f)
        assertEquals(before.focus.mfNormalized, after.focus.mfNormalized)
    }

    @Test
    fun overlayArmingIsIndependentPerLayer() = withController { controller ->
        controller.dispatch(ToggleOverlayArmed(OverlayMode.Peaking))
        controller.dispatch(ToggleOverlayArmed(OverlayMode.TonemapShadows))
        assertEquals(
            setOf(OverlayMode.Peaking, OverlayMode.TonemapShadows),
            controller.state.value.armedOverlays
        )
        controller.dispatch(ToggleOverlayArmed(OverlayMode.Peaking))
        assertEquals(setOf(OverlayMode.TonemapShadows), controller.state.value.armedOverlays)
        controller.dispatch(ToggleFalseColor)
        assertTrue(controller.state.value.falseColorManual)
    }

    @Test
    fun scopeActivationOrderDefinesSlotsAndCollapsePreservesOrder() = withController { controller ->
        controller.dispatch(ToggleScope(ScopeType.Waveform))
        controller.dispatch(ToggleScope(ScopeType.Vectorscope))
        assertEquals(listOf(ScopeType.Waveform, ScopeType.Vectorscope), controller.state.value.activeScopes)
        controller.dispatch(ToggleScopeExpanded(ScopeType.Waveform))
        controller.dispatch(ToggleScopeExpanded(ScopeType.Waveform))
        assertEquals(listOf(ScopeType.Waveform, ScopeType.Vectorscope), controller.state.value.activeScopes)
        assertNull(controller.state.value.expandedScope)
    }

    @Test
    fun normalFocusDoesNotOpenSelector() = withController { controller ->
        controller.dispatch(FocusAt(NormalizedPoint(.2f, .3f)))
        assertFalse(controller.state.value.focus.selectorOpen)
        assertEquals(TargetStatus.Settling, controller.state.value.focus.status)
    }

    @Test
    fun clearAutofocusHidesBoxButKeepsTarget() {
        val tapped =
            CaptureTransitions.beginAutofocus(
                CaptureUiState(capabilities),
                NormalizedPoint(.2f, .3f)
            )
        assertEquals(TargetStatus.Settling, tapped.focus.status)
        val cleared = CaptureTransitions.clearAutofocus(tapped)
        assertEquals(TargetStatus.Hidden, cleared.focus.status)
        assertEquals(tapped.focus.target, cleared.focus.target)
        assertEquals(cleared.focus, CaptureTransitions.clearAutofocus(cleared).focus)
    }

    @Test
    fun enteringAfModeReturnsToFullAutoHidden() {
        val locked =
            CaptureUiState(capabilities).copy(
                focus = FocusUiState(mode = FocusMode.AfLock, status = TargetStatus.Settled)
            )
        val auto = CaptureTransitions.selectFocusMode(locked, FocusMode.Af)
        assertEquals(FocusMode.Af, auto.focus.mode)
        assertEquals(TargetStatus.Hidden, auto.focus.status)
    }

    @Test
    fun retapClearsImmediately() = withController { controller ->
        controller.dispatch(FocusAt(NormalizedPoint(.2f, .3f)))
        assertEquals(TargetStatus.Settling, controller.state.value.focus.status)
        controller.dispatch(ClearAutofocus)
        assertEquals(TargetStatus.Hidden, controller.state.value.focus.status)
    }

    @Test
    fun tapBoxAutoDismissesToFullAuto() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val controller =
            InMemoryCaptureScreenController(
                capabilities = capabilities,
                timings = CaptureSimulationTimings(settleDelayMs = 25L),
                dispatcher = dispatcher
            )
        try {
            controller.dispatch(FocusAt(NormalizedPoint(.2f, .3f)))
            advanceTimeBy(25L)
            runCurrent()
            assertEquals(TargetStatus.Settled, controller.state.value.focus.status)
            advanceTimeBy(4000L)
            runCurrent()
            assertEquals(TargetStatus.Hidden, controller.state.value.focus.status)
            assertEquals(FocusMode.Af, controller.state.value.focus.mode)
        } finally {
            controller.close()
        }
    }

    @Test
    fun afLockStaysLatchedPastAutoDismissWindow() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val controller =
            InMemoryCaptureScreenController(
                capabilities = capabilities,
                timings = CaptureSimulationTimings(settleDelayMs = 25L),
                dispatcher = dispatcher
            )
        try {
            controller.dispatch(SetFocusMode(FocusMode.AfLock))
            advanceTimeBy(25L)
            runCurrent()
            assertEquals(TargetStatus.Settled, controller.state.value.focus.status)
            advanceTimeBy(5000L)
            runCurrent()
            assertEquals(TargetStatus.Settled, controller.state.value.focus.status)
            assertEquals(FocusMode.AfLock, controller.state.value.focus.mode)
        } finally {
            controller.close()
        }
    }

    @Test
    fun longPressActionOpensSelectorWithoutMovingFocus() = withController { controller ->
        val before = controller.state.value.focus.target
        controller.dispatch(OpenFocusSelector)
        assertTrue(controller.state.value.focus.selectorOpen)
        assertEquals(before, controller.state.value.focus.target)
    }

    @Test
    fun spotMoveDoesNotMoveAf() = withController { controller ->
        val af = controller.state.value.focus.target
        controller.dispatch(ActivateSpotAe(NormalizedPoint(.7f, .4f)))
        controller.dispatch(MoveSpotAe(NormalizedPoint(.2f, .8f)))
        assertEquals(af, controller.state.value.focus.target)
        assertEquals(NormalizedPoint(.2f, .8f), controller.state.value.spotAe.target)
    }

    @Test
    fun spotAeRemainsActiveWhileChangingFocusMode() = withController { controller ->
        controller.dispatch(ActivateSpotAe(NormalizedPoint(.7f, .4f)))
        val spot = controller.state.value.spotAe.target
        controller.dispatch(OpenFocusSelector)
        controller.dispatch(SetFocusMode(FocusMode.Mf))
        assertEquals(FocusMode.Mf, controller.state.value.focus.mode)
        assertTrue(controller.state.value.spotAe.active)
        assertEquals(spot, controller.state.value.spotAe.target)
        controller.dispatch(SetManualFocus(.25f))
        assertEquals(.25f, controller.state.value.focus.mfNormalized)
        assertTrue(controller.state.value.spotAe.active)
    }

    @Test
    fun controlSurfaceStyleCanSwitchWithoutChangingCaptureState() = withController { controller ->
        val before = controller.state.value
        controller.dispatch(SetControlSurfaceStyle(ControlSurfaceStyle.Basic))
        val after = controller.state.value
        assertEquals(ControlSurfaceStyle.Basic, after.controlSurfaceStyle)
        assertEquals(before.exposureControl, after.exposureControl)
        assertEquals(before.exposureApplied, after.exposureApplied)
        assertEquals(before.focus, after.focus)
        assertEquals(before.spotAe, after.spotAe)
    }

    @Test
    fun requestedExposureCanDivergeFromAppliedMonitorState() {
        val before = CaptureUiState(capabilities)
        val nextIso = capabilities.iso.candidates[before.requestedCandidateIndexFor(ExposureParameter.Iso) + 1]
        val requested = CaptureTransitions.requestExposureCandidate(before, ExposureParameter.Iso, nextIso.id)

        assertEquals(nextIso.id, requested.exposureControl.requestedIsoId)
        assertEquals(before.exposureApplied.iso, requested.exposureApplied.iso)

        val applied =
            CaptureTransitions.reportAppliedExposureCandidate(
                requested,
                requested.cameraContextGeneration,
                ExposureParameter.Iso,
                nextIso.id
            )
        assertEquals(nextIso.displayLabel, applied.exposureApplied.iso?.displayLabel)
        assertEquals(nextIso.id, applied.exposureApplied.iso?.sourceCandidateId)
    }

    @Test
    fun inMemoryControllerRoutesStableCandidateIdAndAcknowledgesAppliedValue() = withController { controller ->
        val candidate = capabilities.iso.candidates[8]
        controller.dispatch(SetExposureCandidate(ExposureParameter.Iso, candidate.id))
        assertEquals(candidate.id, controller.state.value.exposureControl.requestedIsoId)
        assertEquals(
            candidate.id,
            controller.state.value.exposureApplied.iso
                ?.sourceCandidateId
        )
        assertEquals(
            candidate.displayLabel,
            controller.state.value.exposureApplied.iso
                ?.displayLabel
        )
    }

    @Test
    fun exposureCandidateIdentityIsNotItsListPosition() {
        val candidate = capabilities.iso.candidates[6]
        val reordered = capabilities.iso.copy(candidates = capabilities.iso.candidates.reversed())
        assertEquals(candidate.displayLabel, reordered.candidate(candidate.id)?.displayLabel)
        assertTrue(reordered.indexOf(candidate.id) != capabilities.iso.indexOf(candidate.id))
    }

    @Test
    fun capabilityReplacementPreservesValidRequestedIdsAndAppliedReadouts() {
        val before = CaptureUiState(capabilities)
        val requestedIso = capabilities.iso.candidates[8]
        val requested = CaptureTransitions.requestExposureCandidate(before, ExposureParameter.Iso, requestedIso.id)
        val appliedBefore = requested.exposureApplied

        val replacement =
            capabilities.copy(
                lenses = listOf(capabilities.lenses[1], capabilities.lenses[2]),
                manualFocus = capabilities.manualFocus.copy(minNormalized = .2f, maxNormalized = .8f)
            )
        val after = CaptureTransitions.applyCameraCapabilities(requested, replacement)

        assertEquals(requestedIso.id, after.exposureControl.requestedIsoId)
        assertEquals(ExposureAppliedState.pending(), after.exposureApplied)
        assertEquals(capabilities.lenses[1].id, after.selectedLensId)
    }

    @Test
    fun capabilityReplacementFallsBackInvalidRequestsAndExitsUnsupportedMf() {
        val before =
            CaptureUiState(capabilities).copy(
                selectedLensId = capabilities.lenses.last().id,
                exposureControl =
                    ExposureControlState(
                        mode = ExposureMode.Manual,
                        requestedShutterId =
                            capabilities.shutter.candidates
                                .last()
                                .id,
                        requestedIsoId =
                            capabilities.iso.candidates
                                .last()
                                .id,
                        requestedEvId =
                            capabilities.ev.candidates
                                .last()
                                .id
                    ),
                focus =
                    FocusUiState(
                        mode = FocusMode.Mf,
                        mfNormalized = .9f,
                        backendNativeReadout = "old-camera"
                    )
            )
        val replacement =
            CaptureFixtures.baseline().copy(
                lenses = listOf(LensCapability("new-main", "MAIN")),
                shutter =
                    capabilities.shutter.copy(
                        candidates = capabilities.shutter.candidates.take(5),
                        fullStopAnchorIds =
                            capabilities.shutter.fullStopAnchorIds.intersect(
                                capabilities.shutter.candidates
                                    .take(5)
                                    .mapTo(mutableSetOf()) { it.id }
                            ),
                        initialCandidateId = capabilities.shutter.candidates[2].id
                    ),
                iso =
                    capabilities.iso.copy(
                        candidates = capabilities.iso.candidates.take(5),
                        fullStopAnchorIds =
                            capabilities.iso.fullStopAnchorIds.intersect(
                                capabilities.iso.candidates
                                    .take(5)
                                    .mapTo(mutableSetOf()) { it.id }
                            ),
                        initialCandidateId = capabilities.iso.candidates[2].id
                    ),
                ev =
                    capabilities.ev.copy(
                        candidates = capabilities.ev.candidates.subList(20, 41),
                        fullStopAnchorIds =
                            capabilities.ev.fullStopAnchorIds.intersect(
                                capabilities.ev.candidates
                                    .subList(20, 41)
                                    .mapTo(mutableSetOf()) { it.id }
                            ),
                        initialCandidateId = capabilities.ev.candidates[30].id
                    ),
                manualFocus =
                    ManualFocusCapability(
                        supported = false,
                        minNormalized = .25f,
                        maxNormalized = .75f
                    )
            )

        val after = CaptureTransitions.applyCameraCapabilities(before, replacement)

        assertEquals(replacement.shutter.initialCandidateId, after.exposureControl.requestedShutterId)
        assertEquals(replacement.iso.initialCandidateId, after.exposureControl.requestedIsoId)
        assertEquals(replacement.ev.initialCandidateId, after.exposureControl.requestedEvId)
        assertEquals("new-main", after.selectedLensId)
        assertEquals(FocusMode.Af, after.focus.mode)
        assertEquals(replacement.manualFocus.maxNormalized, after.focus.mfNormalized)
        assertNull(after.focus.backendNativeReadout)
        assertEquals(ExposureAppliedState.pending(), after.exposureApplied)
        assertEquals(TargetStatus.Hidden, after.focus.status)
        assertFalse(after.spotAe.active)
        assertEquals(TargetStatus.Hidden, after.spotAe.status)
    }

    @Test
    fun capabilityReplacementDropsFaceDetections() {
        val before =
            CaptureUiState(capabilities).copy(
                faceDetections = listOf(FaceDetection(.1f, .2f, .3f, .4f, 80))
            )
        val replacement = capabilities.copy(lenses = listOf(LensCapability("camera-b", "B")))
        assertTrue(CaptureTransitions.applyCameraCapabilities(before, replacement).faceDetections.isEmpty())
    }

    @Test
    fun inMemoryControllerAppliesCapabilitiesThroughApplicationBoundary() = withController { controller ->
        val replacement =
            capabilities.copy(
                lenses = listOf(LensCapability("replacement", "R")),
                manualFocus = ManualFocusCapability(false)
            )
        controller.dispatch(ApplyCameraCapabilities(replacement))
        assertEquals(replacement, controller.state.value.capabilities)
        assertEquals("replacement", controller.state.value.selectedLensId)
    }

    @Test
    fun capabilityReplacementInvalidatesSettlingTargetsAndAppliedExposure() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val controller =
            InMemoryCaptureScreenController(
                capabilities = capabilities,
                timings = CaptureSimulationTimings(settleDelayMs = 25L),
                dispatcher = dispatcher
            )
        try {
            controller.dispatch(FocusAt(NormalizedPoint(.2f, .3f)))
            controller.dispatch(ActivateSpotAe(NormalizedPoint(.7f, .4f)))
            assertEquals(TargetStatus.Settling, controller.state.value.focus.status)
            assertEquals(TargetStatus.Settling, controller.state.value.spotAe.status)

            val replacement = capabilities.copy(lenses = listOf(LensCapability("camera-b", "B")))
            controller.dispatch(ApplyCameraCapabilities(replacement))

            assertEquals(TargetStatus.Hidden, controller.state.value.focus.status)
            assertFalse(controller.state.value.spotAe.active)
            assertEquals(TargetStatus.Hidden, controller.state.value.spotAe.status)
            assertEquals(ExposureAppliedState.pending(), controller.state.value.exposureApplied)

            advanceTimeBy(100L)
            runCurrent()
            assertEquals(TargetStatus.Hidden, controller.state.value.focus.status)
            assertEquals(TargetStatus.Hidden, controller.state.value.spotAe.status)
        } finally {
            controller.close()
        }
    }

    @Test
    fun freshBackendExposureReportRepresentsNewCameraContext() {
        val before = CaptureUiState(capabilities)
        val replacement = capabilities.copy(lenses = listOf(LensCapability("camera-b", "B")))
        val pending = CaptureTransitions.applyCameraCapabilities(before, replacement)
        assertEquals(ExposureAppliedState.pending(), pending.exposureApplied)

        val refreshed =
            CaptureTransitions.reportAppliedExposure(
                pending,
                contextGeneration = pending.cameraContextGeneration,
                shutter = AppliedExposureReadout("1/60"),
                iso = AppliedExposureReadout("100"),
                evOrMeter = AppliedExposureReadout("+0.0")
            )
        assertEquals("1/60", refreshed.exposureApplied.shutter?.displayLabel)
        assertEquals("100", refreshed.exposureApplied.iso?.displayLabel)
        assertEquals("+0.0", refreshed.exposureApplied.evOrMeter?.displayLabel)

        val stale =
            CaptureTransitions.reportAppliedExposure(
                pending,
                contextGeneration = pending.cameraContextGeneration - 1L,
                shutter = AppliedExposureReadout("stale"),
                iso = AppliedExposureReadout("stale"),
                evOrMeter = AppliedExposureReadout("stale")
            )
        assertEquals(ExposureAppliedState.pending(), stale.exposureApplied)
    }

    @Test
    fun usableCameraCapabilitiesRequireAtLeastOneLens() {
        assertFailsWith<IllegalArgumentException> { capabilities.copy(lenses = emptyList()) }
    }

    @Test
    fun physicalOrientationUsesHysteresisNearTransitions() {
        assertEquals(Orientation.Portrait, classifyPhysicalOrientation(45, Orientation.Portrait))
        assertEquals(Orientation.Landscape, classifyPhysicalOrientation(45, Orientation.Landscape))
        assertEquals(Orientation.Landscape, classifyPhysicalOrientation(90, Orientation.Portrait))
        assertEquals(Orientation.Portrait, classifyPhysicalOrientation(180, Orientation.Landscape))
    }

    @Test
    fun physicalAngleConvertsToDisplayRotationConvention() {
        assertEquals(0, physicalAngleToDisplayRotation(0))
        assertEquals(270, physicalAngleToDisplayRotation(90))
        assertEquals(180, physicalAngleToDisplayRotation(180))
        assertEquals(90, physicalAngleToDisplayRotation(270))
        assertEquals(270, physicalAngleToDisplayRotation(89))
        assertEquals(90, physicalAngleToDisplayRotation(271))
    }

    @Test
    fun spotAeCannotSettleWhileTargetIsBeingDragged() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val controller =
            InMemoryCaptureScreenController(
                capabilities = capabilities,
                timings = CaptureSimulationTimings(settleDelayMs = 15L),
                dispatcher = dispatcher
            )
        try {
            controller.dispatch(ActivateSpotAe(NormalizedPoint(.7f, .4f)))
            controller.dispatch(MoveSpotAe(NormalizedPoint(.2f, .8f)))
            advanceTimeBy(30L)
            runCurrent()
            assertEquals(TargetStatus.Settling, controller.state.value.spotAe.status)

            controller.dispatch(FinishSpotAeMove)
            advanceTimeBy(15L)
            runCurrent()
            assertEquals(TargetStatus.Settled, controller.state.value.spotAe.status)
        } finally {
            controller.close()
        }
    }

    @Test
    fun newerCaptureOwnsFlashTimeout() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val controller =
            InMemoryCaptureScreenController(
                capabilities = capabilities,
                timings = CaptureSimulationTimings(captureFlashMs = 30L, captureProcessingBaseMs = 1_000L),
                dispatcher = dispatcher
            )
        try {
            controller.dispatch(TriggerCapture)
            advanceTimeBy(20L)
            controller.dispatch(TriggerCapture)
            advanceTimeBy(20L)
            runCurrent()
            assertTrue(controller.state.value.captureFlash)

            advanceTimeBy(10L)
            runCurrent()
            assertFalse(controller.state.value.captureFlash)
        } finally {
            controller.close()
        }
    }

    @Test
    fun multipleCapturesAreAllowedWhileProcessing() = withController { controller ->
        repeat(3) { controller.dispatch(TriggerCapture) }
        assertEquals(3, controller.state.value.pendingCaptures.size)
    }

    @Test
    fun restoreAppliesFilmSimPreference() {
        fun restore(filmSim: Boolean) = RestoreCapturePreferences(
            jpegEnabled = true,
            grid = GridMode.Off,
            armedOverlays = emptySet(),
            falseColorManual = false,
            activeScopes = emptyList(),
            waveformMode = WaveformMode.Luma,
            filmSimEnabled = filmSim
        )

        assertTrue(CaptureReducer.reduce(CaptureUiState(capabilities), restore(true)).filmSimEnabled)
        assertFalse(CaptureReducer.reduce(CaptureUiState(capabilities), restore(false)).filmSimEnabled)
    }

    @Test
    fun selfTimerCyclesOffThreeFiveTen() {
        var state = CaptureUiState(capabilities)
        assertEquals(SelfTimer.Off, state.selfTimer)
        state = CaptureReducer.reduce(state, CycleSelfTimer)
        assertEquals(SelfTimer.ThreeSeconds, state.selfTimer)
        state = CaptureReducer.reduce(state, CycleSelfTimer)
        assertEquals(SelfTimer.FiveSeconds, state.selfTimer)
        state = CaptureReducer.reduce(state, CycleSelfTimer)
        assertEquals(SelfTimer.TenSeconds, state.selfTimer)
        state = CaptureReducer.reduce(state, CycleSelfTimer)
        assertEquals(SelfTimer.Off, state.selfTimer)
    }

    @Test
    fun selfTimerCountdownStartsTicksAndCancels() {
        var state = CaptureUiState(capabilities)
        state = CaptureReducer.reduce(
            state,
            SetCaptureSelfTimer(com.rawr.camera.settings.model.SelfTimer.ThreeSeconds)
        )
        assertNull(state.selfTimerRemainingMs)
        state = CaptureReducer.reduce(state, StartSelfTimerCountdown)
        assertEquals(3000L, state.selfTimerRemainingMs)
        val runId = state.selfTimerRunId
        // Stale run ids are ignored.
        state = CaptureReducer.reduce(state, TickSelfTimer(2500L, runId + 99))
        assertEquals(3000L, state.selfTimerRemainingMs)
        state = CaptureReducer.reduce(state, TickSelfTimer(2500L, runId))
        assertEquals(2500L, state.selfTimerRemainingMs)
        state = CaptureReducer.reduce(state, TickSelfTimer(0L, runId))
        assertNull(state.selfTimerRemainingMs)
        // Start-while-running is a no-op; cancel clears.
        state = CaptureReducer.reduce(state, StartSelfTimerCountdown)
        val running = state.selfTimerRemainingMs
        assertTrue(running != null && running > 0)
        state = CaptureReducer.reduce(state, StartSelfTimerCountdown)
        assertEquals(running, state.selfTimerRemainingMs)
        state = CaptureReducer.reduce(state, CancelSelfTimer)
        assertNull(state.selfTimerRemainingMs)
        // Ticks after cancel are ignored.
        state = CaptureReducer.reduce(state, TickSelfTimer(1000L, runId))
        assertNull(state.selfTimerRemainingMs)
    }

    @Test
    fun selfTimerStartIsNoopWhenOff() {
        val state = CaptureReducer.reduce(CaptureUiState(capabilities), StartSelfTimerCountdown)
        assertNull(state.selfTimerRemainingMs)
    }

    @Test
    fun selfTimerCancelledOnModeSwitch() {
        var state = CaptureUiState(capabilities)
        state = CaptureReducer.reduce(
            state,
            SetCaptureSelfTimer(com.rawr.camera.settings.model.SelfTimer.FiveSeconds)
        )
        state = CaptureReducer.reduce(state, StartSelfTimerCountdown)
        assertTrue(state.selfTimerRemainingMs != null)
        state = CaptureReducer.reduce(state, SetCaptureMode(CaptureMode.Video))
        assertNull(state.selfTimerRemainingMs)
    }

    @Test
    fun selfTimerCaptureFiresAfterCountdown() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val controller =
            InMemoryCaptureScreenController(
                capabilities = capabilities,
                timings = CaptureSimulationTimings(captureFlashMs = 30L, captureProcessingBaseMs = 10_000L),
                dispatcher = dispatcher
            )
        try {
            controller.dispatch(
                SetCaptureSelfTimer(com.rawr.camera.settings.model.SelfTimer.ThreeSeconds)
            )
            controller.dispatch(TriggerCapture)
            runCurrent()
            // Counting: no capture yet.
            assertTrue(controller.state.value.selfTimerRemainingMs != null)
            assertTrue(controller.state.value.pendingCaptures.isEmpty())
            advanceTimeBy(3_100L)
            runCurrent()
            assertNull(controller.state.value.selfTimerRemainingMs)
            assertEquals(1, controller.state.value.pendingCaptures.size)
        } finally {
            controller.close()
        }
    }

    @Test
    fun selfTimerSecondPressCancels() = runTest {
        val dispatcher = StandardTestDispatcher(testScheduler)
        val controller =
            InMemoryCaptureScreenController(
                capabilities = capabilities,
                timings = CaptureSimulationTimings(captureFlashMs = 30L, captureProcessingBaseMs = 10_000L),
                dispatcher = dispatcher
            )
        try {
            controller.dispatch(
                SetCaptureSelfTimer(com.rawr.camera.settings.model.SelfTimer.FiveSeconds)
            )
            controller.dispatch(TriggerCapture)
            runCurrent()
            assertTrue(controller.state.value.selfTimerRemainingMs != null)
            controller.dispatch(TriggerCapture)
            runCurrent()
            assertNull(controller.state.value.selfTimerRemainingMs)
            advanceTimeBy(6_000L)
            runCurrent()
            assertTrue(controller.state.value.pendingCaptures.isEmpty())
        } finally {
            controller.close()
        }
    }

    private fun state(mode: ExposureMode) = CaptureUiState(capabilities).let {
        it.copy(exposureControl = it.exposureControl.copy(mode = mode))
    }

    @Test
    fun compactLockMappingEntersPriorityModes() {
        assertEquals(
            ExposureMode.ShutterPriority,
            CaptureTransitions.lockModeFor(ExposureParameter.Shutter, ExposureMode.Auto)
        )
        assertEquals(
            ExposureMode.Manual,
            CaptureTransitions.lockModeFor(ExposureParameter.Shutter, ExposureMode.IsoPriority)
        )
        assertEquals(
            ExposureMode.IsoPriority,
            CaptureTransitions.lockModeFor(ExposureParameter.Iso, ExposureMode.Auto)
        )
        assertEquals(
            ExposureMode.Manual,
            CaptureTransitions.lockModeFor(ExposureParameter.Iso, ExposureMode.ShutterPriority)
        )
        assertNull(CaptureTransitions.lockModeFor(ExposureParameter.Shutter, ExposureMode.Manual))
        assertNull(CaptureTransitions.lockModeFor(ExposureParameter.Ev, ExposureMode.Auto))
    }

    @Test
    fun compactUnlockMappingReturnsTowardAuto() {
        assertEquals(
            ExposureMode.IsoPriority,
            CaptureTransitions.unlockModeFor(ExposureParameter.Shutter, ExposureMode.Manual)
        )
        assertEquals(
            ExposureMode.Auto,
            CaptureTransitions.unlockModeFor(ExposureParameter.Shutter, ExposureMode.ShutterPriority)
        )
        assertEquals(
            ExposureMode.ShutterPriority,
            CaptureTransitions.unlockModeFor(ExposureParameter.Iso, ExposureMode.Manual)
        )
        assertEquals(
            ExposureMode.Auto,
            CaptureTransitions.unlockModeFor(ExposureParameter.Iso, ExposureMode.IsoPriority)
        )
        assertNull(CaptureTransitions.unlockModeFor(ExposureParameter.Shutter, ExposureMode.Auto))
        assertNull(CaptureTransitions.unlockModeFor(ExposureParameter.Ev, ExposureMode.ShutterPriority))
    }

    @Test
    fun compactMappingFallsBackToManualWithoutPriorityModes() {
        val noPriority = setOf(ExposureMode.Auto, ExposureMode.Manual)
        for (parameter in listOf(ExposureParameter.Shutter, ExposureParameter.Iso)) {
            assertEquals(ExposureMode.Manual, CaptureTransitions.lockModeFor(parameter, ExposureMode.Auto, noPriority))
            assertEquals(ExposureMode.Auto, CaptureTransitions.unlockModeFor(parameter, ExposureMode.Manual, noPriority))
        }
    }

    @Test
    fun compactMappingUsesOnlyTheSupportedPriorityAxis() {
        val isoOnly = setOf(ExposureMode.Auto, ExposureMode.Manual, ExposureMode.IsoPriority)
        assertEquals(ExposureMode.Manual, CaptureTransitions.lockModeFor(ExposureParameter.Shutter, ExposureMode.Auto, isoOnly))
        assertEquals(ExposureMode.IsoPriority, CaptureTransitions.lockModeFor(ExposureParameter.Iso, ExposureMode.Auto, isoOnly))
        assertEquals(
            ExposureMode.IsoPriority,
            CaptureTransitions.unlockModeFor(ExposureParameter.Shutter, ExposureMode.Manual, isoOnly)
        )
        assertEquals(ExposureMode.Auto, CaptureTransitions.unlockModeFor(ExposureParameter.Iso, ExposureMode.Manual, isoOnly))
    }

    @Test
    fun compactMappingIsNullWhenNoLockModeIsSupported() {
        val autoOnly = setOf(ExposureMode.Auto)
        assertNull(CaptureTransitions.lockModeFor(ExposureParameter.Shutter, ExposureMode.Auto, autoOnly))
        assertNull(CaptureTransitions.lockModeFor(ExposureParameter.Iso, ExposureMode.Auto, autoOnly))
    }

    @Test
    fun compactLockedFlagsFollowExposureMode() {
        assertFalse(CaptureTransitions.isParameterLocked(state(ExposureMode.Auto), ExposureParameter.Shutter))
        assertTrue(
            CaptureTransitions.isParameterLocked(state(ExposureMode.ShutterPriority), ExposureParameter.Shutter)
        )
        assertTrue(CaptureTransitions.isParameterLocked(state(ExposureMode.Manual), ExposureParameter.Iso))
        assertFalse(CaptureTransitions.isParameterLocked(state(ExposureMode.Auto), ExposureParameter.Iso))
    }

    @Test
    fun manualEvButtonDrivesRenderExposureOnlyWhenTonemapOwnsTheRender() {
        val manual = state(ExposureMode.Manual)
        assertTrue(CaptureTransitions.evAdjustsRenderExposure(manual))
        assertFalse(CaptureTransitions.evAdjustsRenderExposure(state(ExposureMode.ShutterPriority)))
        assertFalse(CaptureTransitions.evAdjustsRenderExposure(manual.copy(filmSimEnabled = true)))
        val video = manual.copy(captureMode = CaptureMode.Video, filmSimEnabled = true)
        assertTrue(CaptureTransitions.evAdjustsRenderExposure(video))
        assertFalse(CaptureTransitions.evAdjustsRenderExposure(video.copy(videoLogEnabled = true)))
    }

    @Test
    fun renderExposureScrubClampsToContractAndSyncQuantizes() {
        val base = state(ExposureMode.Manual)
        assertEquals(3, CaptureTransitions.scrubRenderExposure(base, 3).renderExposureTenths)
        assertEquals(50, CaptureTransitions.scrubRenderExposure(base, 80).renderExposureTenths)
        assertEquals(-50, CaptureTransitions.scrubRenderExposure(base, -80).renderExposureTenths)
        assertEquals(-7, renderExposureTenthsOf(-.7f))
        assertEquals(0, renderExposureTenthsOf(Float.NaN))
        assertEquals(.3f, TonemapControlContract.exposureEvFromTenths(3))
    }

    @Test
    fun compactTogglesFlipPresentationState() = withController { controller ->
        val before = controller.state.value
        assertEquals(CaptureControlLayout.Compact, before.captureLayout)
        controller.dispatch(SetCaptureLayout(CaptureControlLayout.Classic))
        assertEquals(CaptureControlLayout.Classic, controller.state.value.captureLayout)
        controller.dispatch(ToggleFilmSim)
        assertTrue(controller.state.value.filmSimEnabled)
        controller.dispatch(ToggleMultiframe)
        assertTrue(controller.state.value.experimentalMultiframeEnabled)
    }

    @Test
    fun wbSnapUsesNativeFallbackEstimate() {
        // Live gains from a very warm scene: the display follows the lenient
        // live estimate (same source as the per-frame follow, header, compact
        // strip and monitor) so all surfaces stay consistent and realtime.
        val before = CaptureUiState(capabilities).copy(
            whiteBalanceMode = WhiteBalanceMode.ManualTempTint,
            whiteBalanceTemperatureK = 5475,
            whiteBalanceTint = 0,
            autoWhiteBalanceTemperatureK = 7600,
            autoWhiteBalanceTint = -8
        )
        val after = CaptureTransitions.snapWhiteBalanceDisplayToMode(before, WhiteBalanceMode.Auto, 5475, 0)
        assertEquals(WhiteBalanceMode.Auto, after.whiteBalanceMode)
        assertEquals(7600, after.whiteBalanceTemperatureK)
        assertEquals(-8, after.whiteBalanceTint)
    }

    @Test
    fun wbSnapUsesNativeNeutralEstimate() {
        val before = CaptureUiState(capabilities).copy(
            whiteBalanceMode = WhiteBalanceMode.ManualTempTint,
            whiteBalanceTemperatureK = 5475,
            whiteBalanceTint = 0,
            autoWhiteBalanceTemperatureK = 6600,
            autoWhiteBalanceTint = 0
        )
        val after = CaptureTransitions.snapWhiteBalanceDisplayToMode(before, WhiteBalanceMode.Auto, 5475, 0)
        assertEquals(6600, after.whiteBalanceTemperatureK)
        assertEquals(0, after.whiteBalanceTint)
    }

    @Test
    fun lockWhiteBalanceKeepsDisplayedValuesWithoutReseed() {
        // Gains that would lenient-seed elsewhere must not move the display here.
        val before = CaptureUiState(capabilities).copy(
            whiteBalanceMode = WhiteBalanceMode.Auto,
            whiteBalanceTemperatureK = 5200,
            whiteBalanceTint = 3,
            autoWhiteBalanceTemperatureK = 6600,
            autoWhiteBalanceTint = 0
        )
        val after = CaptureTransitions.lockWhiteBalance(before, 5200, 3)
        assertEquals(WhiteBalanceMode.ManualTempTint, after.whiteBalanceMode)
        assertEquals(5200, after.whiteBalanceTemperatureK)
        assertEquals(3, after.whiteBalanceTint)
    }

    private inline fun withController(
        timings: CaptureSimulationTimings = CaptureSimulationTimings(),
        block: (InMemoryCaptureScreenController) -> Unit
    ) {
        val controller = InMemoryCaptureScreenController(capabilities = capabilities, timings = timings)
        try {
            block(controller)
        } finally {
            controller.close()
        }
    }
}
