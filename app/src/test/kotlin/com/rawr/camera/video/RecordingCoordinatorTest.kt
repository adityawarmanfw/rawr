package com.rawr.camera.video

import android.net.Uri
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import com.rawr.camera.architecture.*
import com.rawr.camera.fixtures.CaptureFixtures
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.CaptureMode
import com.rawr.camera.settings.model.SelfTimer
import com.rawr.camera.location.CaptureGps
import kotlin.test.*
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.test.*
import org.json.JSONObject

@OptIn(ExperimentalCoroutinesApi::class)
class RecordingCoordinatorTest {
    private class Controller(timer: SelfTimer = SelfTimer.Off, mode: CaptureMode = CaptureMode.Photo) : CaptureScreenController {
        private val mutable = MutableStateFlow(CaptureUiState(CaptureFixtures.baseline(), selfTimer = timer, captureMode = mode))
        override val state = mutable.asStateFlow()
        fun setMode(mode: CaptureMode) { mutable.value = mutable.value.copy(captureMode = mode) }
        override fun dispatch(action: CaptureAction) {
            if (action is PresentationCaptureAction) mutable.value = CaptureReducer.reduce(mutable.value, action)
        }
    }
    private class Recorder(private val failure: Boolean = false) : VideoRecording {
        private var active = true
        var closes = 0
        override val isRecording: Boolean get() = active
        override val outputUri: Uri? = null
        override fun stats(): JSONObject = error("No statistics tick expected in this test")
        override fun journalSnapshot() = Unit
        override fun close() {
            closes++
            active = false
            if (failure) error("finalize failed")
        }
    }
    private fun TestScope.coordinator(
        controller: CaptureScreenController = Controller(),
        foreground: () -> Any? = { Unit },
        validateAccess: suspend () -> Boolean = { true },
        admitGps: () -> CaptureGps? = { null },
        start: suspend (CaptureGps?) -> VideoRecording
    ) = RecordingCoordinator(
        controller, start, { testScheduler.currentTime * 1_000_000 }, foreground, validateAccess, admitGps,
        dispatcher = StandardTestDispatcher(testScheduler), ioDispatcher = StandardTestDispatcher(testScheduler)
    )
    private fun RecordingCoordinator.release() { closeWhenIdle {} }

    @Test fun permissionValidationCannotStartAfterStopCloseModeCountdownOrBackground() = runTest {
        for (event in listOf("ready", "failed", "stop", "close", "mode", "roundTrip", "countdown", "background")) {
            val controller = Controller(mode = CaptureMode.Video)
            val validation = CompletableDeferred<Boolean>()
            var token: Any? = Any()
            val allowed = mutableListOf<Boolean>()
            val fix = CaptureGps(0.0, 0.0, null, null, 1L)
            val owner = RecordingCoordinator(controller, { allowed += it != null; Recorder() }, { 0L }, { token },
                { validation.await() }, { fix }, dispatcher = UnconfinedTestDispatcher(testScheduler),
                ioDispatcher = StandardTestDispatcher(testScheduler))
            val ticket = requireNotNull(owner.permissionTicket())
            owner.startAfterMicrophonePermission(ticket)
            assertTrue(allowed.isEmpty())
            assertEquals(RecordingPhase.Starting, owner.state.value.phase)
            owner.startAfterMicrophonePermission(ticket)
            when (event) {
                "stop" -> owner.stop()
                "close" -> owner.release()
                "mode" -> controller.setMode(CaptureMode.Photo)
                "roundTrip" -> {
                    controller.dispatch(SetCaptureMode(CaptureMode.Photo))
                    controller.dispatch(SetCaptureMode(CaptureMode.Video))
                }
                "countdown" -> controller.dispatch(SetCaptureSelfTimer(SelfTimer.TwoSeconds))
                "background" -> token = Any()
            }
            runCurrent()
            if (event == "stop" || event == "close") assertEquals(RecordingPhase.Idle, owner.state.value.phase)
            validation.complete(event != "failed")
            runCurrent()
            val expected = when (event) { "ready" -> listOf(true); "failed" -> listOf(false); else -> emptyList() }
            assertEquals(expected, allowed, event)
            owner.release()
            runCurrent()
        }
    }

    @Test fun ordinaryRequestsCannotAdmitAfterLeavingForegroundOrReviveOnReturn() = runTest {
        val controller = Controller(SelfTimer.TwoSeconds, CaptureMode.Video)
        var token: Any? = Any()
        var validations = 0
        var starts = 0
        val owner = coordinator(controller, { token }, { validations++; true }) { starts++; Recorder() }
        owner.toggle()
        advanceTimeBy(1_000)
        token = Any()
        advanceTimeBy(1_500)
        runCurrent()
        assertEquals(0, starts, "A countdown from before Home cannot revive on return")
        owner.toggle()
        advanceTimeBy(2_000)
        runCurrent()
        assertEquals(1, starts)
        assertEquals(0, validations, "Ordinary starts never wait for location validation")
        owner.release()
        runCurrent()
    }

    private class PermissionHost : LifecycleOwner {
        override val lifecycle = LifecycleRegistry.createUnsafe(this)
        fun event(event: Lifecycle.Event) = lifecycle.handleLifecycleEvent(event)
    }

