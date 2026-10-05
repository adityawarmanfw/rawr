package com.rawr.camera.ui

import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.CaptureFilmEvent
import com.rawr.camera.model.CaptureControlLayout
import com.rawr.camera.model.CaptureMode
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.FilmSimQuickState
import com.rawr.camera.model.RenderProfileQuickState
import com.rawr.camera.model.monitorProjection
import com.rawr.camera.video.VideoResolutionMode

/**
 * Single physical capture hierarchy for every device posture. The Activity remains
 * portrait-locked; orientation changes only content rotation, gesture interpretation,
 * pointer mapping, and deterministic scope slots.
 */
@Composable
internal fun PhysicalCaptureLayout(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    preview: CapturePreview = CapturePreview(),
    monitor: @Composable () -> Unit = { ExposureMonitor(state.monitorProjection()) },
    renderProfiles: RenderProfileQuickState,
    onSelectRenderProfile: (com.rawr.camera.model.RenderProfileSelection) -> Unit,
    onOpenSettings: () -> Unit,
    latestImageUri: Uri?,
    onOpenLatestImage: () -> Unit,
    onOpenRenderer: () -> Unit,
    onOpenFilmSimSettings: () -> Unit,
    onOpenMultiframeSettings: () -> Unit = {},
    modifier: Modifier,
    filmQuick: FilmSimQuickState? = null,
    onFilmEvent: (CaptureFilmEvent) -> Unit = {},
    videoControls: VideoControlState = VideoControlState(),
    onToggleVideoRecording: () -> Unit = {}
) {
    val videoLocked = videoControls.busy || videoControls.recording
    val isVideo = state.captureMode == CaptureMode.Video
    val compactControls = isVideo || state.captureLayout == CaptureControlLayout.Compact
    // 16:9 viewfinders run past the photo frame and behind the shutter
    // cluster; Open Gate is exactly the photo box, so nothing bleeds.
    val videoBleed = isVideo && state.videoResolution != VideoResolutionMode.OPEN_GATE
    BoxWithConstraints(modifier) {
        // Reserve the shutter/record area and both monitors before sizing the
        // 3:4 viewfinder. A short display must not push the readout below nav.
        val controlsHeight = if (state.captureLayout == CaptureControlLayout.Compact) {
            CaptureDimens.CompactVideoControlsMinimumHeight
        } else {
            CaptureDimens.VideoControlsClassicHeight
        }
        val viewfinderMaxHeight = (maxHeight - CaptureDimens.TopBarHeight -
            CaptureDimens.MonitorHeight * 2 - controlsHeight - CaptureDimens.VideoMonitorBottomInset)
            .coerceAtLeast(1.dp)
        val viewfinderWidth = minOf(maxWidth, viewfinderMaxHeight * .75f)
        // The photo 3:4 frame anchors every mode: the bottom strip sits at its
        // bottom edge and the shutter cluster directly below it, so photo,
        // Open Gate, and 16:9 video share identical control geometry.
        val photoFrame = DpSize(viewfinderWidth, viewfinderWidth * 4f / 3f)
        Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally) {
            CaptureTopBar(state, dispatch, onOpenSettings, videoLocked = videoLocked)
            monitor()
            Box(Modifier.fillMaxWidth().weight(1f)) {
                CaptureViewfinder(
                    state = state,
                    dispatch = dispatch,
                    preview = preview,
                    renderProfiles = renderProfiles.copy(locked = isVideo && videoLocked),
                    onSelectRenderProfile = onSelectRenderProfile,
                    modifier = Modifier.viewfinderFrame(videoBleed, viewfinderWidth)
                        .align(Alignment.TopCenter),
                    slotFrame = photoFrame,
                    forceCompact = isVideo,
                    forceTonemapStrip = isVideo,
                    filmQuick = filmQuick,
                    onFilmEvent = onFilmEvent
                )
                if (isVideo && videoControls.recording) {
                    VideoRecordTimer(
                        status = videoControls.status,
                        dropped = videoControls.timing?.dropped,
                        orientation = state.orientation,
                        modifier = Modifier.align(Alignment.TopCenter).padding(top = 8.dp)
                    )
                }
                // Shutter cluster starts at the photo frame bottom in every
                // mode. 16:9 video floats it over the preview bleed; photo
                // and Open Gate paint the surface color down to the monitor row.
                Column(Modifier.fillMaxSize()) {
                    Spacer(Modifier.height(photoFrame.height))
                    CaptureControls(
                        state = state,
                        dispatch = dispatch,
                        latestImageUri = latestImageUri,
                        onOpenLatestImage = onOpenLatestImage,
                        onOpenRenderer = onOpenRenderer,
                        onOpenFilmSimSettings = onOpenFilmSimSettings,
                        onOpenMultiframeSettings = onOpenMultiframeSettings,
                        videoControls = videoControls,
                        onToggleVideoRecording = onToggleVideoRecording,
                        overlay = videoBleed,
                        modifier = if (compactControls) {
                            Modifier.fillMaxWidth().weight(1f)
                        } else {
                            Modifier.fillMaxWidth().height(CaptureDimens.VideoControlsClassicHeight)
                        }
                    )
                }
            }
            if (isVideo) {
                VideoTimingMonitor(videoControls)
            } else {
                // Same height and color as the video timing row, so nothing
                // above it shifts between modes.
                Spacer(
                    Modifier.fillMaxWidth().height(CaptureDimens.MonitorHeight)
                        .background(CaptureColors.Surface)
                )
            }
            Spacer(Modifier.height(CaptureDimens.VideoMonitorBottomInset))
        }
    }
}

/**
 * Viewfinder box for the current mode. The Activity is portrait-locked, so a
 * landscape 16:9 recording presents as a 9:16 box; Open Gate matches the ~4:3
 * sensor and uses exactly the photo box.
 *
 * 16:9 boxes are full-bleed width and may run taller than the remaining space
 * (unbounded height, never scaled down); the overflow draws behind the shutter
 * cluster. Native presentation is aspect-fit and the idle preview is cropped
 * to the exact record rect, so the box fills with no bars and framing never
 * jumps at record start. No image work happens here — only box sizing.
 */
private fun Modifier.viewfinderFrame(videoBleed: Boolean, photoWidth: Dp): Modifier = if (videoBleed) {
    this.fillMaxWidth()
        .wrapContentHeight(Alignment.Top, unbounded = true)
        .aspectRatio(9f / 16f)
} else {
    this.width(photoWidth).aspectRatio(3f / 4f)
}
