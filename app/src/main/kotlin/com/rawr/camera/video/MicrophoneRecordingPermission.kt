package com.rawr.camera.video

import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.LifecycleOwner

/**
 * One outstanding microphone prompt bound to the ticket created before launch. A grant continues that ticket
 * once at RESUMED; STOP/DESTROY cancel it, and an old result can never belong to a newer request.
 */
internal class MicrophoneRecordingPermission(
    private val lifecycle: Lifecycle,
    private val microphoneGranted: () -> Boolean,
    private val launch: () -> Unit,
    private val continueRequest: (RecordingCoordinator.PermissionTicket) -> Unit
) : LifecycleEventObserver {
    private var outstanding: RecordingCoordinator.PermissionTicket? = null
    private var granted: RecordingCoordinator.PermissionTicket? = null

    init {
        lifecycle.addObserver(this)
    }

    fun request(ticket: RecordingCoordinator.PermissionTicket) {
        if (outstanding != null) {
            ticket.cancel()
            return
        }
        outstanding = ticket
        launch()
    }

    fun onResult(granted: Boolean) {
        val ticket = outstanding ?: return
        outstanding = null
        if (granted && !ticket.cancelled) this.granted = ticket
        continueIfResumed()
    }

    override fun onStateChanged(source: LifecycleOwner, event: Lifecycle.Event) {
        when (event) {
            Lifecycle.Event.ON_RESUME -> continueIfResumed()
            Lifecycle.Event.ON_STOP -> cancel()
            Lifecycle.Event.ON_DESTROY -> {
                cancel()
                lifecycle.removeObserver(this)
            }
            else -> Unit
        }
    }

    // The outstanding launch stays bound to its cancelled ticket so its result is consumed, not reused.
    private fun cancel() {
        outstanding?.cancel()
        granted?.cancel()
        granted = null
    }

    private fun continueIfResumed() {
        val ticket = granted ?: return
        if (!lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) return
        granted = null
        if (!ticket.cancelled && microphoneGranted()) continueRequest(ticket)
    }
}
