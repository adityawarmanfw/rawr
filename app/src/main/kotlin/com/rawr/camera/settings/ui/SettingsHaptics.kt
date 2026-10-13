package com.rawr.camera.settings.ui

import android.view.HapticFeedbackConstants
import android.view.View
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.platform.LocalView

/** Presentation-only haptics for Settings interactions. */
internal interface SettingsHaptics {
    fun detent()

    /** Softer detent for sliders, which tick far more often than other controls. */
    fun sliderTick() = detent()
}

private object NoOpSettingsHaptics : SettingsHaptics {
    override fun detent() = Unit
}

internal val LocalSettingsHaptics = staticCompositionLocalOf<SettingsHaptics> { NoOpSettingsHaptics }

private class AndroidSettingsHaptics(private val view: View) : SettingsHaptics {
    private val softTick = com.rawr.camera.ui.SoftTick(view)

    override fun detent() {
        view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
    }

    override fun sliderTick() = softTick.perform()
}

@Composable
internal fun rememberSettingsHaptics(): SettingsHaptics {
    val view = LocalView.current
    return remember(view) { AndroidSettingsHaptics(view) }
}
