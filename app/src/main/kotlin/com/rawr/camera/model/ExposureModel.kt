package com.rawr.camera.model

import kotlin.math.roundToInt

fun CaptureUiState.exposureOwners(): List<ExposureParameter> = when (exposureControl.mode) {
    ExposureMode.Auto -> listOf(ExposureParameter.Ev)
    ExposureMode.Manual -> listOf(ExposureParameter.Shutter, ExposureParameter.Iso)
    ExposureMode.ShutterPriority -> listOf(ExposureParameter.Shutter, ExposureParameter.Ev)
    ExposureMode.IsoPriority -> listOf(ExposureParameter.Iso, ExposureParameter.Ev)
}

fun CameraCapabilities.capabilityFor(parameter: ExposureParameter): DiscreteExposureCapability = when (parameter) {
    ExposureParameter.Shutter -> shutter
    ExposureParameter.Iso -> iso
    ExposureParameter.Ev -> ev
}

fun CaptureUiState.requestedCandidateIdFor(parameter: ExposureParameter): String = when (parameter) {
    ExposureParameter.Shutter -> exposureControl.requestedShutterId
    ExposureParameter.Iso -> exposureControl.requestedIsoId
    ExposureParameter.Ev -> exposureControl.requestedEvId
}

fun CaptureUiState.requestedCandidateIndexFor(parameter: ExposureParameter): Int {
    val capability = capabilities.capabilityFor(parameter)
    val index = capability.indexOf(requestedCandidateIdFor(parameter))
    check(index >= 0) { "Requested ${parameter.name} candidate is not present in current capabilities" }
    return index
}

val ExposureParameter.displayLabel: String
    get() =
        when (this) {
            ExposureParameter.Shutter -> "SHUTTER"
            ExposureParameter.Iso -> "ISO"
            ExposureParameter.Ev -> "EV"
        }

data class ExposureLabels(val previousAnchor: String, val current: String, val nextAnchor: String)

/**
 * Resolves the EV candidate representing exactly +0.0 EV.
 *
 * Candidate ids are opaque to Compose, but the native projection uses `ev:<steps>`
 * with `ev:0` for zero. Fall back to the `+0.0` display label, then to the
 * capability initial as a last resort for ranges excluding zero.
 */
fun DiscreteExposureCapability.zeroEvCandidateId(): String {
    candidates.firstOrNull { it.id == "ev:0" }?.let { return it.id }
    val zeroLabel = ExposureFormat.formatEv(0.0)
    candidates.firstOrNull { it.displayLabel == zeroLabel }?.let { return it.id }
    return initialCandidateId
}

/**
 * Converts a live lens position in diopters to the normalized MF coordinate.
 * Inverse of the native normalized->diopters mapping
 * (`diopters = (1 - normalized) * minimumFocusDistance`).
 * Returns null when the minimum focus distance is unknown (fixed-focus).
 */
fun mfNormalizedFromDiopters(diopters: Float, minimumFocusDistance: Float): Float? {
    if (minimumFocusDistance <= 0f) return null
    return (1f - diopters / minimumFocusDistance).coerceIn(0f, 1f)
}

fun DiscreteExposureCapability.labelsAround(candidateId: String): ExposureLabels {
    val currentIndex = indexOf(candidateId)
    require(currentIndex >= 0) { "Unknown exposure candidate id: $candidateId" }

    val previous =
        (currentIndex - 1 downTo 0)
            .firstOrNull { candidates[it].id in fullStopAnchorIds }
    val next =
        (currentIndex + 1 until candidates.size)
            .firstOrNull { candidates[it].id in fullStopAnchorIds }

    return ExposureLabels(
        previousAnchor = previous?.let { candidates[it].displayLabel }.orEmpty(),
        current = candidates[currentIndex].displayLabel,
        nextAnchor = next?.let { candidates[it].displayLabel }.orEmpty()
    )
}

/**
 * Compact focus value. In MF it is the *requested* distance (exact ∞ at the
 * top of the scale) so the readout never lags or under-reports the anchor;
 * in AF/AF-L it is the lens-reported distance. Untrusted calibration falls
 * back to a percentage of the range.
 */
fun CaptureUiState.focusDistanceLabel(): String {
    val capability = capabilities.manualFocus
    val minimum = capability.minimumFocusDistance
    if (focus.mode == FocusMode.Mf) {
        if (focus.mfNormalized >= 1f) return "∞"
        if (!capability.distanceReadoutTrustworthy || !(minimum > 0f)) {
            return "${(focus.mfNormalized * 100).roundToInt()}%"
        }
        return ExposureFormat.formatFocusDistance((1f - focus.mfNormalized) * minimum)
    }
    val applied = focus.appliedFocusDiopters?.takeIf { it.isFinite() } ?: return "AUTO"
    if (!capability.distanceReadoutTrustworthy) {
        return mfNormalizedFromDiopters(applied, minimum)?.let { "${(it * 100).roundToInt()}%" } ?: "AUTO"
    }
    return ExposureFormat.formatFocusDistance(applied)
}
