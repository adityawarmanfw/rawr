package com.rawr.camera.ui

import android.os.VibrationEffect
import android.os.VibratorManager
import android.view.HapticFeedbackConstants
import android.view.View
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.platform.LocalView
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.TargetStatus

internal interface CaptureHaptics {
    fun detent()

    /** Softer detent for editing sliders, which tick far more often than capture controls. */
    fun sliderTick() = detent()

    fun selection()

    fun longPress()

    fun capture()

    fun success()

    fun failure()
}

private object NoOpCaptureHaptics : CaptureHaptics {
    override fun detent() = Unit

    override fun sliderTick() = Unit

    override fun selection() = Unit

    override fun longPress() = Unit

    override fun capture() = Unit

    override fun success() = Unit

    override fun failure() = Unit
}

internal val LocalCaptureHaptics = staticCompositionLocalOf<CaptureHaptics> { NoOpCaptureHaptics }

internal class AndroidCaptureHaptics(private val view: View) : CaptureHaptics {
    override fun detent() = perform(HapticFeedbackConstants.CLOCK_TICK)

    private val softTick = SoftTick(view)

    override fun sliderTick() = softTick.perform()

    override fun selection() = perform(HapticFeedbackConstants.CONTEXT_CLICK)

    override fun longPress() = perform(HapticFeedbackConstants.LONG_PRESS)

    // Shutter and record press: one clear click, but lighter than CONFIRM, which felt too hard.
    private val shutterClick = SoftTick(
        view, VibrationEffect.Composition.PRIMITIVE_CLICK, SHUTTER_CLICK_SCALE, HapticFeedbackConstants.CONTEXT_CLICK
    )

    override fun capture() = shutterClick.perform()

    override fun success() = perform(HapticFeedbackConstants.CONFIRM)

    override fun failure() = perform(HapticFeedbackConstants.REJECT)

    private fun perform(type: Int) {
        view.performHapticFeedback(type)
    }
}

/** Strength of the shutter click, 0..1 of the motor's CLICK primitive. Lower = softer. */
private const val SHUTTER_CLICK_SCALE = 0.7f

/**
 * One scaled vibration primitive. The default is the low-amplitude tick for dense slider detents.
 * Without primitive support, or with touch feedback off, it falls back to the system effect [fallback].
 */
internal class SoftTick(
    private val view: View,
    primitive: Int = VibrationEffect.Composition.PRIMITIVE_LOW_TICK,
    scale: Float = 0.4f,
    private val fallback: Int = HapticFeedbackConstants.CLOCK_TICK
) {
    private val vibrator = view.context.getSystemService(VibratorManager::class.java)?.defaultVibrator
    private val effect = vibrator
        ?.takeIf { it.areAllPrimitivesSupported(primitive) }
        ?.let { VibrationEffect.startComposition().addPrimitive(primitive, scale).compose() }

    fun perform() {
        val effect = effect
        if (effect == null || !view.isHapticFeedbackEnabled) view.performHapticFeedback(fallback)
        else vibrator?.vibrate(effect)
    }
}

@Composable
internal fun rememberCaptureHaptics(): CaptureHaptics {
    val view = LocalView.current
    return androidx.compose.runtime.remember(view) { AndroidCaptureHaptics(view) }
}

/** Emits only completion/failure feedback; interaction-start haptics stay at gesture sites. */
@Composable
internal fun CaptureStateHaptics(state: CaptureUiState) {
    val haptics = LocalCaptureHaptics.current
    var previousFocus by androidx.compose.runtime.remember {
        androidx.compose.runtime.mutableStateOf(
            state.focus.status
        )
    }
    var previousSpotAe by androidx.compose.runtime.remember {
        androidx.compose.runtime.mutableStateOf(
            state.spotAe.status
        )
    }
    var previousTimerSecond by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf<Int?>(null) }

    LaunchedEffect(state.focus.status) {
        val next = state.focus.status
        if (next != previousFocus) {
            when (next) {
                TargetStatus.Settled -> if (previousFocus == TargetStatus.Settling) haptics.success()
                TargetStatus.Failed -> haptics.failure()
                else -> Unit
            }
            previousFocus = next
        }
    }

    LaunchedEffect(state.spotAe.status) {
        val next = state.spotAe.status
        if (next != previousSpotAe) {
            when (next) {
                TargetStatus.Settled -> if (previousSpotAe == TargetStatus.Settling) haptics.success()
                TargetStatus.Failed -> haptics.failure()
                else -> Unit
            }
            previousSpotAe = next
        }
    }

    // HDR+ Bracketed: a light tick once the post-shutter frames are in (the user may move).
    var previousBracketDone by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf<Long?>(null) }
    LaunchedEffect(state.bracketHoldDone) {
        val previous = previousBracketDone
        if (previous != null && state.bracketHoldDone > previous) haptics.sliderTick()
        previousBracketDone = state.bracketHoldDone
    }

    // Self-timer countdown ticks: one detent per whole-second boundary.
    val timerSecond = state.selfTimerRemainingMs?.let { ((it + 999) / 1000).toInt().coerceAtLeast(1) }
    LaunchedEffect(timerSecond) {
        val previous = previousTimerSecond
        if (timerSecond != null && previous != null && timerSecond < previous) {
            haptics.detent()
        }
        previousTimerSecond = timerSecond
    }
}
