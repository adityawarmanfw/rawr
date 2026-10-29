package com.rawr.camera.video

import com.rawr.camera.architecture.CancelSelfTimer
import com.rawr.camera.architecture.CaptureScreenController
import com.rawr.camera.architecture.StartSelfTimerCountdown
import com.rawr.camera.architecture.TickSelfTimer
import com.rawr.camera.location.CaptureGps
import com.rawr.camera.model.VideoTimingState
import com.rawr.camera.model.CaptureMode
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

enum class RecordingPhase { Idle, Starting, Recording, Finalizing, Failed }

data class RecordingState(
    val phase: RecordingPhase = RecordingPhase.Idle,
    val status: String = "REC",
    val timing: VideoTimingState? = null
) {
    val recording: Boolean get() = phase == RecordingPhase.Recording || phase == RecordingPhase.Finalizing
    val busy: Boolean get() = phase == RecordingPhase.Starting || phase == RecordingPhase.Finalizing
}

/** Owns every recording job, including finalization after the screen or ViewModel leaves. */
class RecordingCoordinator(
    private val controller: CaptureScreenController,
    private val startRecorder: suspend (CaptureGps?) -> VideoRecording,
    private val clockNs: () -> Long,
    private val foregroundToken: () -> Any?,
    private val validateAccess: suspend () -> Boolean,
    private val admitGps: () -> CaptureGps?,
    private val onSaved: suspend (VideoRecording) -> Unit = {},
    dispatcher: CoroutineDispatcher = Dispatchers.Main.immediate,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO
) {
    /** Created before the microphone prompt; a cancelled ticket can never start or be rebound. */
    class PermissionTicket internal constructor(internal val runId: Long) {
        @Volatile internal var cancelled = false
            private set
        internal fun cancel() { cancelled = true }
    }
    private class Request(val token: Any, val runId: Long, val permissionOrigin: Boolean)

    private val scope = CoroutineScope(SupervisorJob() + dispatcher)
    private val mutableState = MutableStateFlow(RecordingState())
    val state = mutableState.asStateFlow()
    private val mutableErrors = Channel<String>(Channel.BUFFERED)
    val errors = mutableErrors.receiveAsFlow()
    private var recorder: VideoRecording? = null
    private var startJob: Job? = null
    private var stopJob: Job? = null
    private var timerJob: Job? = null
    private var statsJob: Job? = null
    private var closing = false
    private var admitted = false
    private var ticket: PermissionTicket? = null

    /** Binds a prompt to the current video mode and timer run; replaces any earlier ticket. */
    fun permissionTicket(): PermissionTicket? {
        val capture = controller.state.value
        if (closing || state.value.busy || state.value.recording || capture.captureMode != CaptureMode.Video ||
            capture.selfTimerRemainingMs != null) return null
        ticket?.cancel()
        return PermissionTicket(capture.selfTimerRunId).also { ticket = it }
    }

    /** Continues the ticket's request once, under the foreground identity current at RESUMED. */
    fun startAfterMicrophonePermission(ticket: PermissionTicket) {
        val capture = controller.state.value
        if (ticket.cancelled || this.ticket !== ticket) return
        this.ticket = null
        ticket.cancel()
        if (capture.captureMode != CaptureMode.Video || capture.selfTimerRunId != ticket.runId ||
            state.value.recording || capture.selfTimerRemainingMs != null) return
        begin(foregroundToken() ?: return, permissionOrigin = true)
    }

    fun toggle() {
        if (closing || state.value.busy) return
        if (state.value.recording) { stop(); return }
        if (controller.state.value.selfTimerRemainingMs != null) { cancelCountdown(); return }
        begin(foregroundToken() ?: return, permissionOrigin = false)
    }

    private fun begin(token: Any, permissionOrigin: Boolean) {
        if (closing || state.value.busy) return
        if (controller.state.value.selfTimer.seconds <= 0) {
            start(Request(token, controller.state.value.selfTimerRunId, permissionOrigin))
            return
        }
        controller.dispatch(StartSelfTimerCountdown)
        val request = Request(token, controller.state.value.selfTimerRunId, permissionOrigin)
        if (controller.state.value.selfTimerRemainingMs == null) { start(request); return }
        timerJob = scope.launch {
            while (true) {
                delay(100)
                val capture = controller.state.value
                val remaining = capture.selfTimerRemainingMs ?: return@launch
                if (capture.selfTimerRunId != request.runId) return@launch
                val next = remaining - 100
                controller.dispatch(TickSelfTimer(next, request.runId))
                if (next <= 0) break
            }
            timerJob = null
            if (controller.state.value.selfTimerRunId == request.runId) start(request)
        }
    }

    fun start(): Job? {
        val token = foregroundToken() ?: return startJob
        return start(Request(token, controller.state.value.selfTimerRunId, permissionOrigin = false))
    }

    private fun start(request: Request): Job? {
        if (closing || state.value.busy || recorder != null) return startJob
        val requestedMode = controller.state.value.captureMode
        admitted = false
        mutableState.value = RecordingState(RecordingPhase.Starting, "Starting…")
        startJob = scope.launch {
            try {
                // Only a microphone-permission continuation waits, and only for its access check.
                val gpsAllowed = !request.permissionOrigin || validateAccess()
                val capture = controller.state.value
                if (closing || foregroundToken() !== request.token || capture.captureMode != requestedMode ||
                    capture.selfTimerRunId != request.runId) {
                    mutableState.value = RecordingState()
                    return@launch
                }
                admitted = true
                recorder = startRecorder(if (gpsAllowed) admitGps() else null)
                val active = requireNotNull(recorder)
                val startedAt = clockNs()
                mutableState.value = RecordingState(RecordingPhase.Recording, "REC 00:00")
                statsJob = scope.launch {
                    while (active.isRecording) {
                        delay(1_000)
                        val stats = withContext(ioDispatcher) { active.journalSnapshot(); active.stats() }
                        val seconds = ((clockNs() - startedAt) / 1_000_000_000L).coerceAtLeast(0)
                        mutableState.value = RecordingState(
                            RecordingPhase.Recording,
                            "REC %02d:%02d".format(seconds / 60, seconds % 60),
                            VideoTimingState(
                                actualFps = stats.optDouble("currentFps"),
                                dropped = stats.optLong("dropped"),
                                shortfall = stats.optLong("targetFrameShortfall"),
                                gpuMs = stats.optDouble("videoGpuMs", stats.optDouble("gpuMs")),
                                encoderMs = stats.optDouble("encoderLatencyMs"),
                                micDbfs = stats.optDouble("audioRmsDbfs", -120.0),
                                dropReason = stats.optString("dropReason").takeIf { it.isNotEmpty() },
                                gpuPeakMs = stats.optDouble("videoGpuPeakMs", 0.0),
                                encoderPeakMs = stats.optDouble("encoderLatencyPeakMs", 0.0),
                                iFrames = stats.optLong("iFrames"),
                                pFrames = stats.optLong("pFrames"),
                                bFrames = stats.optLong("bFrames"),
                                actualGop = stats.optLong("actualGop")
                            )
                        )
                        if (stats.optString("failure").let { it.isNotEmpty() && it != "null" }) {
                            stop()
                            return@launch
                        }
                    }
                }
            } catch (e: kotlinx.coroutines.CancellationException) {
                throw e
            } catch (e: Exception) {
                mutableState.value = RecordingState(RecordingPhase.Failed, "ERROR")
                mutableErrors.send(e.message ?: "Video mode could not start")
            }
        }
        return startJob
    }

    private fun cancelCountdown() {
        timerJob?.cancel()
        timerJob = null
        if (controller.state.value.selfTimerRemainingMs != null) controller.dispatch(CancelSelfTimer)
    }

    /** Joins an in-flight start before finalizing; callers can safely release preview afterward. */
    fun stop(): Job {
        ticket?.cancel()
        ticket = null
        cancelCountdown()
        if (state.value.phase == RecordingPhase.Starting && !admitted) {
            startJob?.cancel()
            mutableState.value = RecordingState()
        }
        stopJob?.takeIf { it.isActive }?.let { return it }
        return scope.launch {
            startJob?.join()
            statsJob?.cancel()
            statsJob = null
            val active = recorder ?: return@launch
            mutableState.value = RecordingState(RecordingPhase.Finalizing, "Saving…")
            try {
                withContext(ioDispatcher) { active.close() }
                onSaved(active)
                mutableState.value = RecordingState()
            } catch (e: kotlinx.coroutines.CancellationException) {
                throw e
            } catch (e: Exception) {
                mutableState.value = RecordingState(RecordingPhase.Failed, "ERROR")
                mutableErrors.send(e.message ?: "Video finalization failed")
            } finally {
                recorder = null
            }
        }.also { stopJob = it }
    }

    fun closeWhenIdle(onClosed: () -> Unit) {
        closing = true
        val stopping = stop()
        scope.launch {
            stopping.join()
            try { onClosed() } finally { scope.cancel() }
        }
    }
}
