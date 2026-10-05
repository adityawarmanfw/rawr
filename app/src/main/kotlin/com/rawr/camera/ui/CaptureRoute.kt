package com.rawr.camera.ui

import android.net.Uri
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rawr.camera.architecture.CaptureFilmEvent
import com.rawr.camera.architecture.CaptureScreenController
import com.rawr.camera.architecture.SetOrientation
import com.rawr.camera.integration.RawPreviewCoordinator
import com.rawr.camera.model.FilmSimQuickState
import com.rawr.camera.model.RenderProfileQuickState
import com.rawr.camera.model.controlProjection
import com.rawr.camera.model.monitorProjection
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map

@Composable
fun CaptureRoute(
    controller: CaptureScreenController,
    previewCoordinator: RawPreviewCoordinator,
    renderProfiles: RenderProfileQuickState,
    onSelectRenderProfile: (com.rawr.camera.model.RenderProfileSelection) -> Unit,
    onOpenSettings: () -> Unit,
    latestImageUri: Uri? = null,
    onOpenLatestImage: () -> Unit,
    onOpenRenderer: () -> Unit,
    onOpenFilmSimSettings: () -> Unit,
    onOpenMultiframeSettings: () -> Unit = {},
    filmQuick: FilmSimQuickState? = null,
    onFilmEvent: (CaptureFilmEvent) -> Unit,
    videoControls: VideoControlState = VideoControlState(),
    onToggleVideoRecording: () -> Unit
) {
    val controlFlow = androidx.compose.runtime.remember(controller) {
        controller.state.map { it.controlProjection() }.distinctUntilChanged()
    }
    val initialControls = androidx.compose.runtime.remember(controller) { controller.state.value.controlProjection() }
    val state by controlFlow.collectAsStateWithLifecycle(initialControls)
    val monitor: @Composable () -> Unit = androidx.compose.runtime.remember(controller) {
        {
            val monitorFlow = androidx.compose.runtime.remember(controller) {
                controller.state.map { it.monitorProjection() }.distinctUntilChanged()
            }
            val initialMonitor = androidx.compose.runtime.remember(controller) { controller.state.value.monitorProjection() }
            val readouts by monitorFlow.collectAsStateWithLifecycle(initialMonitor)
            ExposureMonitor(readouts)
        }
    }
    var deviceRotationDegrees by androidx.compose.runtime.remember {
        androidx.compose.runtime.mutableIntStateOf(previewCoordinator.captureDeviceRotationDegrees)
    }
    val preview = androidx.compose.runtime.remember(previewCoordinator, deviceRotationDegrees) {
        CapturePreview(
            nativeContent = true,
            deviceRotationDegrees = deviceRotationDegrees,
            content = { RawPreviewSurface(previewCoordinator, it) },
            faceOverlay = {
                val faceFlow = androidx.compose.runtime.remember(controller) {
                    controller.state.map { it.faceDetections }.distinctUntilChanged()
                }
                val initialFaces = androidx.compose.runtime.remember(controller) { controller.state.value.faceDetections }
                val faces by faceFlow.collectAsStateWithLifecycle(initialFaces)
                FaceBoxes(faces)
            },
            onScopePresentation = { scopes ->
                val types = IntArray(3)
                val modes = IntArray(3)
                val rotations = IntArray(3)
                val rects = FloatArray(15)
                scopes.take(3).forEachIndexed { index, scope ->
                    types[index] = if (scope.type == com.rawr.camera.model.ScopeType.Waveform) 1 else 2
                    modes[index] = if (scope.type == com.rawr.camera.model.ScopeType.Waveform &&
                        scope.mode == com.rawr.camera.model.WaveformMode.RgbOverlay) 1 else 0
                    rotations[index] = scope.quarterTurns
                    rects[index * 5] = scope.x
                    rects[index * 5 + 1] = scope.y
                    rects[index * 5 + 2] = scope.width
                    rects[index * 5 + 3] = scope.height
                    rects[index * 5 + 4] = scope.cornerFraction
                }
                previewCoordinator.setScopePresentationState(types, modes, rotations, rects)
            }
        )
    }
    DeviceOrientationEffect(
        onOrientationChanged = { orientation -> controller.dispatch(SetOrientation(orientation)) },
        onDeviceRotationChanged = {
            previewCoordinator.setCaptureDeviceRotationDegrees(it)
            deviceRotationDegrees = it
        }
    )
    CaptureScreen(
        state = state,
        dispatch = controller::dispatch,
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
        onToggleVideoRecording = onToggleVideoRecording
    )
}
