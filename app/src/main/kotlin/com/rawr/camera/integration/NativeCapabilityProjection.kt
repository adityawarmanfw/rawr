package com.rawr.camera.integration

import com.rawr.camera.model.*
import kotlin.math.abs
import kotlin.math.log2
import kotlin.math.pow
import kotlin.math.roundToInt

internal data class NativeExposureProjection(
    val capabilities: CameraCapabilities,
    val isoValues: Map<String, Int>,
    val shutterValuesNs: Map<String, Long>,
    val evSteps: Map<String, Int>,
    val shutterAnglesDeg: Map<String, Double>
)

internal object NativeCapabilityProjection {
    // Native owns the lens table (its camera profile); until the first snapshot
    // arrives only the default lens is known.
    private val pendingLenses = listOf(LensCapability("main", "—"))

    /** Placeholder until the next snapshot; a lens switch keeps the known [lenses]. */
    fun pending(lenses: List<LensCapability> = pendingLenses): CameraCapabilities {
        val p = DiscreteExposureCapability(listOf(ExposureCandidate("pending", "—")), setOf("pending"), "pending")
        return CameraCapabilities(
            lenses = lenses.ifEmpty { pendingLenses },
            supportedExposureModes = ExposureMode.entries.toSet(),
            shutter = p,
            iso = p,
            ev = p,
            manualFocus = ManualFocusCapability(false),
            tapAfSupported = false,
            fpsChoices = emptyList(),
            stabilizationOptions = emptyList(),
            supportedWhiteBalanceModes = setOf(WhiteBalanceMode.Auto),
            manualWhiteBalanceSupported = false
        )
    }

    fun project(s: NativeCameraUiSnapshot, videoFps: Int? = null): NativeExposureProjection {
        val effectiveIsoMin = s.sensitivityMin
        val effectiveIsoMax = s.sensitivityMax
        val isoValues = fineIsoValues(effectiveIsoMin, effectiveIsoMax)
        val evValues = (s.evMinSteps..s.evMaxSteps).toList().ifEmpty { listOf(0) }

        val isoCandidates = isoValues.map { ExposureCandidate("iso:$it", it.toString()) }
        val evCandidates = evValues.map { ExposureCandidate("ev:$it", ExposureFormat.formatEv(it * s.evStep)) }

        val isoInitial = nearestInt(isoValues, s.requestedSensitivity.takeIf { it > 0 } ?: s.appliedSensitivity ?: 100)

        // Native supplies legal angles and their resolved request values.
        // Kotlin formats choices and maps displayed IDs to semantic commands.
        val videoChoices = if (videoFps != null) s.shutterAngleChoices.takeIf { it.isNotEmpty() } else null
        val videoAngles = videoChoices?.map { it.degrees }
        val shutterValues: List<Long>
        val ssCandidates: List<ExposureCandidate>
        val ssInitial: Long
        val fullStopIds: Set<String>
        val magneticIds: Set<String>
        val initialId: String
        if (videoFps != null && videoAngles == null) {
            // No legal native angle choices: do not expose photo speeds as
            // recording commands while capabilities are pending/unavailable.
            shutterValues = emptyList()
            ssCandidates = listOf(ExposureCandidate("pending", "—"))
            ssInitial = 0L
            fullStopIds = emptySet()
            magneticIds = emptySet()
            initialId = "pending"
        } else if (videoAngles != null) {
            val nsForAngle = videoChoices!!.associate { it.degrees to it.exposureTimeNs }
            fun angleId(angle: Double): String = "ssa:${angleLabel(angle)}"
            ssCandidates = videoAngles.map { ExposureCandidate(angleId(it), ExposureFormat.formatShutterDegree(it)) }
            shutterValues = videoAngles.map { nsForAngle.getValue(it) }
            val targetNs =
                s.requestedExposureTimeNs.takeIf { it > 0 } ?: s.appliedExposureTimeNs
                    ?: nsForAngle[180.0] ?: shutterValues.first()
            ssInitial = nearestLong(shutterValues, targetNs)
            // Default to 180° when there is no live request yet (initial launch):
            // requestedExposureTimeNs is 0 until the first native request echo.
            val defaultInitial =
                if (s.requestedExposureTimeNs > 0 || s.appliedExposureTimeNs != null) ssInitial
                else nsForAngle[180.0] ?: ssInitial
            val ids = ssCandidates.map { it.id }
            fullStopIds = ids.toSet()
            magneticIds = ids.toSet()
            initialId = s.requestedShutterAngleDegrees?.takeIf { it in videoAngles }?.let(::angleId)
                ?: angleId(videoAngles.minBy { kotlin.math.abs(nsForAngle.getValue(it) - defaultInitial) })
        } else {
            val photoValues = fineShutterValues(s.exposureTimeMinNs, s.exposureTimeMaxNs)
            shutterValues = photoValues
            ssCandidates =
                photoValues.map { ExposureCandidate("ss:$it", ExposureFormat.formatShutterNs(it)) }
            ssInitial =
                nearestLong(
                    photoValues,
                    s.requestedExposureTimeNs.takeIf { it > 0 } ?: s.appliedExposureTimeNs ?: 10_000_000L
                )
            fun <T> candidateId(prefix: String, value: T) = "$prefix:$value"
            fullStopIds =
                photoValues.filter { isFullStopShutter(it) }.mapTo(mutableSetOf()) { candidateId("ss", it) }
            magneticIds =
                photoValues.filter { isThirdStopShutter(it) }.mapTo(mutableSetOf()) { candidateId("ss", it) }
            initialId = candidateId("ss", ssInitial)
        }
        val evInitial = nearestInt(evValues, s.requestedEvSteps)

        fun <T> candidateId(prefix: String, value: T) = "$prefix:$value"
        // The selector itself always renders Auto/Manual/ShutterPriority/IsoPriority. Only modes actually
        // advertised by this camera are interactive; otherwise a tap must not
        // create optimistic UI state that disagrees with the native owner.
        val supportedModes =
            buildSet {
                add(ExposureMode.Auto)
                if (s.manualExposureSupported) add(ExposureMode.Manual)
                if (s.shutterPrioritySupported) add(ExposureMode.ShutterPriority)
                if (s.isoPrioritySupported) add(ExposureMode.IsoPriority)
            }
        // Native publishes raw NDK AWB values; unknown values are dropped and
        // Auto is always present so the UI never strands the user modeless.
        val supportedWbModes =
            s.supportedAwbModes
                .mapNotNull { raw -> WhiteBalanceMode.entries.firstOrNull { it.awbValue == raw } }
                .filter { it != WhiteBalanceMode.ManualTempTint }
                .toMutableSet()
                .also { it.add(WhiteBalanceMode.Auto) }
        val caps =
            CameraCapabilities(
                lenses = s.profileLenses.map { (id, label) -> LensCapability(id, label.ifBlank { id }) }.ifEmpty { pendingLenses },
                supportedExposureModes = supportedModes,
                shutter =
                    DiscreteExposureCapability(
                        ssCandidates,
                        fullStopIds,
                        initialId,
                        magneticIds
                    ),
                iso =
                    DiscreteExposureCapability(
                        isoCandidates,
                        isoValues.filter { isFullStopIso(it) }.mapTo(mutableSetOf()) { candidateId("iso", it) },
                        candidateId("iso", isoInitial),
                        isoValues.filter { isThirdStopIso(it) }.mapTo(mutableSetOf()) { candidateId("iso", it) }
                    ),
                ev =
                    DiscreteExposureCapability(
                        evCandidates,
                        evValues.filter { isWholeEv(it * s.evStep) }.mapTo(mutableSetOf()) { candidateId("ev", it) },
                        candidateId("ev", evInitial)
                    ),
                manualFocus =
                    ManualFocusCapability(
                        supported = s.manualFocusSupported,
                        distanceReadoutTrustworthy = s.manualFocusSupported && s.focusDistanceReadoutTrustworthy,
                        minimumFocusDistance = s.minimumFocusDistance,
                        hyperfocalDistance = s.hyperfocalDistance
                    ),
                tapAfSupported = s.tapAfSupported,
                fpsChoices = emptyList(),
                stabilizationOptions = emptyList(),
                supportedWhiteBalanceModes = supportedWbModes,
                manualWhiteBalanceSupported = s.manualWhiteBalanceSupported
            )
        return NativeExposureProjection(
            caps,
            isoValues.associate { candidateId("iso", it) to it },
            ssCandidates.map { it.id }.zip(shutterValues).toMap(),
            evValues.associate { candidateId("ev", it) to it },
            videoChoices.orEmpty().associate { "ssa:${angleLabel(it.degrees)}" to it.degrees }
        )
    }

