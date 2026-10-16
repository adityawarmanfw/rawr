package com.rawr.camera.video

import android.view.Surface

/** JNI handle for the C++ recording engine. The output fd transfers ownership on nativeStart. */
internal class NativeAvEngine {
    companion object {
        init { System.loadLibrary("rawrcam_native") }
    }

    external fun nativeStart(fd: Int, width: Int, height: Int, fps: Int, bitrate: Int,
                             intraFrames: Int, rotationDegrees: Int, bitrateMode: Int,
                             maxBFrames: Int, audioChannels: Int, audioBitrate: Int,
                             audioPreset: Int, audioDeviceId: Int, audioGainDb: Float, audioLowCutHz: Int,
                             bitDepth: Int, timestampPolicy: Int, deviceMake: String,
                             deviceModel: String, software: String, renderProfile: String,
                             gamut: String, transfer: String, log: Boolean): Long
    external fun nativeSupportedTimestampPolicies(): IntArray
    external fun nativeSurface(handle: Long): Surface
    external fun nativeStats(handle: Long): String
    external fun nativeStop(handle: Long): String
}
