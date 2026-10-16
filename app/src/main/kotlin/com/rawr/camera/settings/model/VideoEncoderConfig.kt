package com.rawr.camera.settings.model

import com.rawr.camera.video.VideoResolutionMode

/** MediaCodec bitrate modes, with their KEY_BITRATE_MODE wire values. */
enum class VideoBitrateMode(val wireValue: Int, val label: String) {
    Cbr(2, "CBR"),
    Vbr(1, "VBR"),
    Cq(0, "CQ");

    companion object {
        fun of(wireValue: Int): VideoBitrateMode = entries.firstOrNull { it.wireValue == wireValue } ?: Cbr
    }
}

/**
 * Video encoder settings, applied when a recording starts. The factory
 * clamps every field to what NativeAvRecorder accepts, so a stale or
 * hand-edited preference can never fail recording start.
 */
data class VideoEncoderConfig(
    /** 8 = HEVC Main, 10 = HEVC Main10. */
    val bitDepth: Int = 10,
    val bitrate1080pMbps: Int = 12,
    val bitrate4kMbps: Int = 40,
    val bitrateOpenGateMbps: Int = 60,
    val bitrateMode: VideoBitrateMode = VideoBitrateMode.Cbr,
    /** Frames between I-frames; 1 = all-intra. */
    val keyframeFrames: Int = 30,
    /** -1 lets the codec choose; 0 disables B-frames. */
    val maxBFrames: Int = -1,
    /** 1 = mono, 2 = stereo. */
    val audioChannels: Int = 2,
    val audioBitrateKbps: Int = 192
) {
    fun sanitized(): VideoEncoderConfig = copy(
        bitDepth = if (bitDepth == 8) 8 else 10,
        bitrate1080pMbps = bitrate1080pMbps.coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS),
        bitrate4kMbps = bitrate4kMbps.coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS),
        bitrateOpenGateMbps = bitrateOpenGateMbps.coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS),
        keyframeFrames = keyframeFrames.coerceIn(MIN_KEYFRAME_FRAMES, MAX_KEYFRAME_FRAMES),
        maxBFrames = maxBFrames.coerceIn(-1, 2),
        audioChannels = if (audioChannels == 1) 1 else 2,
        audioBitrateKbps = AUDIO_BITRATES_KBPS.minBy { kotlin.math.abs(it - audioBitrateKbps) }
    )

    fun bitrateMbps(mode: VideoResolutionMode): Int = when (mode) {
        VideoResolutionMode.HD1080 -> bitrate1080pMbps
        VideoResolutionMode.UHD4K -> bitrate4kMbps
        VideoResolutionMode.OPEN_GATE -> bitrateOpenGateMbps
    }

    fun withBitrateMbps(mode: VideoResolutionMode, mbps: Int): VideoEncoderConfig = when (mode) {
        VideoResolutionMode.HD1080 -> copy(bitrate1080pMbps = mbps)
        VideoResolutionMode.UHD4K -> copy(bitrate4kMbps = mbps)
        VideoResolutionMode.OPEN_GATE -> copy(bitrateOpenGateMbps = mbps)
    }.sanitized()

    companion object {
        const val MIN_BITRATE_MBPS = com.rawr.camera.video.VideoEncoderLimits.MIN_BITRATE_MBPS
        const val MAX_BITRATE_MBPS = com.rawr.camera.video.VideoEncoderLimits.MAX_BITRATE_MBPS
        val AUDIO_BITRATES_KBPS = listOf(128, 192, 256, 320)
        val MAX_B_FRAME_CHOICES = listOf(-1, 0, 1, 2)
        const val MIN_KEYFRAME_FRAMES = 1
        const val MAX_KEYFRAME_FRAMES = 60
        /** Below this, every few frames is an I-frame and file size climbs steeply. */
        const val SHORT_GOP_FRAMES = 5

        /** Requested GOP in display order, e.g. "I B B P B B P … (30)"; all-intra is "I I I …". */
        fun gopPattern(frames: Int, maxBFrames: Int): String {
            if (frames <= 1) return "I I I … (all-intra)"
            val shown = minOf(frames - 1, 6)
            val run = List(maxBFrames.coerceAtLeast(0)) { "B" } + "P"
            val tail = generateSequence { run }.flatten().take(shown).toList()
            val more = if (frames - 1 > shown) " …" else ""
            val auto = if (maxBFrames < 0) " · B-frames auto" else ""
            return (listOf("I") + tail).joinToString(" ") + "$more ($frames)$auto"
        }
    }
}
