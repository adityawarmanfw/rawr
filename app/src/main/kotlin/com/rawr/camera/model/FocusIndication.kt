package com.rawr.camera.model

/**
 * Whether the camera's own autofocus owns the viewfinder's focus graphic.
 *
 * Face outlines and the tap reticle win whenever they exist, and manual focus has
 * no camera-chosen target to show. This is the remaining case, which had no
 * representation at all: passive continuous AF over a faceless scene requests no
 * metering region, so the photographer had no way to see what the camera was
 * judging sharp on.
 *
 * Deliberately built only from Camera2 result state, never from a vendor tag, so
 * it behaves the same on a HAL that publishes no extras.
 */
fun CaptureUiState.focusTracked(): Boolean {
    if (focus.mode == FocusMode.Mf) return false
    if (faceDetections.isNotEmpty()) return false
    // An outstanding tap-AF request already draws the better indicator.
    return focus.status == TargetStatus.Hidden
}

/**
 * The camera reports that it has found focus, by its own result rather than by an
 * app gesture.
 *
 * Narrower than [focusTracked] on purpose: the camera also reports a state while the
 * lens is still scanning, and claiming success then would be a lie. It is not gated
 * on [focusTracked], so the level meter can still report a lock that faces or a tap
 * achieved.
 */
fun CaptureUiState.focusConfirmed(): Boolean =
    focus.mode != FocusMode.Mf && focus.cameraAfStatus == TargetStatus.Settled
