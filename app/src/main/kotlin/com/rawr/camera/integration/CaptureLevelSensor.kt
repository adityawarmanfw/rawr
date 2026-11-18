package com.rawr.camera.integration

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.SystemClock
import com.rawr.camera.ui.LevelReading
import com.rawr.camera.ui.LevelTracker
import java.util.concurrent.atomic.AtomicReference

/** Pure mapping from the on-screen reading to what a capture records (see [CaptureLevel] for the signs). */
internal fun LevelReading.toCaptureLevel(): CaptureLevel =
    // The tracker's roll is positive for a counter-clockwise turn; the file records clockwise as positive.
    CaptureLevel(pitchDeg = elevationDeg, rollDeg = if (flat) null else -rollDeg)

/**
 * Keeps the latest camera attitude while the capture screen is in the foreground, independent of the
 * level overlay, so every capture can record it. Gravity sensor first, accelerometer as the fallback.
 * [snapshot] is safe from any thread and returns null when there is no fresh reading.
 */
class CaptureLevelSensor(
    private val context: Context,
    private val nowNanos: () -> Long = SystemClock::elapsedRealtimeNanos
) {
    private class Sample(val level: CaptureLevel, val atNanos: Long)

    private val latest = AtomicReference<Sample?>(null)
    private val tracker = LevelTracker()
    private var listener: SensorEventListener? = null

    @Synchronized
    fun setActive(active: Boolean) {
        if (!active) {
            listener?.let { context.getSystemService(SensorManager::class.java)?.unregisterListener(it) }
            listener = null
            tracker.reset()
            latest.set(null)
            return
        }
        if (listener != null) return
        val manager = context.getSystemService(SensorManager::class.java) ?: return
        val sensor = manager.getDefaultSensor(Sensor.TYPE_GRAVITY)
            ?: manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) ?: return
        val next = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                val reading = tracker.update(event.values[0], event.values[1], event.values[2]) ?: return
                latest.set(Sample(reading.toCaptureLevel(), nowNanos()))
            }

            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
        }
        if (manager.registerListener(next, sensor, SensorManager.SENSOR_DELAY_GAME)) listener = next
    }

    /** The newest attitude, or null when the sensor is off or has gone quiet. */
    fun snapshot(): CaptureLevel? {
        val sample = latest.get() ?: return null
        return if (nowNanos() - sample.atNanos <= MAX_AGE_NANOS) sample.level else null
    }

    private companion object {
        const val MAX_AGE_NANOS = 500_000_000L
    }
}
