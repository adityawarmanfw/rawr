package com.rawr.camera.video

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import com.rawr.camera.settings.model.VideoAudioRoute
import com.rawr.camera.settings.model.VideoEncoderConfig

/** Microphone setup shared by the video-mode meter and the recorder. */
data class VideoAudioInput(
    val channels: Int = 2,
    /** AAudio input preset (VideoAudioSource.wireValue). */
    val preset: Int = 5,
    val route: VideoAudioRoute = VideoAudioRoute.Auto,
    val gainDb: Int = 0,
    val lowCutHz: Int = 0
) {
    /** AudioDeviceInfo id for [route]; 0 lets Android route. External falls back to Auto when none is attached. */
    fun deviceId(context: Context): Int {
        val types = when (route) {
            VideoAudioRoute.Auto -> return 0
            VideoAudioRoute.BuiltIn -> setOf(AudioDeviceInfo.TYPE_BUILTIN_MIC)
            VideoAudioRoute.External -> EXTERNAL_TYPES
        }
        val manager = context.getSystemService(AudioManager::class.java) ?: return 0
        return manager.getDevices(AudioManager.GET_DEVICES_INPUTS).firstOrNull { it.type in types }?.id ?: 0
    }

    companion object {
        private val EXTERNAL_TYPES = setOf(
            AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADSET
        )

        fun of(encoder: VideoEncoderConfig) = VideoAudioInput(
            channels = encoder.audioChannels, preset = encoder.audioSource.wireValue,
            route = encoder.audioRoute, gainDb = encoder.audioGainDb, lowCutHz = encoder.audioLowCutHz
        )

        /** Whether the device advertises a real UNPROCESSED source (no AGC/NS). */
        fun supportsUnprocessed(context: Context): Boolean =
            context.getSystemService(AudioManager::class.java)
                ?.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true"
    }
}
