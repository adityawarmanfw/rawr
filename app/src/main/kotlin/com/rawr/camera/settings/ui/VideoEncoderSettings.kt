package com.rawr.camera.settings.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import com.rawr.camera.settings.architecture.SetVideoEncoder
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.settings.model.KeyframeInterval
import com.rawr.camera.settings.model.SettingsUiState
import com.rawr.camera.settings.model.VideoBitrateMode
import com.rawr.camera.settings.model.VideoEncoderConfig
import com.rawr.camera.video.VideoEncoderCapabilities
import com.rawr.camera.video.VideoResolutionMode

@Composable
internal fun VideoEncoderSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val encoder = state.values.videoEncoder
    val set: (VideoEncoderConfig) -> Unit = { dispatch.invoke(SetVideoEncoder(it)) }
    val supportedModes = remember { VideoEncoderCapabilities.supportedBitrateModes }
    val maxBitrateMbps = remember { VideoEncoderCapabilities.maxBitrateMbps }
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("VideoEncoder")) {
        SettingsGroup(
            title = "Video",
            description = "HEVC settings apply to the next recording. Some encoders ignore B-frame limits; the recording monitor shows the actual GOP."
        ) {
            SettingsRow(
                "Bit depth"
            )
            if (state.values.videoLogEnabled) SettingsRow("10-bit", value = "Required by LOG")
            else CompactChoiceRow(listOf(10, 8), encoder.bitDepth, { "$it-bit" }) { set(encoder.copy(bitDepth = it)) }
            SettingDivider()
            SettingsRow("Bitrate mode")
            CompactChoiceRow(
                VideoBitrateMode.entries.filter { it.wireValue in supportedModes || it == encoder.bitrateMode },
                encoder.bitrateMode,
                { it.label }
            ) { set(encoder.copy(bitrateMode = it)) }
            VideoResolutionMode.entries.forEach { mode ->
                SettingDivider()
                StandaloneNumericSliderRow(
                    identity = "video_bitrate_${mode.name}",
                    label = "${mode.label} bitrate",
                    minimum = VideoEncoderConfig.MIN_BITRATE_MBPS.toFloat(),
                    maximum = maxBitrateMbps.toFloat(),
                    step = 1f,
                    decimals = 0,
                    value = encoder.bitrateMbps(mode).toFloat(),
                    defaultValue = VideoEncoderConfig().bitrateMbps(mode).toFloat(),
                    unit = " Mbps",
                    enabled = encoder.bitrateMode != VideoBitrateMode.Cq
                ) { set(encoder.withBitrateMbps(mode, it.toInt())) }
            }
            SettingDivider()
            val interval = encoder.keyframeInterval
            val frames = interval.frames(30)
            SettingsRow(
                "I-frame interval",
                value = if (frames <= 1) "Every frame"
                else "${if (interval.wireValue == 1) "0.5" else "${interval.wireValue / 2}"} s · ${interval.frames(24)} / $frames frames"
            )
            CompactChoiceRow(KeyframeInterval.entries, interval, { it.label }) {
                set(encoder.copy(keyframeInterval = it))
            }
            SettingDivider()
            if (frames <= 1) SettingsRow("Max B-frames", value = "Off with All-I")
            else {
                SettingsRow("Max B-frames", value = if (encoder.maxBFrames < 0) "Auto" else "${encoder.maxBFrames}")
                CompactChoiceRow(
                    VideoEncoderConfig.MAX_B_FRAME_CHOICES,
                    encoder.maxBFrames,
                    { if (it < 0) "Auto" else "$it" }
                ) { set(encoder.copy(maxBFrames = it)) }
            }
            SettingDivider()
            SettingsRow("GOP at 30 fps", value = VideoEncoderConfig.gopPattern(frames, encoder.maxBFrames))
        }
        SettingsGroup(title = "Audio", description = "AAC-LC, 48 kHz. Stereo falls back to mono at 128 kbps if the microphone can't do stereo.") {
            SettingsRow("Channels", value = if (encoder.audioChannels == 1) "Mono" else "Stereo")
            CompactChoiceRow(listOf(2, 1), encoder.audioChannels, { if (it == 1) "Mono" else "Stereo" }) {
                set(encoder.copy(audioChannels = it))
            }
            SettingDivider()
            SettingsRow("Bitrate", value = "${encoder.audioBitrateKbps} kbps")
            CompactChoiceRow(VideoEncoderConfig.AUDIO_BITRATES_KBPS, encoder.audioBitrateKbps, { "$it" }) {
                set(encoder.copy(audioBitrateKbps = it))
            }
        }
    }
}
