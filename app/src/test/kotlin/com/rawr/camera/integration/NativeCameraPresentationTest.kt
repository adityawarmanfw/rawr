package com.rawr.camera.integration

import com.rawr.camera.model.*
import org.junit.Assert.*
import org.junit.Test

class NativeCameraPresentationTest {
    @Test
    fun manualFocusEntrySeedsOnlyDisplayFromLatestTelemetry() {
        val capabilities = NativeCapabilityProjection.project(snapshot()).capabilities.copy(
            manualFocus = ManualFocusCapability(supported = true, minimumFocusDistance = 10f)
        )
        val current = CaptureUiState(capabilities, focus = FocusUiState(
            mode = FocusMode.Af, appliedFocusDiopters = 4f, mfNormalized = 0.9f
        ))
        val pending = NativeFocusPresentation.requestMode(current, FocusMode.Mf)
        assertEquals(FocusMode.Mf, pending.focus.mode)
        assertEquals(0.6f, pending.focus.mfNormalized, 0.00001f)
        val dragged = pending.copy(focus = pending.focus.copy(mfNormalized = 0.3f))
        assertEquals(0.3f, NativeFocusPresentation.requestMode(dragged, FocusMode.Mf).focus.mfNormalized)
        // The authoritative acknowledgment may seed from a newer lens position.
        val acknowledged = NativeFocusPresentation.project(pending.focus, snapshot().copy(
            focusMode = 2, focusRequestId = 2, requestedManualFocusNormalized = 0.55f
        ), 2, false)
        assertEquals(0.55f, acknowledged.mfNormalized)
    }

    @Test
    fun invalidOrMissingManualFocusSeedRetainsRailAndUnsupportedModeIsIgnored() {
        val capabilities = NativeCapabilityProjection.pending().copy(
            manualFocus = ManualFocusCapability(supported = true, minimumFocusDistance = 10f)
        )
        val current = CaptureUiState(capabilities, focus = FocusUiState(mfNormalized = 0.4f))
        listOf(null, Float.NaN, Float.POSITIVE_INFINITY).forEach { distance ->
            val seeded = NativeFocusPresentation.requestMode(current.copy(
                focus = current.focus.copy(appliedFocusDiopters = distance)
            ), FocusMode.Mf)
            assertEquals(0.4f, seeded.focus.mfNormalized)
        }
        val invalidMinimum = current.copy(capabilities = capabilities.copy(
            manualFocus = capabilities.manualFocus.copy(minimumFocusDistance = Float.NaN)
        ), focus = current.focus.copy(appliedFocusDiopters = 4f))
        assertEquals(0.4f, NativeFocusPresentation.requestMode(invalidMinimum, FocusMode.Mf).focus.mfNormalized)
        val unsupported = current.copy(capabilities = NativeCapabilityProjection.pending())
        assertSame(unsupported, NativeFocusPresentation.requestMode(unsupported, FocusMode.Mf))
        assertSame(unsupported, NativeFocusPresentation.requestMode(unsupported, FocusMode.AfLock))
    }

    @Test
    fun manualFocusGestureEntersModeAndClampsRailAtomically() {
        val capabilities = NativeCapabilityProjection.pending().copy(
            manualFocus = ManualFocusCapability(supported = true, minNormalized = 0.1f, maxNormalized = 0.8f)
        )
        val current = CaptureUiState(capabilities)
        val dragged = NativeFocusPresentation.requestManualFocus(current, 2f)
        assertEquals(FocusMode.Mf, dragged.focus.mode)
        assertEquals(0.8f, dragged.focus.mfNormalized)
        assertSame(current, NativeFocusPresentation.requestManualFocus(current, Float.NaN))
        assertEquals(0.1f, NativeFocusPresentation.requestManualFocus(current, -1f).focus.mfNormalized)
    }