    // Shutter/ISO are exposed on a dense 1/24-EV grid. This is fine enough to feel
    // continuous while still giving the backend stable integer request values. The UI
    // magnetically snaps to the conventional 1/3-stop subset. Exact Camera2 endpoints
    // are retained even when they do not lie on that grid.
    private fun fineIsoValues(min: Int, max: Int): List<Int> {
        val values = mutableSetOf(min, max)
        for (k in -192..288) {
            val value = (100.0 * 2.0.pow(k / 24.0)).roundToInt()
            if (value in min..max) values += value
        }
        return values.sorted()
    }

    private fun fineShutterValues(min: Long, max: Long): List<Long> {
        val values = mutableSetOf(min, max)
        for (k in -432..192) {
            val ns = (1_000_000_000.0 * 2.0.pow(k / 24.0)).toLong()
            if (ns in min..max) values += ns
        }
        return values.sorted()
    }

    private fun isWholeEv(ev: Double): Boolean = abs(ev - ev.roundToInt()) < 0.02

    private fun isFullStopIso(iso: Int): Boolean = isNearEvGrid(log2(iso / 100.0), 1.0)

    private fun isThirdStopIso(iso: Int): Boolean = isNearEvGrid(log2(iso / 100.0), 1.0 / 3.0)

    private fun isFullStopShutter(ns: Long): Boolean = isNearEvGrid(log2(ns / 1_000_000_000.0), 1.0)

    private fun isThirdStopShutter(ns: Long): Boolean = isNearEvGrid(log2(ns / 1_000_000_000.0), 1.0 / 3.0)

    private fun isNearEvGrid(ev: Double, step: Double): Boolean {
        val grid = (ev / step).roundToInt() * step
        return abs(ev - grid) < 0.012
    }

    private fun nearestInt(values: List<Int>, target: Int): Int = values.minBy { abs(it.toLong() - target.toLong()) }

    private fun nearestLong(values: List<Long>, target: Long): Long = values.minBy { abs(it - target) }

    private fun angleLabel(angle: Double): String =
        if (angle == kotlin.math.floor(angle)) angle.toInt().toString() else angle.toString()
}
