package com.rawr.camera.integration

import com.rawr.camera.architecture.*
import com.rawr.camera.model.CaptureMode
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.settings.model.SelfTimer
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.test.*
import kotlin.test.*

@OptIn(ExperimentalCoroutinesApi::class)
class CaptureSelfTimerTest {
    private class Fixture(scope: TestScope, preference: SelfTimer = SelfTimer.ThreeSeconds) {
        val state = MutableStateFlow(CaptureUiState(NativeCapabilityProjection.pending(), selfTimer = preference))
        var captures = 0
        var foreground: Any? = Any()
        val expiredTokens = mutableListOf<Any>()
        val timer = CaptureSelfTimer(scope.backgroundScope, state, { state.update(it) },
            { expiredTokens += it; captures++ }, { foreground })

        fun dispatch(action: PresentationCaptureAction) {
            state.update { CaptureReducer.reduce(it, action) }
            timer.reconcile()
        }
    }

    @Test
    fun offCapturesImmediatelyAndCountdownExpiresOnce() = runTest {
        val f = Fixture(this, SelfTimer.Off)
        f.timer.trigger()
        assertEquals(1, f.captures)
        f.dispatch(SetCaptureSelfTimer(SelfTimer.ThreeSeconds))
        f.timer.trigger()
        runCurrent()
        assertEquals(3_000L, f.state.value.selfTimerRemainingMs)
        advanceTimeBy(2_999)
        runCurrent()
        assertEquals(1, f.captures)
        assertEquals(100L, f.state.value.selfTimerRemainingMs)
        advanceTimeBy(1)
        runCurrent()
        assertNull(f.state.value.selfTimerRemainingMs)
        assertEquals(2, f.captures)
        advanceTimeBy(5_000)
        runCurrent()
        assertEquals(2, f.captures)
    }

    @Test
    fun secondPressCancelsAndNewRunIgnoresOldDeadline() = runTest {
        val f = Fixture(this)
        f.timer.trigger()
        runCurrent()
        advanceTimeBy(2_900)
        runCurrent()
        val oldRun = f.state.value.selfTimerRunId
        f.timer.trigger()
        assertNull(f.state.value.selfTimerRemainingMs)
        f.timer.trigger()
        runCurrent()
        assertTrue(f.state.value.selfTimerRunId > oldRun)
        advanceTimeBy(100)
        runCurrent()
        assertEquals(0, f.captures)
        assertEquals(2_900L, f.state.value.selfTimerRemainingMs)
        advanceTimeBy(2_900)
        runCurrent()
        assertEquals(1, f.captures)
    }

    @Test
    fun reselectingCurrentPreferenceAndModeDoesNotStrandCountdown() = runTest {
        val f = Fixture(this)
        f.timer.trigger()
        runCurrent()
        advanceTimeBy(1_000)
        runCurrent()
        f.dispatch(SetCaptureSelfTimer(SelfTimer.ThreeSeconds))
        f.dispatch(SetCaptureMode(CaptureMode.Photo))
        advanceTimeBy(2_000)
        runCurrent()
        assertEquals(1, f.captures)
        assertNull(f.state.value.selfTimerRemainingMs)
    }

    @Test
    fun cancellationActionsAndModeChangesNeverExpire() = runTest {
        val actions = listOf(CancelSelfTimer, CycleSelfTimer, SetCaptureSelfTimer(SelfTimer.FiveSeconds),
            SetCaptureMode(CaptureMode.Video))
        actions.forEach { action ->
            val f = Fixture(this)
            f.timer.trigger()
            runCurrent()
            f.dispatch(action)
            advanceTimeBy(5_000)
            runCurrent()
            assertNull(f.state.value.selfTimerRemainingMs)
            assertEquals(0, f.captures, action.toString())
        }
    }

    @Test
    fun lensOrContextInvalidationCancelsJobAndDisplayedRun() = runTest {
        val f = Fixture(this)
        f.timer.trigger()
        runCurrent()
        f.timer.cancel() // Lens selection.
        f.timer.trigger()
        runCurrent()
        f.state.update(CaptureTransitions::cancelSelfTimerRun) // Accepted replacement snapshot.
        f.timer.reconcile()
        advanceTimeBy(5_000)
        runCurrent()
        assertEquals(0, f.captures)
        assertNull(f.state.value.selfTimerRemainingMs)
    }

    @Test
    fun expiryCarriesThePressIdentityAndInactivePressesAreIgnored() = runTest {
        val f = Fixture(this)
        val pressed = requireNotNull(f.foreground)
        f.timer.trigger()
        runCurrent()
        f.foreground = Any()
        advanceTimeBy(3_000)
        runCurrent()
        assertSame(pressed, f.expiredTokens.single(), "Admission checks the countdown's original identity")
        f.foreground = null
        f.timer.trigger()
        assertNull(f.state.value.selfTimerRemainingMs)
        assertEquals(1, f.captures)
    }

    @Test
    fun closeCancelsCountdownAndPreventsFurtherTriggers() = runTest {
        val f = Fixture(this)
        f.timer.trigger()
        runCurrent()
        f.timer.close()
        f.timer.trigger()
        advanceTimeBy(5_000)
        runCurrent()
        assertEquals(0, f.captures)
        assertNull(f.state.value.selfTimerRemainingMs)
    }
}
