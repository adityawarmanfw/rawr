package com.rawr.camera.ui

/** Index and carried track offset after one compact-scrub drag event. */
internal data class DetentScrubResult(val index: Int, val trackOffsetPx: Float)

/**
 * Shared stepping for the compact scrub buttons. A fixed drag distance moves
 * one stop; leaving a magnetic anchor costs [magneticReleasePx] instead.
 * [onStep] fires for every stop crossed, with the new index and whether it
 * should emit a haptic detent (every stop, or anchors only when magnetic).
 * Pushing past either end rubber-bands at 0.18×.
 */
internal inline fun detentScrub(
    index: Int,
    trackOffsetPx: Float,
    deltaPx: Float,
    lastIndex: Int,
    fineStepPx: Float,
    magneticReleasePx: Float,
    magnetic: Boolean,
    isAnchor: (Int) -> Boolean,
    onStep: (index: Int, detent: Boolean) -> Unit
): DetentScrubResult {
    var current = index
    var residual = trackOffsetPx + deltaPx
    while (current < lastIndex) {
        val threshold = if (isAnchor(current)) magneticReleasePx else fineStepPx
        if (residual < threshold) break
        current += 1
        onStep(current, !magnetic || isAnchor(current))
        residual -= threshold
    }
    while (current > 0) {
        val threshold = if (isAnchor(current)) magneticReleasePx else fineStepPx
        if (residual > -threshold) break
        current -= 1
        onStep(current, !magnetic || isAnchor(current))
        residual += threshold
    }
    val pushingPastMin = current == 0 && residual < 0f
    val pushingPastMax = current == lastIndex && residual > 0f
    return DetentScrubResult(current, if (pushingPastMin || pushingPastMax) residual * .18f else residual)
}
