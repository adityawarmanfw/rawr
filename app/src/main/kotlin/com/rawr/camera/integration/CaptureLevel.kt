package com.rawr.camera.integration

/**
 * Camera attitude at the shutter press, measured against gravity.
 *
 * [pitchDeg] is the lens elevation above the horizon (0 = level, +90 = sky, -90 = ground).
 * [rollDeg] is the turn about the lens axis, positive = clockwise as the photographer sees it (the
 * horizon then leans counter-clockwise in the frame), measured from the nearest 90 degrees of device
 * orientation. It is null when the lens points near vertical, where roll has no meaning.
 */
data class CaptureLevel(val pitchDeg: Float, val rollDeg: Float?) {
    /** JNI form: {pitch, roll}, with NaN standing for an absent roll. */
    fun toNative(): FloatArray = floatArrayOf(pitchDeg, rollDeg ?: Float.NaN)
}
