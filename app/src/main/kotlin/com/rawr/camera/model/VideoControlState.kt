package com.rawr.camera.model

/** Presentation of the recording controls, separate from recorder lifecycle ownership. */
data class VideoControlState(
    val resolution: String = "1080p",
    val fps: Int = 30,
    val recording: Boolean = false,
    val busy: Boolean = false,
    val status: String = "REC",
    val timing: VideoTimingState? = null
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
    val encoderPeakMs: Double = 0.0
)
