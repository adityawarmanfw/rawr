package com.rawr.camera.video

/**
 * Reference to the shared native video-mode microphone (AudioInput), for the
 * level meter. The recorder takes the same stream, so holding this across a
 * recording start causes no reopen gap. Not thread-safe; use from one thread.
 */
internal class AudioLevelMonitor private constructor(private var handle: Long) : AutoCloseable {
    private val scratch = FloatArray(8)

    /**
     * Reads levels; peaks are the maximum since the previous read. Null once closed: a
     * frame callback can still run after the screen pauses and closes the monitor.
     */
    fun read(): AudioLevels? {
        if (handle == 0L) return null
        nativeLevels(handle, scratch)
        val channels = scratch[0].toInt()
        return AudioLevels(
            channels = channels,
            failed = scratch[1] != 0f,
            inputClips = scratch[2].toLong(),
            outputClips = scratch[3].toLong(),
            peak = FloatArray(channels) { scratch[4 + it * 2] },
            rms = FloatArray(channels) { scratch[5 + it * 2] }
        )
    }

    /** Live gain and low-cut; applies to the recording too, since the stream is shared. */
    fun setProcessing(gainDb: Int, lowCutHz: Int) {
        if (handle != 0L) nativeSetProcessing(handle, gainDb.toFloat(), lowCutHz)
    }

    override fun close() {
        if (handle == 0L) return
        nativeClose(handle)
        handle = 0L
    }

    private external fun nativeLevels(handle: Long, out: FloatArray)
    private external fun nativeClose(handle: Long)
    private external fun nativeSetProcessing(handle: Long, gainDb: Float, lowCutHz: Int)

    companion object {
        init { System.loadLibrary("rawrcam_native") }

        /** Null when the microphone cannot open (permission, busy, unsupported). */
        @androidx.annotation.RequiresPermission(android.Manifest.permission.RECORD_AUDIO)
        fun open(context: android.content.Context, input: VideoAudioInput): AudioLevelMonitor? =
            nativeOpen(input.channels, input.preset, input.deviceId(context)).takeIf { it != 0L }
                ?.let(::AudioLevelMonitor)?.also { it.setProcessing(input.gainDb, input.lowCutHz) }

        @JvmStatic private external fun nativeOpen(channels: Int, preset: Int, deviceId: Int): Long
    }
}

/**
 * Linear 0..1 full-scale levels per channel, after gain. [inputClips] counts blocks where the
 * mic itself hit full scale; [outputClips] blocks where gain pushed a sample past it.
 */
internal class AudioLevels(val channels: Int, val failed: Boolean, val inputClips: Long, val outputClips: Long,
                           val peak: FloatArray, val rms: FloatArray)
