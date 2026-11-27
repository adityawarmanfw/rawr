package com.rawr.camera.model

import com.rawr.camera.fixtures.CaptureFixtures
import kotlin.test.Test
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * The two predicates here decide when peaking stands in for the camera's own
 * focus and when the level meter is allowed to claim a lock, so the cases that
 * matter are the ones where an indicator would be a lie.
 */
class FocusIndicationTest {
    private fun state(
        mode: FocusMode = FocusMode.Af,
        status: TargetStatus = TargetStatus.Hidden,
        cameraAfStatus: TargetStatus = TargetStatus.Settled,
        faces: List<FaceDetection> = emptyList()
    ) = CaptureUiState(
        CaptureFixtures.baseline(),
        focus = FocusUiState(mode = mode, status = status, cameraAfStatus = cameraAfStatus),
        faceDetections = faces
    )

    private val face = FaceDetection(x = .4f, y = .4f, w = .2f, h = .2f, score = 90)

    @Test
    fun passiveAutoWithoutFacesIsTracked() {
        assertTrue(state().focusTracked())
    }

    @Test
    fun manualFocusIsNeverTracked() {
        assertFalse(state(mode = FocusMode.Mf).focusTracked())
    }

    @Test
    fun facesOwnTheGraphic() {
        assertFalse(state(faces = listOf(face)).focusTracked())
    }

    @Test
    fun anOutstandingTapOwnsTheGraphic() {
        assertFalse(state(status = TargetStatus.Settling).focusTracked())
    }

    @Test
    fun onlyAReportedLockConfirmsFocus() {
        assertTrue(state(cameraAfStatus = TargetStatus.Settled).focusConfirmed())
        assertFalse(state(cameraAfStatus = TargetStatus.Settling).focusConfirmed())
        assertFalse(state(cameraAfStatus = TargetStatus.Failed).focusConfirmed())
        assertFalse(state(cameraAfStatus = TargetStatus.Hidden).focusConfirmed())
    }

    /**
     * A lock the photographer got by tapping, or one faces achieved, is still a
     * lock — the meter must not withhold it because the tap reticle currently
     * owns the viewfinder.
     */
    @Test
    fun confirmationSurvivesTheTapReticle() {
        assertTrue(state(status = TargetStatus.Settling).focusConfirmed())
        assertTrue(state(faces = listOf(face)).focusConfirmed())
        assertFalse(state(mode = FocusMode.Mf).focusConfirmed())
    }
}