    @Test
    fun angleProjectionUsesNativeValuesAndStableIds() {
        val snapshot = snapshot().copy(
            videoMode = true,
            videoPreviewFps = 24,
            requestedShutterAngleDegrees = 180.0,
            // Deliberately differs from a formula: the adapter must use the
            // supplied native request value, not independently calculate it.
            shutterAngleChoices = listOf(NativeShutterAngleChoice(180.0, 1234567))
        )
        val projected = NativeCapabilityProjection.project(snapshot, 24)
        assertEquals(mapOf("ssa:180" to 1234567L), projected.shutterValuesNs)
        assertEquals(mapOf("ssa:180" to 180.0), projected.shutterAnglesDeg)
        assertEquals("ssa:180", projected.capabilities.shutter.initialCandidateId)
        assertEquals("180°", projected.capabilities.shutter.candidates.single().displayLabel)
        val changed = NativeCapabilityProjection.project(snapshot.copy(
            videoPreviewFps = 30,
            shutterAngleChoices = listOf(NativeShutterAngleChoice(180.0, 16666666))
        ), 30)
        assertEquals(projected.shutterAnglesDeg, changed.shutterAnglesDeg)
        assertEquals(16666666L, changed.shutterValuesNs["ssa:180"])
    }

    @Test
    fun unavailableVideoAnglesDoNotOfferPhotoRequests() {
        val projected = NativeCapabilityProjection.project(snapshot().copy(videoMode = true), 30)
        assertTrue(projected.shutterValuesNs.isEmpty())
        assertTrue(projected.shutterAnglesDeg.isEmpty())
        assertEquals("pending", projected.capabilities.shutter.initialCandidateId)
    }

    @Test
    fun photoProjectionKeepsAbsoluteSpeeds() {
        val projected = NativeCapabilityProjection.project(snapshot())
        assertTrue(projected.shutterAnglesDeg.isEmpty())
        assertTrue(projected.shutterValuesNs.isNotEmpty())
        assertTrue(projected.capabilities.shutter.initialCandidateId.startsWith("ss:"))
    }

    @Test
    fun olderSnapshotPreservesPendingModeBoxAndRail() {
        val current = FocusUiState(mode = FocusMode.Mf, status = TargetStatus.Settling, mfNormalized = 0.42f)
        val projected = NativeFocusPresentation.project(current, snapshot().copy(
            focusRequestId = 1,
            appliedFocusDistance = 2f
        ), latestRequestId = 2, distanceReadoutTrustworthy = true)
        assertEquals(current.mode, projected.mode)
        assertEquals(current.status, projected.status)
        assertEquals(current.mfNormalized, projected.mfNormalized)
        assertEquals(2f, projected.appliedFocusDiopters)
        assertNotNull(projected.backendNativeReadout)
    }

    @Test
    fun acceptedTapAndNativeExpiryProjectWithoutUiTimer() {
        val current = FocusUiState(mode = FocusMode.Af, status = TargetStatus.Settling)
        val active = snapshot().copy(focusRequestId = 2, tapAfActive = true, afState = 4)
        val settled = NativeFocusPresentation.project(current, active, 2, false)
        assertEquals(TargetStatus.Settled, settled.status)
        val cleared = NativeFocusPresentation.project(settled, active.copy(tapAfActive = false), 2, false)
        assertEquals(TargetStatus.Hidden, cleared.status)
        assertNull(cleared.backendNativeReadout)
    }

    @Test
    fun oldActiveTapCannotReopenPendingClear() {
        val current = FocusUiState(mode = FocusMode.Af, status = TargetStatus.Hidden)
        val active = snapshot().copy(focusRequestId = 1, tapAfActive = true, afState = 4)
        assertEquals(TargetStatus.Hidden, NativeFocusPresentation.project(current, active, 2, false).status)
        assertEquals(TargetStatus.Hidden, NativeFocusPresentation.project(
            current, active.copy(focusRequestId = 2, tapAfActive = false), 2, false
        ).status)
    }

    @Test
    fun rejectedCommandAcknowledgmentRestoresNativeState() {
        val current = FocusUiState(mode = FocusMode.Mf, status = TargetStatus.Settling, mfNormalized = 0.42f)
        val rejected = snapshot().copy(focusRequestId = 2, focusMode = 0, tapAfActive = false)
        val projected = NativeFocusPresentation.project(current, rejected, 2, false)
        assertEquals(FocusMode.Af, projected.mode)
        assertEquals(TargetStatus.Hidden, projected.status)
        assertEquals(rejected.requestedManualFocusNormalized, projected.mfNormalized)
    }

