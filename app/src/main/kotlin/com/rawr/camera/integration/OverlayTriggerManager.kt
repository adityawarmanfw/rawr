package com.rawr.camera.integration

import com.rawr.camera.model.OverlayMode
import com.rawr.camera.model.TargetStatus
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Interaction gates for the monitoring overlays.
 *
 * The user arms overlays in the MONITORING panel; each layer renders only while
 * its trigger fires. FalseColor is manual-only and bypasses the gates. The
 * resulting native layer bitmask (bit0 peaking, bit1 RAW highlights, bit2
 * tonemap shadows, bit3 false color) is pushed only when it changes.
 *
 * All mutable state is guarded by [lock]; timer jobs re-enter it after delays.
 * [pushMask] must be thread-safe (RawPreviewCoordinator.setMonitoringOverlay is).
 */
internal class OverlayTriggerManager(
    private val scope: CoroutineScope,
    private val pushMask: (Int) -> Unit
) {
    companion object {
        const val BIT_PEAK = 1
        const val BIT_RAW = 2
        const val BIT_SHADOW = 4
        const val BIT_FALSE = 8

        /** Gesture streams dispatch per-detent; this long without events counts as release. */
        const val INACTIVITY_MS = 350L

        /** Peaking lingers this long after spot AF settles (green). */
        const val AF_DWELL_MS = 1000L

        /** Failsafe: never leave peaking stuck if AF never reports back. */
        const val AF_FAILSAFE_MS = 5000L
    }

    private val lock = Any()
    private var armed: Set<OverlayMode> = emptySet()
    private var falseColor = false
    private var shadowPulse = false
    private var rawPulse = false
    private var peakManualPulse = false
    private var afPeaking = false
    private var afInFlight = false
    private var autoAfPeaking = false
    private var lastMask = -1

    private var hideShadow: Job? = null
    private var hideRaw: Job? = null
    private var hidePeakManual: Job? = null
    private var afDwell: Job? = null
    private var afFailsafe: Job? = null

    fun setArmed(armed: Set<OverlayMode>) = synchronized(lock) {
        this.armed = armed
        emitLocked()
    }

    fun setFalseColor(active: Boolean) = synchronized(lock) {
        falseColor = active
        emitLocked()
    }

    /** Shadow/Blacks tone scrub event. Visible during the scrub, hides on release. */
    fun pulseShadow() = synchronized(lock) {
        shadowPulse = true
        hideShadow = relaunchLocked(hideShadow, INACTIVITY_MS) {
            shadowPulse = false
            emitLocked()
        }
        emitLocked()
    }

    /** SS/ISO/EV candidate step. Visible during the scrub, hides on release. */
    fun pulseRawHighlight() = synchronized(lock) {
        rawPulse = true
        hideRaw = relaunchLocked(hideRaw, INACTIVITY_MS) {
            rawPulse = false
            emitLocked()
        }
        emitLocked()
    }

    /** MF rail interaction. Visible during the drag, hides on release. */
    fun pulsePeakManual() = synchronized(lock) {
        peakManualPulse = true
        hidePeakManual = relaunchLocked(hidePeakManual, INACTIVITY_MS) {
            peakManualPulse = false
            emitLocked()
        }
        emitLocked()
    }

    /** Spot-AF tap (or AF-lock tap): peaking renders while converging. */
    fun autofocusStart() = synchronized(lock) {
        afInFlight = true
        afPeaking = true
        afDwell?.cancel()
        afDwell = null
        afFailsafe = relaunchLocked(afFailsafe, AF_FAILSAFE_MS) {
            afInFlight = false
            afPeaking = false
            emitLocked()
        }
        emitLocked()
    }

    /**
     * Snapshot AF status. Ignored unless an AF request is in flight (tap-gated)
     * and never in MF (manual path owns peaking there). Settled (green) starts
     * the +1s dwell; Failed (red) hides immediately; Settling keeps rendering.
     */
    fun autofocusStatus(status: TargetStatus, isMf: Boolean) = synchronized(lock) {
        if (!afInFlight || isMf) return
        if (status != TargetStatus.Settled && status != TargetStatus.Failed) return
        afInFlight = false
        afFailsafe?.cancel()
        afFailsafe = null
        afDwell?.cancel()
        if (status == TargetStatus.Settled) {
            afDwell = relaunchLocked(null, AF_DWELL_MS) {
                afPeaking = false
                emitLocked()
            }
        } else {
            afDwell = null
            afPeaking = false
        }
        emitLocked()
    }

    /** Tap-AF cleared back to full auto (retap / auto-dismiss): stop peaking now. */
    fun autofocusCancel() = synchronized(lock) {
        afInFlight = false
        afPeaking = false
        afFailsafe?.cancel()
        afFailsafe = null
        afDwell?.cancel()
        afDwell = null
        emitLocked()
    }
    /**
     * Passive autofocus over a faceless scene asks for no metering region, so
     * peaking is the only thing that shows what the camera is judging sharp on.
     * Unlike [autofocusStart] this is a level, not a gesture: it holds for as
     * long as the camera owns the viewfinder's focus graphic.
     */
    fun setAutoAfPeaking(active: Boolean) = synchronized(lock) {
        if (autoAfPeaking == active) return
        autoAfPeaking = active
        emitLocked()
    }

    private fun relaunchLocked(previous: Job?, delayMs: Long, body: () -> Unit): Job {
        previous?.cancel()
        return scope.launch {
            delay(delayMs)
            synchronized(lock) { body() }
        }
    }

    private fun emitLocked() {
        var mask = 0
        if (OverlayMode.Peaking in armed && (peakManualPulse || afPeaking || autoAfPeaking)) mask = mask or BIT_PEAK
        if (OverlayMode.RawHighlights in armed && rawPulse) mask = mask or BIT_RAW
        if (OverlayMode.TonemapShadows in armed && shadowPulse) mask = mask or BIT_SHADOW
        if (falseColor) mask = mask or BIT_FALSE
        if (mask != lastMask) {
            lastMask = mask
            pushMask(mask)
        }
    }
}
