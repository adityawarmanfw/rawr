package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun DenoiseSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    SettingsPageContainer {
        DenoiseTargetSelector(state, dispatch)
        PhotoWaveletGroup(state, dispatch)
        VideoDenoiseGroup(state, dispatch)
    }
}

@Composable
internal fun DenoiseGroup(state: SettingsUiState, dispatch: SettingsDispatch) {
    DenoiseTargetSelector(state, dispatch)
    PhotoWaveletGroup(state, dispatch)
    VideoDenoiseGroup(state, dispatch)
}

@Composable
private fun DenoiseTargetSelector(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsGroup(
        description = "Photo denoise runs on single-frame stills only; multiframe merges denoise themselves. Video denoise is wavelets only, with its own strength."
    ) {
        PhotoVideoTargetSelector(
            photoOn = v.photoDenoise is DenoiseConfig.Wavelet,
            videoOn = v.videoDenoiseEnabled,
            onPhotoChange = { dispatch.invoke(SetPhotoDenoiseEnabled(it)) },
            onVideoChange = { dispatch.invoke(SetVideoDenoiseEnabled(it)) }
        )
    }
}

@Composable
private fun PhotoWaveletGroup(state: SettingsUiState, dispatch: SettingsDispatch) {
    // Off parks the last values; show them greyed out, like video.
    val on = state.values.photoDenoise is DenoiseConfig.Wavelet
    val wavelet = state.values.photoDenoise.config
    SettingsGroup(title = "Photo · Wavelet") {
        StandaloneNumericSliderRow(
            identity = "denoise_strength_photo",
            label = "Strength",
            minimum = 0f,
            maximum = 8f,
            step = 0.1f,
            decimals = 1,
            value = wavelet.strength,
            enabled = on
        ) { dispatch.invoke(SetPhotoDenoiseStrength(it)) }
        SettingDivider()
        StandaloneNumericSliderRow(
            identity = "denoise_detail_photo",
            label = "Detail",
            minimum = 0f,
            maximum = 1.8f,
            step = 0.05f,
            decimals = 2,
            value = wavelet.detail,
            enabled = on
        ) { dispatch.invoke(SetPhotoDenoiseDetail(it)) }
        SettingDivider()
        StandaloneNumericSliderRow(
            identity = "denoise_luma_photo",
            label = "Luma",
            minimum = 0f,
            maximum = 1f,
            step = 0.05f,
            decimals = 2,
            value = wavelet.luma,
            enabled = on
        ) { dispatch.invoke(SetPhotoDenoiseLuma(it)) }
        SettingDivider()
        StandaloneNumericSliderRow(
            identity = "denoise_scales_photo",
            label = "Scales",
            minimum = 1f,
            maximum = 7f,
            step = 1f,
            decimals = 0,
            value = wavelet.scales.toFloat(),
            enabled = on
        ) { dispatch.invoke(SetPhotoDenoiseScales(it.toInt())) }
    }
}

@Composable
private fun VideoDenoiseGroup(state: SettingsUiState, dispatch: SettingsDispatch) {
    val v = state.values
    SettingsGroup(title = "Video · Wavelet") {
        StandaloneNumericSliderRow(
            identity = "denoise_strength_video",
            label = "Strength",
            minimum = 0f,
            maximum = 8f,
            step = 0.1f,
            decimals = 1,
            value = v.videoDenoiseStrength,
            enabled = v.videoDenoiseEnabled
        ) { dispatch.invoke(SetVideoDenoiseStrength(it)) }
        SettingDivider()
        StandaloneNumericSliderRow(
            identity = "denoise_detail_video",
            label = "Detail",
            minimum = 0f,
            maximum = 1.8f,
            step = 0.05f,
            decimals = 2,
            value = v.videoDenoiseDetail,
            enabled = v.videoDenoiseEnabled
        ) { dispatch.invoke(SetVideoDenoiseDetail(it)) }
        SettingDivider()
        StandaloneNumericSliderRow(
            identity = "denoise_luma_video",
            label = "Luma",
            minimum = 0f,
            maximum = 1f,
            step = 0.05f,
            decimals = 2,
            value = v.videoDenoiseLuma,
            enabled = v.videoDenoiseEnabled
        ) { dispatch.invoke(SetVideoDenoiseLuma(it)) }
        SettingDivider()
        StandaloneNumericSliderRow(
            identity = "denoise_scales_video",
            label = "Scales",
            minimum = 1f,
            maximum = 7f,
            step = 1f,
            decimals = 0,
            value = v.videoDenoiseScales.toFloat(),
            enabled = v.videoDenoiseEnabled
        ) { dispatch.invoke(SetVideoDenoiseScales(it.toInt())) }
    }
}
