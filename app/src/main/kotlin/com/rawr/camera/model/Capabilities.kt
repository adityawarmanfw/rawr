package com.rawr.camera.model

data class LensCapability(val id: String, val displayName: String) {
    init {
        require(id.isNotBlank()) { "Lens id must not be blank" }
        require(displayName.isNotBlank()) { "Lens display name must not be blank" }
    }
}

/**
 * One backend-stable exposure choice exposed to the capture screen.
 *
 * [id] is deliberately opaque to Compose. A real application/backend adapter may map it
 * to calibrated sensitivity, shutter, or compensation coordinates without exposing those
 * implementation details to the UI.
 */
data class ExposureCandidate(val id: String, val displayLabel: String) {
    init {
        require(id.isNotBlank()) { "Exposure candidate id must not be blank" }
        require(displayLabel.isNotBlank()) { "Exposure candidate label must not be blank" }
    }
}

data class DiscreteExposureCapability(
    val candidates: List<ExposureCandidate>,
    val fullStopAnchorIds: Set<String>,
    val initialCandidateId: String,
    val magneticSnapAnchorIds: Set<String> = emptySet()
) {
    init {
        require(candidates.isNotEmpty()) { "Exposure capability must contain at least one candidate" }
        val ids = candidates.map(ExposureCandidate::id)
        require(ids.distinct().size == ids.size) { "Exposure candidate ids must be unique" }
        require(initialCandidateId in ids) { "initialCandidateId must reference a candidate" }
        require(fullStopAnchorIds.all(ids::contains)) { "Full-stop anchors must reference candidate ids" }
        require(magneticSnapAnchorIds.all(ids::contains)) { "Magnetic snap anchors must reference candidate ids" }
    }

    fun indexOf(candidateId: String): Int = candidates.indexOfFirst { it.id == candidateId }

    fun candidate(candidateId: String): ExposureCandidate? = candidates.firstOrNull { it.id == candidateId }

    val initialCandidate: ExposureCandidate
        get() = requireNotNull(candidate(initialCandidateId))
}

data class ManualFocusCapability(
    val supported: Boolean,
    val minNormalized: Float = 0f,
    val maxNormalized: Float = 1f,
    val distanceReadoutTrustworthy: Boolean = false,
    /** Camera2 LENS_INFO_MINIMUM_FOCUS_DISTANCE in diopters; 0 when unknown/fixed-focus. */
    val minimumFocusDistance: Float = 0f,
    /** Camera2 LENS_INFO_HYPERFOCAL_DISTANCE in diopters; 0 when unreported. */
    val hyperfocalDistance: Float = 0f
) {
    init {
        require(minNormalized in 0f..1f && maxNormalized in 0f..1f) {
            "Manual-focus normalized range must stay inside [0, 1]"
        }
        require(minNormalized <= maxNormalized) { "Manual-focus range must be ordered" }
    }
}

data class CameraCapabilities(
    val lenses: List<LensCapability>,
    val supportedExposureModes: Set<ExposureMode> = ExposureMode.entries.toSet(),
    val shutter: DiscreteExposureCapability,
    val iso: DiscreteExposureCapability,
    val ev: DiscreteExposureCapability,
    val manualFocus: ManualFocusCapability,
    val tapAfSupported: Boolean,
    val fpsChoices: List<Int>,
    val stabilizationOptions: List<String>,
    /** Advertised HAL AWB presets (never contains ManualTempTint). Always contains Auto. */
    val supportedWhiteBalanceModes: Set<WhiteBalanceMode> = setOf(WhiteBalanceMode.Auto),
    /** True when the HAL advertises COLOR_CORRECTION TRANSFORM_MATRIX. */
    val manualWhiteBalanceSupported: Boolean = false
) {
    init {
        require(lenses.isNotEmpty()) { "A usable camera capability snapshot must contain at least one lens" }
        require(supportedExposureModes.isNotEmpty()) { "At least one exposure mode must be supported" }
        require(lenses.map(LensCapability::id).distinct().size == lenses.size) { "Lens ids must be unique" }
        require(fpsChoices.all { it > 0 }) { "FPS choices must be positive" }
        require(fpsChoices.distinct().size == fpsChoices.size) { "FPS choices must be unique" }
        require(stabilizationOptions.none(String::isBlank)) { "Stabilization option labels must not be blank" }
    }
}
