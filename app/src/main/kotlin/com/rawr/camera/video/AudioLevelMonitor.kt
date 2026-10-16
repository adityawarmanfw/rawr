package com.rawr.camera.video

/**
 * Reference to the shared native video-mode microphone (AudioInput), for the
 * level meter. The recorder takes the same stream, so holding this across a
 * recording start causes no reopen gap. Not thread-safe; use from one thread.
 */
internal class AudioLevelMonitor private constructor(private var handle: Long) : AutoCloseable {
    private val scratch = FloatArray(7)

    /** Reads levels; peaks are the maximum since the previous read. */
    fun read(): AudioLevels {
        check(handle != 0L) { "Audio level monitor closed" }
        nativeLevels(handle, scratch)
        val channels = scratch[0].toInt()
        return AudioLevels(
            channels = channels,
            failed = scratch[1] != 0f,
            clips = scratch[2].toLong(),
            peak = FloatArray(channels) { scratch[3 + it * 2] },
            rms = FloatArray(channels) { scratch[4 + it * 2] }
        )
    }

    override fun close() {
        if (handle == 0L) return
        nativeClose(handle)
        handle = 0L
    }

    private external fun nativeLevels(handle: Long, out: FloatArray)
    private external fun nativeClose(handle: Long)

    companion object {
        init { System.loadLibrary("rawrcam_native") }

        /** Null when the microphone cannot open (permission, busy, unsupported). */
        @androidx.annotation.RequiresPermission(android.Manifest.permission.RECORD_AUDIO)
        fun open(channels: Int = 2): AudioLevelMonitor? =
            nativeOpen(channels).takeIf { it != 0L }?.let(::AudioLevelMonitor)

        @JvmStatic private external fun nativeOpen(channels: Int): Long
    }
}

/** Linear 0..1 full-scale levels per channel; [clips] counts blocks with a full-scale sample. */
internal class AudioLevels(val channels: Int, val failed: Boolean, val clips: Long,
                           val peak: FloatArray, val rms: FloatArray)
