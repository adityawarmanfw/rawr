package com.rawr.camera.ui

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.LifecycleResumeEffect
import kotlin.math.abs

/**
 * Streams the smoothed posture while the overlay is composed and the app is resumed; the sensor is
 * released on pause and when the overlay leaves composition. Gravity is preferred, and a raw accelerometer is the fallback.
 * Reads of the returned state should happen in the draw phase so only the canvas redraws.
 */
@Composable
internal fun rememberLevelReading(): State<LevelReading?> {
    val context = LocalContext.current
    val reading = remember { mutableStateOf<LevelReading?>(null) }
    LifecycleResumeEffect(context) {
        val manager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val sensor = manager?.getDefaultSensor(Sensor.TYPE_GRAVITY)
            ?: manager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val tracker = LevelTracker()
        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                val next = tracker.update(event.values[0], event.values[1], event.values[2]) ?: return
                val prev = reading.value
                // Skip sub-0.05 degree changes so a still phone does not redraw at sensor rate.
                if (prev == null || prev.quadrant != next.quadrant ||
                    prev.rollLevel != next.rollLevel || prev.pitchLevel != next.pitchLevel ||
                    prev.tiltLevel != next.tiltLevel || abs(prev.flatBlend - next.flatBlend) > 0.005f ||
                    abs(prev.rollDeg - next.rollDeg) > 0.05f ||
                    abs(prev.elevationDeg - next.elevationDeg) > 0.05f ||
                    abs(prev.bubbleXDeg - next.bubbleXDeg) > 0.05f ||
                    abs(prev.bubbleYDeg - next.bubbleYDeg) > 0.05f
                ) reading.value = next
            }

            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
        }
        if (manager != null && sensor != null) {
            manager.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_GAME)
        }
        onPauseOrDispose {
            manager?.unregisterListener(listener)
            tracker.reset()
        }
    }
    return reading
}