    @Test
    fun pendingWhiteBalanceGestureKeepsModeAndRailsUntilAcknowledged() {
        val current = CaptureUiState(NativeCapabilityProjection.project(snapshot()).capabilities).copy(
            whiteBalanceMode = WhiteBalanceMode.ManualTempTint,
            whiteBalanceTemperatureK = 4000,
            whiteBalanceTint = 7
        )
        val delayed = snapshot().copy(
            whiteBalanceRequestId = 1, hasAutoWbEstimate = true, autoWbTemperatureK = 3300, autoWbTint = 9
        )
        val pending = NativeWhiteBalancePresentation.project(current, delayed, 2)
        assertEquals(WhiteBalanceMode.ManualTempTint, pending.whiteBalanceMode)
        assertEquals(4000, pending.whiteBalanceTemperatureK)
        assertEquals(7, pending.whiteBalanceTint)
        assertEquals(3300, pending.autoWhiteBalanceTemperatureK)
        // Native entry may resolve a newer neutral than the optimistic UI seed.
        val accepted = NativeWhiteBalancePresentation.project(pending, delayed.copy(
            whiteBalanceRequestId = 2, whiteBalanceMode = 9, whiteBalanceTemperatureK = 4000, whiteBalanceTint = 9
        ), 2)
        assertEquals(9, accepted.whiteBalanceTint)
    }

    @Test
    fun pendingAutoSwitchIgnoresOldManualEchoAndRejectionRestoresNativeValues() {
        val current = CaptureUiState(NativeCapabilityProjection.project(snapshot()).capabilities).copy(
            whiteBalanceMode = WhiteBalanceMode.Auto, whiteBalanceTemperatureK = 6000, whiteBalanceTint = 2
        )
        val delayed = snapshot().copy(
            whiteBalanceMode = 9, whiteBalanceTemperatureK = 4000, whiteBalanceTint = 8, whiteBalanceRequestId = 1
        )
        val pending = NativeWhiteBalancePresentation.project(current, delayed, 2)
        assertEquals(WhiteBalanceMode.Auto, pending.whiteBalanceMode)
        assertEquals(6000, pending.whiteBalanceTemperatureK)
        val rejected = NativeWhiteBalancePresentation.project(pending, delayed.copy(whiteBalanceRequestId = 2), 2)
        assertEquals(WhiteBalanceMode.ManualTempTint, rejected.whiteBalanceMode)
        assertEquals(4000, rejected.whiteBalanceTemperatureK)
        assertEquals(8, rejected.whiteBalanceTint)
    }

    @Test
    fun whiteBalanceDisplayUsesNativeEstimateAndClearsUnavailableSeed() {
        val current = CaptureUiState(NativeCapabilityProjection.project(snapshot()).capabilities)
        val native = snapshot().copy(
            hasAutoWbGains = true, autoWbGainR = 4f, autoWbGainG = 4f, autoWbGainB = 4f,
            hasAutoWbEstimate = true, autoWbTemperatureK = 8037, autoWbTint = 4, autoWbEstimateCalibrated = true
        )
        val calibrated = NativeWhiteBalancePresentation.project(current, native, 0)
        assertEquals(8050, calibrated.whiteBalanceTemperatureK)
        assertEquals(4, calibrated.whiteBalanceTint)
        val fallback = NativeWhiteBalancePresentation.project(calibrated, native.copy(
            autoWbTemperatureK = 5600, autoWbTint = 5, autoWbEstimateCalibrated = false
        ), 0)
        assertEquals(5600, fallback.whiteBalanceTemperatureK)
        assertEquals(5, fallback.whiteBalanceTint)
        val missing = NativeWhiteBalancePresentation.project(fallback, native.copy(
            hasAutoWbEstimate = false, whiteBalanceTemperatureK = 5200, whiteBalanceTint = 0
        ), 0)
        assertNull(missing.autoWhiteBalanceTemperatureK)
        assertNull(missing.autoWhiteBalanceTint)
        assertEquals(5200, missing.whiteBalanceTemperatureK)
    }

