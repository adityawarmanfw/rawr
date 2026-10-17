package com.rawr.camera.model

/** Presentation of the recording controls, separate from recorder lifecycle ownership. */
data class VideoControlState(
    val resolution: String = "1080p",
    val fps: Int = 30,
    val recording: Boolean = false,
    val busy: Boolean = false,
    val status: String = "REC",
    val timing: VideoTimingState? = null,
    val audio: com.rawr.camera.video.VideoAudioInput = com.rawr.camera.video.VideoAudioInput()
)

data class VideoTimingState(
    val actualFps: Double,
    val dropped: Long,
    val shortfall: Long,
    val gpuMs: Double,
    /** Sensor capture to encoded output (queue time to output on drivers without display timing). */
    val encoderMs: Double,
    val micDbfs: Double,
    val dropReason: String? = null,
    /** Worst frame in the last ~2 s; the averages hide the single slow frame behind a drop. */
    val gpuPeakMs: Double = 0.0,
    val encoderPeakMs: Double = 0.0,
    /** Encoded frame counts from slice headers, and the last complete I-to-I distance (0 until two I-frames). */
    val iFrames: Long = 0,
    val pFrames: Long = 0,
    val bFrames: Long = 0,
    val actualGop: Long = 0
) {
    /** e.g. "30 IPB": measured GOP plus the frame types the encoder actually produced. */
    val gopSummary: String get() {
        val types = buildString { if (iFrames > 0) append('I'); if (pFrames > 0) append('P'); if (bFrames > 0) append('B') }
        val gop = when {
            actualGop > 0 -> "$actualGop"
            iFrames > 0 && pFrames == 0L && bFrames == 0L -> "1"
            else -> "—"
        }
        return if (types.isEmpty()) "—" else "$gop $types"
    }
}
