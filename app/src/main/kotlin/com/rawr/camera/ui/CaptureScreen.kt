package com.rawr.camera.ui

import android.net.Uri
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.CaptureFilmEvent
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.FilmSimQuickState
import com.rawr.camera.model.RenderProfileQuickState
import com.rawr.camera.model.monitorProjection

@Composable
fun CaptureScreen(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    modifier: Modifier = Modifier,
    preview: CapturePreview = CapturePreview(),
    monitor: @Composable () -> Unit = { ExposureMonitor(state.monitorProjection()) },
    renderProfiles: RenderProfileQuickState = RenderProfileQuickState(),
    onSelectRenderProfile: (com.rawr.camera.model.RenderProfileSelection) -> Unit = {},
    onOpenSettings: () -> Unit = {},
    latestImageUri: Uri? = null,
    onOpenLatestImage: () -> Unit = {},
    onOpenRenderer: () -> Unit = {},
    onOpenFilmSimSettings: () -> Unit = {},
    onOpenMultiframeSettings: () -> Unit = {},
    filmQuick: FilmSimQuickState? = null,
    onFilmEvent: (CaptureFilmEvent) -> Unit = {},
    videoControls: VideoControlState = VideoControlState(),
    onToggleVideoRecording: () -> Unit = {}
) {
    val haptics = rememberCaptureHaptics()
    CompositionLocalProvider(
        LocalControlSurfaceStyle provides state.controlSurfaceStyle,
        LocalCaptureHaptics provides haptics
    ) {
        CaptureStateHaptics(state)
        Surface(modifier.fillMaxSize(), color = CaptureColors.Background) {
            PhysicalCaptureLayout(
                state = state,
                dispatch = dispatch,
                preview = preview,
                monitor = monitor,
                renderProfiles = renderProfiles,
                onSelectRenderProfile = onSelectRenderProfile,
                onOpenSettings = onOpenSettings,
                latestImageUri = latestImageUri,
                onOpenLatestImage = onOpenLatestImage,
                onOpenRenderer = onOpenRenderer,
                onOpenFilmSimSettings = onOpenFilmSimSettings,
                onOpenMultiframeSettings = onOpenMultiframeSettings,
                filmQuick = filmQuick,
                onFilmEvent = onFilmEvent,
                videoControls = videoControls,
                onToggleVideoRecording = onToggleVideoRecording,
                modifier =
                    Modifier
                        .fillMaxSize()
                        .statusBarsPadding()
                        .navigationBarsPadding()
            )
        }
    }
}