    @Test
    fun telemetryUpdatesReuseCapabilityGridAndPreserveHeldRail() {
        val presenter = NativeCaptureSnapshotPresentation()
        val initial = CaptureUiState(NativeCapabilityProjection.pending(), selectedLensId = "main")
        val first = presenter.prepare(initial, snapshot())!!
        assertTrue(first.contextChanged)
        val accepted = first.project(initial, 0, 0)
        val projection = presenter.exposureProjection!!
        val selected = projection.shutterValuesNs.keys.last()
        val held = accepted.copy(exposureControl = accepted.exposureControl.copy(
            mode = ExposureMode.ShutterPriority, requestedShutterId = selected
        ))
        val next = presenter.prepare(held, snapshot().copy(
            semanticExposureMode = 2, exposureMode = 1, appliedExposureTimeNs = 12345678, appliedSensitivity = 250
        ))!!
        assertFalse(next.contextChanged)
        assertSame(projection, presenter.exposureProjection)
        val projected = next.project(held, 0, 0)
        assertEquals(selected, projected.exposureControl.requestedShutterId)
        assertEquals(ExposureFormat.formatShutterNs(12345678), projected.exposureApplied.shutter?.displayLabel)
        assertEquals("250", projected.exposureApplied.iso?.displayLabel)
        assertEquals(accepted.cameraContextGeneration, projected.cameraContextGeneration)
    }

    @Test
    fun nativeLensIsAdoptedWhenNothingOrAnUnknownLensIsSelected() {
        val lenses = listOf("14" to "14", "35" to "35")
        val fresh = CaptureUiState(NativeCapabilityProjection.pending(), selectedLensId = "")
        val adopted = NativeCaptureSnapshotPresentation().prepare(fresh, snapshot().copy(lensId = "14", profileLenses = lenses))!!
            .project(fresh, 0, 0)
        assertEquals("14", adopted.selectedLensId)
        assertEquals(listOf("14", "35"), adopted.capabilities.lenses.map { it.id })
        // The selected lens was renamed away: native fell back to its first lens.
        val renamed = CaptureUiState(NativeCapabilityProjection.pending(), selectedLensId = "main")
        assertEquals(
            "14",
            NativeCaptureSnapshotPresentation().prepare(renamed, snapshot().copy(lensId = "14", profileLenses = lenses))!!
                .project(renamed, 0, 0).selectedLensId
        )
        // A pending switch to 35 still ignores snapshots of the old lens.
        val switching = adopted.copy(selectedLensId = "35")
        assertNull(NativeCaptureSnapshotPresentation().prepare(switching, snapshot().copy(lensId = "14", profileLenses = lenses)))
    }

    @Test
    fun capabilityReplacementClearsCountdownAndOldCameraObservations() {
        val presenter = NativeCaptureSnapshotPresentation()
        val initial = CaptureUiState(NativeCapabilityProjection.pending(), selectedLensId = "main")
        val accepted = presenter.prepare(initial, snapshot())!!.project(initial, 0, 0).copy(
            selectedLensId = "tele",
            selfTimerRemainingMs = 2000, selfTimerRunId = 3,
            autoWhiteBalanceTemperatureK = 8000, autoWhiteBalanceTint = -3,
            faceDetections = listOf(FaceDetection(0.2f, 0.2f, 0.1f, 0.1f, 80))
        )
        assertNull(presenter.prepare(accepted, snapshot()))
        val replacement = presenter.prepare(accepted, snapshot().copy(
            generation = 8, lensId = "tele", hasAutoWbEstimate = false
        ))!!
        assertTrue(replacement.contextChanged)
        val next = replacement.project(accepted, 0, 0)
        assertEquals("tele", next.selectedLensId)
        assertNull(next.selfTimerRemainingMs)
        assertTrue(next.selfTimerRunId > accepted.selfTimerRunId)
        assertTrue(next.faceDetections.isEmpty())
        assertNull(next.autoWhiteBalanceTemperatureK)
        assertEquals(accepted.cameraContextGeneration + 1, next.cameraContextGeneration)
    }

