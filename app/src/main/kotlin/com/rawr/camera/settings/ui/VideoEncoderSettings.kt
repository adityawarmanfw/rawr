package com.rawr.camera.settings.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import com.rawr.camera.settings.architecture.SetVideoEncoder
import com.rawr.camera.settings.architecture.SettingsDispatch
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
            description = "HEVC settings apply to the next recording."
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
            val frames = encoder.keyframeFrames
            StandaloneNumericSliderRow(
                identity = "video_keyframe_frames",
                label = "I-frame interval",
                supportingText = when {
                    frames <= 1 -> "All-intra: every frame is an I-frame. Largest files, easiest to edit."
                    frames < VideoEncoderConfig.SHORT_GOP_FRAMES ->
                        "Very short GOP: file size and bitrate demand rise sharply."
                    else -> "%.2f s at 24 fps · %.2f s at 30 fps".format(frames / 24f, frames / 30f)
                },
                minimum = VideoEncoderConfig.MIN_KEYFRAME_FRAMES.toFloat(),
                maximum = VideoEncoderConfig.MAX_KEYFRAME_FRAMES.toFloat(),
                step = 1f,
                decimals = 0,
                value = frames.toFloat(),
                defaultValue = VideoEncoderConfig().keyframeFrames.toFloat(),
                valueFormatter = { if (it.toInt() <= 1) "All-I" else "${it.toInt()} frames" }
            ) { set(encoder.copy(keyframeFrames = it.toInt())) }
            SettingDivider()
            if (frames <= 1) SettingsRow("Max B-frames", value = "Off (all-intra)")
            else {
                SettingsRow("Max B-frames")
                CompactChoiceRow(
                    VideoEncoderConfig.MAX_B_FRAME_CHOICES,
                    encoder.maxBFrames,
                    { if (it < 0) "Auto" else "$it" }
                ) { set(encoder.copy(maxBFrames = it)) }
            }
            SettingsRow(
                "GOP",
                supportingText = VideoEncoderConfig.gopPattern(frames, encoder.maxBFrames) +
                    "\nRequested structure; some encoders ignore B-frame limits."
            )
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
