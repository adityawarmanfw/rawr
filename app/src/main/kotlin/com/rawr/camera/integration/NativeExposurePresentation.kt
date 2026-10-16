package com.rawr.camera.integration

import com.rawr.camera.model.*

/** Applied readouts and optimistic held rail presentation from native state. */
internal object NativeExposurePresentation {
    fun project(
        current: CaptureUiState,
        snapshot: NativeCameraUiSnapshot,
        projection: NativeExposureProjection,
        videoFpsChanged: Boolean
    ): CaptureUiState {
        // Applied/telemetry readouts stay in shutter speeds (1/N) in every
        // mode, including Video: the monitor mirrors the actual exposure.
        // The shutter control resolves through the candidate labels, so
        // it shows degrees in Video (see CompactExposureButton).
        val applied =
            ExposureAppliedState(
                shutter =
                    snapshot.appliedExposureTimeNs?.let {
                        AppliedExposureReadout(
                            ExposureFormat.formatShutterNs(it),
                            projection.shutterValuesNs
                                .minByOrNull { (_, v) ->
                                    kotlin.math.abs(v - it)
                                }?.key
                        )
                    },
                iso =
                    snapshot.appliedSensitivity?.let {
                        AppliedExposureReadout(
                            it.toString(),
                            projection.isoValues
                                .minByOrNull { (_, v) ->
                                    kotlin.math.abs(v - it)
                                }?.key
                        )
                    },
                evOrMeter =
                    if (snapshot.exposureMode == 0) {
                        snapshot.appliedEvSteps?.let { steps ->
                            AppliedExposureReadout(
                                ExposureFormat.formatEv(steps * snapshot.evStep),
                                projection.evSteps.entries
                                    .firstOrNull {
                                        it.value ==
                                            steps
                                    }?.key
                            )
                        }
                    } else {
                        null
                    }
            )
        // S/I own one axis in application/Camera2 state. The native snapshot is
        // necessarily one or more frames behind a just-selected held value, so
        // feeding that stale Camera2 request back into Compose makes the rail
        // visibly jump/revert and can make priority control appear nonfunctional.
        // Keep held request selections stable while gestures await the
        // backend. Applied readouts remain CaptureResult truth above.
        val requestedShutterId =
            if (current.exposureControl.mode == ExposureMode.ShutterPriority ||
                (current.exposureControl.mode == ExposureMode.Manual && snapshot.semanticExposureMode in 1..2) ||
                videoFpsChanged) {
                current.exposureControl.requestedShutterId
            } else if (snapshot.requestedShutterAngleDegrees != null && snapshot.semanticExposureMode in 1..2) {
                projection.shutterAnglesDeg.entries.firstOrNull { it.value == snapshot.requestedShutterAngleDegrees }?.key
                    ?: current.exposureControl.requestedShutterId
            } else {
                projection.shutterValuesNs.minByOrNull { (_, v) -> kotlin.math.abs(v - snapshot.requestedExposureTimeNs) }?.key
                    ?: current.exposureControl.requestedShutterId
            }
        val requestedIsoId =
            if (current.exposureControl.mode == ExposureMode.IsoPriority) {
                current.exposureControl.requestedIsoId
            } else {
                projection.isoValues.minByOrNull { (_, v) -> kotlin.math.abs(v - snapshot.requestedSensitivity) }?.key
                    ?: current.exposureControl.requestedIsoId
            }
        val requestedEvId =
            projection.evSteps.entries
                .firstOrNull { it.value == snapshot.requestedEvSteps }
                ?.key
                ?: current.exposureControl.requestedEvId
        return current.copy(
            exposureApplied = applied,
            spotAe = if (snapshot.semanticExposureMode != 0) current.spotAe.copy(active = false) else current.spotAe,
            sensitivityBoost = snapshot.appliedPostRawSensitivityBoost,
            longExposure = LongExposureUiState(
                deferredNs = snapshot.deferredCaptureExposureNs,
                activeNs = snapshot.longExposureNs,
                startMs = snapshot.longExposureStartMs,
                lastHonored = snapshot.lastLongExposureHonored
            ),
            rawFps = snapshot.appliedRawFps,
            viewfinderFps = snapshot.measuredViewfinderFps,
            exposureControl = current.exposureControl.copy(
                mode = ExposureMode.entries.getOrElse(snapshot.semanticExposureMode) { ExposureMode.Auto },
                requestedShutterId = requestedShutterId,
                requestedIsoId = requestedIsoId,
                requestedEvId = requestedEvId
            )
        )
    }
}