    @Test
    fun videoGridWaitsForMatchingNativeModeAndFpsAndTracksChangedChoices() {
        val presenter = NativeCaptureSnapshotPresentation()
        val initial = CaptureUiState(NativeCapabilityProjection.pending(), selectedLensId = "main")
        val photo = presenter.prepare(initial, snapshot())!!.project(initial, 0, 0)
        val video = photo.copy(captureMode = CaptureMode.Video, videoFps = 24)
        assertNull(presenter.prepare(video, snapshot()))
        assertNull(presenter.exposureProjectionFor(video))
        val native = snapshot().copy(videoMode = true, videoPreviewFps = 24,
            shutterAngleChoices = listOf(NativeShutterAngleChoice(180.0, 1234567)))
        assertNull(presenter.prepare(video, native.copy(videoPreviewFps = 30)))
        val accepted = presenter.prepare(video, native)!!.project(video, 0, 0)
        assertEquals(1234567L, presenter.exposureProjectionFor(accepted)?.shutterValuesNs?.get("ssa:180"))
        val changed = presenter.prepare(accepted, native.copy(
            shutterAngleChoices = listOf(NativeShutterAngleChoice(180.0, 2222222))))!!
        assertTrue(changed.contextChanged)
        assertEquals(2222222L, presenter.exposureProjection!!.shutterValuesNs["ssa:180"])
    }

    @Test
    fun preparedSnapshotCanRetryAndCannotOverwriteLaterModeGesture() {
        val presenter = NativeCaptureSnapshotPresentation()
        val initial = CaptureUiState(NativeCapabilityProjection.pending(), selectedLensId = "main")
        val prepared = presenter.prepare(initial, snapshot())!!
        val changed = initial.copy(captureMode = CaptureMode.Video, videoFps = 24)
        assertSame(changed, prepared.project(changed, 0, 0))
        val retried = prepared.project(initial.copy(grid = GridMode.Thirds), 0, 0)
        assertEquals(GridMode.Thirds, retried.grid)
        assertEquals(initial.cameraContextGeneration + 1, retried.cameraContextGeneration)
        // Preparing must not mark capabilities as installed when projection was skipped.
        assertTrue(presenter.prepare(initial, snapshot())!!.contextChanged)
    }

    @Test
    fun changedCapabilityFactsRebuildGridWithinSameNativeGeneration() {
        val presenter = NativeCaptureSnapshotPresentation()
        val initial = CaptureUiState(NativeCapabilityProjection.pending(), selectedLensId = "main")
        val accepted = presenter.prepare(initial, snapshot())!!.project(initial, 0, 0)
        val changed = presenter.prepare(accepted, snapshot().copy(
            sensitivityMax = 6400, manualWhiteBalanceSupported = false, supportedAwbModes = listOf(1, 5)
        ))!!
        assertTrue(changed.contextChanged)
        val next = changed.project(accepted, 0, 0)
        assertFalse(next.capabilities.manualWhiteBalanceSupported)
        assertTrue(WhiteBalanceMode.Daylight in next.capabilities.supportedWhiteBalanceModes)
        assertEquals(6400, presenter.exposureProjection!!.isoValues.values.max())
    }

    private fun snapshot() = NativeCameraUiSnapshot(
        generation = 7,
        cameraId = "0",
        lensId = "main",
        sensitivityMin = 50,
        sensitivityMax = 3200,
        exposureTimeMinNs = 100000,
        exposureTimeMaxNs = 1000000000,
        evMinSteps = -6,
        evMaxSteps = 6,
        evStep = 1.0 / 3.0,
        manualExposureSupported = true,
        shutterPrioritySupported = true,
        isoPrioritySupported = true,
        tapAfSupported = true,
        manualFocusSupported = true,
        focusDistanceReadoutTrustworthy = true,
        minimumFocusDistance = 10f,
        hyperfocalDistance = 0f,
        exposureMode = 0,
        semanticExposureMode = 0,
        focusMode = 0,
        whiteBalanceMode = 1,
        whiteBalanceTemperatureK = 5200,
        whiteBalanceTint = 0,
        manualWhiteBalanceSupported = true,
        supportedAwbModes = listOf(1),
        hasAutoWbGains = false,
        autoWbGainR = 1f,
        autoWbGainG = 1f,
        autoWbGainB = 1f,
        requestedExposureTimeNs = 10000000,
        requestedSensitivity = 100,
        requestedEvSteps = 0,
        requestedManualFocusNormalized = 1f,
        appliedExposureTimeNs = null,
        appliedSensitivity = null,
        appliedEvSteps = null,
        afState = null,
        appliedFocusDistance = null
    )
}