    @Test fun stoppedPermissionRequestCannotReviveOnResume() = runTest {
        val host = PermissionHost()
        host.event(Lifecycle.Event.ON_CREATE)
        host.event(Lifecycle.Event.ON_START)
        var admissions = 0
        val owner = coordinator(Controller(mode = CaptureMode.Video)) { admissions++; Recorder() }
        val permission = MicrophoneRecordingPermission(host.lifecycle, { true }, {}, owner::startAfterMicrophonePermission)
        permission.request(requireNotNull(owner.permissionTicket()))
        host.event(Lifecycle.Event.ON_STOP)
        permission.onResult(true)
        host.event(Lifecycle.Event.ON_START)
        host.event(Lifecycle.Event.ON_RESUME)
        runCurrent()
        assertEquals(0, admissions)
        owner.release()
        host.event(Lifecycle.Event.ON_DESTROY)
        runCurrent()
    }

    @Test fun backgroundStopWaitsForStartingRecorderAndFinalizesExactlyOnce() = runTest {
        val startGate = CompletableDeferred<Unit>()
        val recorder = Recorder()
        val owner = coordinator { startGate.await(); recorder }
        owner.start()
        runCurrent()
        assertEquals(RecordingPhase.Starting, owner.state.value.phase)
        val stopping = owner.stop()
        runCurrent()
        assertFalse(stopping.isCompleted)
        startGate.complete(Unit)
        runCurrent()
        assertTrue(stopping.isCompleted)
        assertEquals(RecordingPhase.Idle, owner.state.value.phase)
        assertEquals(1, recorder.closes)
        owner.stop()
        runCurrent()
        assertEquals(1, recorder.closes)
        owner.release()
        runCurrent()
    }

    @Test fun countdownCanBeCancelledWithoutStarting() = runTest {
        val controller = Controller(SelfTimer.TwoSeconds)
        var starts = 0
        val owner = coordinator(controller) { starts++; Recorder() }
        owner.toggle()
        advanceTimeBy(500)
        runCurrent()
        owner.toggle()
        advanceTimeBy(3_000)
        runCurrent()
        assertEquals(0, starts)
        assertNull(controller.state.value.selfTimerRemainingMs)
        owner.release()
        runCurrent()
    }

    @Test fun staleCountdownCannotStartAfterModeChange() = runTest {
        val controller = Controller(SelfTimer.TwoSeconds)
        var starts = 0
        val owner = coordinator(controller) { starts++; Recorder() }
        owner.toggle()
        advanceTimeBy(500)
        controller.dispatch(CancelSelfTimer)
        advanceTimeBy(2_000)
        runCurrent()
        assertEquals(0, starts)
        owner.release()
        runCurrent()
    }

    @Test fun countdownStartsOnceAtExpiry() = runTest {
        val controller = Controller(SelfTimer.TwoSeconds)
        var starts = 0
        val owner = coordinator(controller) { starts++; Recorder() }
        owner.toggle()
        advanceTimeBy(2_000)
        runCurrent()
        assertEquals(1, starts)
        assertEquals(RecordingPhase.Recording, owner.state.value.phase)
        assertNull(controller.state.value.selfTimerRemainingMs)
        owner.release()
        runCurrent()
    }

    @Test fun gpsAdmissionFreezesOnceBeforeIoAndSurvivesLongSetupAndStop() = runTest {
        val controller = Controller(SelfTimer.TwoSeconds)
        val admitted = CaptureGps(0.0, 0.0, -12.5, 0.0, 1_700_000_000_123)
        var current: CaptureGps? = null
        var reads = 0
        var received: CaptureGps? = null
        val setup = CompletableDeferred<Unit>()
        val recorder = Recorder()
        val owner = coordinator(controller, admitGps = { reads++; current }) { frozen ->
            setup.await()
            received = frozen
            recorder
        }
        owner.toggle()
        advanceTimeBy(1_999)
        assertEquals(0, reads)
        current = admitted
        advanceTimeBy(1)
        runCurrent()
        assertEquals(1, reads)
        current = CaptureGps(20.0, 30.0, 99.0, 12.0, 1_700_000_050_000)
        val stopping = owner.stop()
        advanceTimeBy(60_000)
        setup.complete(Unit)
        runCurrent()
        assertSame(admitted, received)
        assertTrue(stopping.isCompleted)
        assertEquals(1, reads)
        assertEquals(1, recorder.closes)
        owner.release()
        runCurrent()
    }

    @Test fun startFailureClearsBusyAndAllowsRetry() = runTest {
        var starts = 0
        val owner = coordinator { starts++; if (starts == 1) error("start failed") else Recorder() }
        owner.start()
        runCurrent()
        assertEquals(RecordingPhase.Failed, owner.state.value.phase)
        assertFalse(owner.state.value.busy)
        owner.start()
        runCurrent()
        assertEquals(RecordingPhase.Recording, owner.state.value.phase)
        owner.release()
        runCurrent()
    }

    @Test fun closingRetainsFinalizationAndReleasesDependenciesAfterward() = runTest {
        val startGate = CompletableDeferred<Unit>()
        val recorder = Recorder(failure = true)
        val owner = coordinator { startGate.await(); recorder }
        var released = false
        owner.start()
        runCurrent()
        owner.closeWhenIdle { released = true }
        runCurrent()
        assertFalse(released)
        startGate.complete(Unit)
        runCurrent()
        assertEquals(1, recorder.closes)
        assertTrue(released)
        assertEquals(RecordingPhase.Failed, owner.state.value.phase)
        assertFalse(owner.state.value.recording)
    }
}
