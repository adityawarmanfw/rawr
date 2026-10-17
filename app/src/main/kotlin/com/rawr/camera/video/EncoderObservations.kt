package com.rawr.camera.video

import android.content.Context
import org.json.JSONObject

/**
 * What the HEVC encoder actually did in recent recordings, for settings that
 * MediaCodec accepts but may silently ignore (many SoCs never emit B-frames).
 */
object EncoderObservations {
    private const val PREFS = "rawr_video_encoder_observations"
    private const val B_FRAMES = "b_frames_produced"
    private const val CODEC = "codec"
    /** Enough frames for several GOPs, so a missing B-frame is not chance. */
    private const val MIN_FRAMES = 60

    /** True/false once a recording that allowed B-frames has finished; null before that. */
    fun bFramesProduced(context: Context): Boolean? {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        return if (prefs.contains(B_FRAMES)) prefs.getBoolean(B_FRAMES, false) else null
    }

    /** Records the outcome of a finished recording from its native stats. */
    fun record(context: Context, maxBFrames: Int, intraFrames: Int, stats: JSONObject) {
        if (maxBFrames == 0 || intraFrames <= 1 || stats.optLong("encodedFrames") < MIN_FRAMES) return
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putBoolean(B_FRAMES, stats.optLong("bFrames") > 0)
            .putString(CODEC, stats.optString("videoCodec"))
            .apply()
    }
}
