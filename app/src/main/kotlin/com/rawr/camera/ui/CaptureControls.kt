package com.rawr.camera.ui

import android.net.Uri
import android.util.Size
import androidx.compose.foundation.Image
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.outlined.BurstMode
import com.rawr.camera.ui.icons.outlined.MovieFilter
import androidx.compose.material3.Text
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.SelectLens
import com.rawr.camera.architecture.SetCaptureMode
import com.rawr.camera.architecture.ToggleFilmSim
import com.rawr.camera.architecture.ToggleMultiframe
import com.rawr.camera.architecture.TriggerCapture
import com.rawr.camera.model.CaptureControlLayout
import com.rawr.camera.model.CaptureMode
import com.rawr.camera.model.RenderProfileQuickState
import com.rawr.camera.model.CaptureSavePhase
import com.rawr.camera.model.CaptureUiState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@Composable
internal fun CaptureControls(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    modifier: Modifier,
    latestImageUri: Uri? = null,
    onOpenLatestImage: () -> Unit = {},
    onOpenRenderer: () -> Unit = {},
    onOpenFilmSimSettings: () -> Unit = {},
    onOpenMultiframeSettings: () -> Unit = {},
    videoControls: VideoControlState = VideoControlState(),
    onToggleVideoRecording: () -> Unit = {},
    /** Transparent background for floating over the 16:9 bleed (video overlay slot). */
    overlay: Boolean = false,
    renderProfiles: RenderProfileQuickState = RenderProfileQuickState(),
    onOpenFilters: () -> Unit = {},
    onOpenParams: () -> Unit = {}
) {
    val pro = state.captureLayout == CaptureControlLayout.Pro
    val isVideo = state.captureMode == CaptureMode.Video
    // Lens switching and mode switching are disabled while a video recording
    // is active; the native session must not bounce mid-record.
    val recordingLock = isVideo && videoControls.recording
    if (isVideo || state.captureLayout.usesButtonStrip) {
        // Shared bottom cluster: identical heights and element positions in
        // both modes. The center swaps photo shutter <-> 70dp record button;
        // video sides stay empty (same row height); the mode strip is always
        // below. No thumbnail, MERGED, or FILM in video mode.
        Column(
            modifier
                .background(if (isVideo && overlay) Color.Transparent else CaptureColors.Surface)
                .padding(horizontal = 12.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                LensSelector(
                    state = state,
                    dispatch = dispatch,
                    enabled = !recordingLock,
                    modifier = Modifier.padding(top = 2.dp)
                )
                if (pro && !isVideo) {
                    ProMergedToggle(
                        state = state,
                        onToggle = { dispatch(ToggleMultiframe) },
                        onOpenSettings = onOpenMultiframeSettings
                    )
                }
            }
            Row(
                Modifier.fillMaxWidth().padding(top = 4.dp, bottom = 4.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                    if (!isVideo) {
                        Thumbnail(
                            state = state,
                            imageUri = latestImageUri,
                            onClick = onOpenLatestImage,
                            onLongClick = onOpenRenderer,
                            modifier = Modifier
                        )
                    }
                }
                if (isVideo) {
                    VideoRecordButton(
                        state = videoControls,
                        orientation = state.orientation,
                        onClick = onToggleVideoRecording,
                        countingDown = state.selfTimerRemainingMs != null
                    )
                } else {
                    Shutter(
                        dispatch = dispatch,
                        modifier = Modifier,
                        countingDown = state.selfTimerRemainingMs != null
                    )
                }
                Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                    if (!isVideo) {
                        if (pro) {
                            ProSideButtons(state, renderProfiles, onOpenFilters, onOpenParams)
                        } else {
                            PhotoLookToggles(state, dispatch, onOpenFilmSimSettings, onOpenMultiframeSettings)
                        }
                    }
                }
            }
            ModeSelectorStrip(
                state = state,
                dispatch = dispatch,
                enabled = !recordingLock,
                modifier = Modifier.padding(bottom = 6.dp)
            )
        }
        return
    }
    Box(modifier.background(CaptureColors.Surface)) {
        LensSelector(
            state = state,
            dispatch = dispatch,
            modifier = Modifier.align(Alignment.TopCenter).padding(top = 5.dp)
        )
        Shutter(
            dispatch = dispatch,
            modifier = Modifier.align(Alignment.TopCenter).padding(top = 39.dp),
            countingDown = state.selfTimerRemainingMs != null
        )
        ModeSelectorStrip(
            state = state,
            dispatch = dispatch,
            modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 8.dp)
        )
        Thumbnail(
            state = state,
            imageUri = latestImageUri,
            onClick = onOpenLatestImage,
            onLongClick = onOpenRenderer,
            modifier = Modifier.align(Alignment.TopStart).padding(start = 34.dp, top = 50.dp)
        )
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 12.dp).padding(top = 50.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Spacer(Modifier.weight(1f))
            Spacer(Modifier.width(CaptureDimens.ShutterOuter))
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                PhotoLookToggles(state, dispatch, onOpenFilmSimSettings, onOpenMultiframeSettings)
            }
        }
    }
}

@Composable
private fun PhotoLookToggles(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    onOpenFilmSimSettings: () -> Unit,
    onOpenMultiframeSettings: () -> Unit
) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        CompactSidePill(
            icon = Icons.Outlined.BurstMode,
            label = "MERGED",
            active = state.experimentalMultiframeEnabled,
            testTag = CaptureTestTags.COMPACT_MULTIFRAME,
            contentDescription = if (state.experimentalMultiframeEnabled) {
                "Multiframe on. Tap to turn off. Long press for settings."
            } else {
                "Multiframe off. Tap to turn on. Long press for settings."
            },
            onClick = { dispatch(ToggleMultiframe) },
            onLongClick = onOpenMultiframeSettings,
            onLongClickLabel = "Multiframe settings",
            modifier = Modifier
        )
        CompactSidePill(
            icon = Icons.Outlined.MovieFilter,
            label = "FILM",
            active = state.filmSimEnabled,
            testTag = CaptureTestTags.COMPACT_FILMSIM,
            contentDescription = if (state.filmSimEnabled) {
                "Film simulation on. Tap to turn off. Long press for settings."
            } else {
                "Film simulation off. Tap to turn on. Long press for settings."
            },
            onClick = { dispatch(ToggleFilmSim) },
            onLongClick = onOpenFilmSimSettings,
            onLongClickLabel = "Film simulation settings",
            modifier = Modifier
        )
    }
}

@Composable
internal fun LensSelector(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    modifier: Modifier,
    enabled: Boolean = true
) {
    val haptics = LocalCaptureHaptics.current
    // Scrolls sideways when a long lens list exceeds the screen width.
    Row(
        modifier.horizontalScroll(androidx.compose.foundation.rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(3.dp)
    ) {
        state.capabilities.lenses.forEach { lens ->
            val lensSelected = state.selectedLensId == lens.id
            Box(
                Modifier
                    .widthIn(min = 32.dp)
                    .height(28.dp)
                    .testTag(CaptureTestTags.lens(lens.id))
                    .then(
                        if (enabled) {
                            Modifier.captureClickable(shape = CaptureTextRippleShape) {
                                haptics.selection()
                                dispatch(SelectLens(lens.id))
                            }
                        } else {
                            Modifier
                        }
                    )
                    .semantics {
                        contentDescription = "Lens ${lens.displayName}" +
                            if (enabled) "" else " (unavailable while recording)"
                        role = Role.RadioButton
                        selected = lensSelected
                        if (!enabled) disabled()
                    },
                contentAlignment = Alignment.Center
            ) {
                Text(
                    lens.displayName,
                    color =
                        if (state.selectedLensId ==
                            lens.id
                        ) {
                            if (enabled) CaptureColors.Accent else CaptureColors.Accent.copy(alpha = .45f)
                        } else {
                            Color.White.copy(alpha = if (enabled) .38f else .18f)
                        },
                    fontFamily = CaptureMono,
                    fontWeight = FontWeight.Bold,
                    fontSize = 10.sp,
                    modifier = Modifier.uprightInLandscape(state.orientation)
                )
            }
        }
    }
}

/**
 * PHOTO | VIDEO text strip below the shutter, in the render-profile strip
 * language (bare mono text, accent = active). Shared by both modes; locked
 * (with lens selection) while a video recording is active.
 */
@Composable
internal fun ModeSelectorStrip(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {
    val haptics = LocalCaptureHaptics.current
    Row(
        modifier
            .testTag(CaptureTestTags.MODE_SELECTOR)
            .padding(horizontal = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(28.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically
    ) {
        CaptureMode.entries.forEach { mode ->
            val isSelected = state.captureMode == mode
            val label = if (mode == CaptureMode.Photo) "PHOTO" else "VIDEO"
            Box(
                Modifier
                    .height(28.dp)
                    .testTag(CaptureTestTags.captureMode(mode.name))
                    .then(
                        if (enabled) {
                            Modifier.captureClickable(shape = CaptureTextRippleShape) {
                                if (!isSelected) {
                                    haptics.selection()
                                    dispatch(SetCaptureMode(mode))
                                }
                            }
                        } else {
                            Modifier
                        }
                    )
                    .semantics {
                        contentDescription = "$label mode" + if (isSelected) ", active" else "" +
                            if (enabled) "" else " (unavailable while recording)"
                        role = Role.Tab
                        selected = isSelected
                        if (!enabled) disabled()
                    },
                contentAlignment = Alignment.Center
            ) {
                Text(
                    label,
                    color = if (isSelected) {
                        if (enabled) CaptureColors.Accent else CaptureColors.Accent.copy(alpha = .45f)
                    } else {
                        Color.White.copy(alpha = if (enabled) .38f else .18f)
                    },
                    fontFamily = CaptureMono,
                    fontWeight = FontWeight.Bold,
                    fontSize = 10.sp,
                    letterSpacing = .4.sp
                )
            }
        }
    }
}

@Composable
internal fun Shutter(dispatch: CaptureDispatch, modifier: Modifier, countingDown: Boolean = false) {
    val haptics = LocalCaptureHaptics.current
    var pressed by remember { mutableStateOf(false) }
    val pressScale by animateFloatAsState(
        targetValue = if (pressed) .94f else 1f,
        animationSpec = tween(durationMillis = 90),
        label = "shutterPressScale"
    )
    val pressAlpha by animateFloatAsState(
        targetValue = if (pressed) .6f else 1f,
        animationSpec = tween(durationMillis = 90),
        label = "shutterPressAlpha"
    )
    Box(
        modifier
            .size(CaptureDimens.ShutterOuter)
            .testTag(CaptureTestTags.SHUTTER)
            .graphicsLayer {
                scaleX = pressScale
                scaleY = pressScale
                alpha = pressAlpha
            }
            .border(2.dp, Color.White.copy(alpha = .86f), CircleShape)
            .semantics {
                role = Role.Button
                contentDescription = if (countingDown) "Cancel self timer" else "Shutter"
                onClick {
                    haptics.capture()
                    dispatch(TriggerCapture)
                    true
                }
            }.pointerInput(Unit) {
                detectTapGestures(onPress = {
                    pressed = true
                    try {
                        haptics.capture()
                        dispatch(TriggerCapture)
                        tryAwaitRelease()
                    } finally {
                        pressed = false
                    }
                })
            },
        contentAlignment = Alignment.Center
    ) {
        Box(Modifier.size(CaptureDimens.ShutterInner).background(Color(0xFFF5F5F5), CircleShape))
    }
}

@Composable
internal fun Thumbnail(state: CaptureUiState, imageUri: Uri?, onClick: () -> Unit, modifier: Modifier, onLongClick: () -> Unit = {}) {
    val context = LocalContext.current
    var thumbnail by remember(imageUri) { mutableStateOf<android.graphics.Bitmap?>(null) }
    LaunchedEffect(imageUri) {
        thumbnail =
            imageUri?.let { uri ->
                withContext(Dispatchers.IO) {
                    runCatching { context.contentResolver.loadThumbnail(uri, Size(256, 256), null) }.getOrNull()
                }
            }
    }
    val shape = RoundedCornerShape(12.dp)
    // Layout-stable loader: the outer Box is always exactly Thumbnail size.
    // Processing/failed states only change border drawing + semantics, never
    // add/remove measured children, so the MERGED pill and shutter row can't shift.
    val isProcessing =
        state.pendingCaptures.any {
            it.phase == CaptureSavePhase.DngProcessing || it.phase == CaptureSavePhase.JpegProcessing
        }
    val hasFailed =
        !isProcessing &&
            state.pendingCaptures.any {
                it.phase == CaptureSavePhase.DngFailed || it.phase == CaptureSavePhase.JpegFailed
            }
    val isSaved =
        !isProcessing &&
            !hasFailed &&
            state.pendingCaptures.any {
                it.phase == CaptureSavePhase.DngSaved || it.phase == CaptureSavePhase.JpegSaved
            }
    // Smooth state transitions: border color/width animate, glow overlays crossfade.
    val borderColor by animateColorAsState(
        targetValue =
            when {
                hasFailed -> CaptureColors.Danger
                isSaved -> CaptureColors.Success
                isProcessing -> CaptureColors.Line.copy(alpha = .35f)
                else -> CaptureColors.Line
            },
        animationSpec = tween<Color>(durationMillis = 250),
        label = "thumbnailBorderColor"
    )
    val borderWidth by animateDpAsState(
        targetValue = if (hasFailed || isProcessing || isSaved) 2.dp else 1.dp,
        animationSpec = tween<Dp>(durationMillis = 250),
        label = "thumbnailBorderWidth"
    )

    Box(
        modifier.size(CaptureDimens.Thumbnail),
        contentAlignment = Alignment.Center
    ) {
        Box(
            Modifier
                .fillMaxSize()
                .testTag(CaptureTestTags.THUMBNAIL)
                .clip(shape)
                .background(Color(0xFF65717A), shape)
                .border(width = borderWidth, color = borderColor, shape = shape)
                .captureCombinedClickable(shape = shape, onClick = { if (imageUri != null) onClick() }, onLongClickLabel = "Open Renderer", onLongClick = onLongClick)
                .semantics {
                    role = Role.Button
                    contentDescription =
                        when {
                            isProcessing -> "Processing image"
                            hasFailed -> "Last save failed"
                            isSaved -> "Image saved"
                            imageUri != null -> "Open last image"
                            else -> "No captured image yet"
                        }
                }
        ) {
            thumbnail?.let { bitmap ->
                Image(
                    bitmap = bitmap.asImageBitmap(),
                    contentDescription = null,
                    contentScale = ContentScale.Crop,
                    modifier = Modifier.fillMaxSize()
                )
            }
        }
        AnimatedVisibility(
            visible = isProcessing,
            enter = fadeIn(tween(250)),
            exit = fadeOut(tween(300)),
            modifier = Modifier.fillMaxSize()
        ) {
            ThumbnailProcessingGlow()
        }
        AnimatedVisibility(
            visible = isSaved,
            enter = fadeIn(tween(250)),
            exit = fadeOut(tween(300)),
            modifier = Modifier.fillMaxSize()
        ) {
            ThumbnailSuccessGlow()
        }
    }
}

/**
 * Traveling-highlight outline drawn around the thumbnail while a capture is
 * processing. The rounded-rect geometry stays fixed; only a bright segment
 * loops around it (marching-ant dash phase). Draw-only overlay inside the
 * fixed-size thumbnail Box, so it never affects measurement or shifts
 * surrounding controls.
 */
@Composable
private fun ThumbnailProcessingGlow() {
    val transition = rememberInfiniteTransition(label = "thumbnailProcessing")
    // Phase target is set per-frame from the measured perimeter (see below),
    // so seed the animation with a 0..1 progress and scale it in draw.
    val progress by transition.animateFloat(
        initialValue = 0f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(animation = tween<Float>(durationMillis = 1400, easing = LinearEasing)),
        label = "thumbnailProgress"
    )
    Box(
        Modifier
            .fillMaxSize()
            .testTag(CaptureTestTags.THUMBNAIL_PROCESSING)
            .drawBehind {
                // Must match the Thumbnail clip shape above.
                val corner = 12.dp.toPx()
                val rect = Rect(Offset.Zero, size)
                val path = Path().apply { addRoundRect(RoundRect(rect, CornerRadius(corner, corner))) }
                // Exact outline length so the traveling segment loops seamlessly.
                val w = size.width
                val h = size.height
                val perimeter = 2f * (w + h) - 8f * corner + 2f * kotlin.math.PI.toFloat() * corner
                val segment = perimeter * .28f
                val phase = progress * perimeter
                val march = PathEffect.dashPathEffect(floatArrayOf(segment, perimeter - segment), phase)
                val glowMarch =
                    PathEffect.dashPathEffect(floatArrayOf(segment, perimeter - segment), phase)
                // Static dim base + soft halo keep the full outline visible.
                drawPath(
                    path = path,
                    color = CaptureColors.Line.copy(alpha = .35f),
                    style = Stroke(width = 2.dp.toPx())
                )
                drawPath(
                    path = path,
                    color = CaptureColors.Accent.copy(alpha = .22f),
                    style = Stroke(width = 5.dp.toPx())
                )
                // Traveling glow tail, then the bright core on top.
                drawPath(
                    path = path,
                    color = CaptureColors.Accent.copy(alpha = .45f),
                    style = Stroke(width = 5.dp.toPx(), pathEffect = glowMarch, cap = StrokeCap.Round)
                )
                drawPath(
                    path = path,
                    color = CaptureColors.AccentSoft,
                    style = Stroke(width = 2.dp.toPx(), pathEffect = march, cap = StrokeCap.Round)
                )
            }
    )
}

/**
 * Static success glow with the same geometry as the processing glow: 2dp
 * bright core over a 5dp soft halo on the same fixed rounded-rect outline.
 * Appearance/disappearance is handled by the AnimatedVisibility crossfade at
 * the call site, so processing -> saved -> idle reads as one smooth transition.
 */
@Composable
private fun ThumbnailSuccessGlow() {
    Box(
        Modifier
            .fillMaxSize()
            .testTag(CaptureTestTags.THUMBNAIL_SUCCESS)
            .drawBehind {
                // Must match the Thumbnail clip shape above.
                val corner = 12.dp.toPx()
                val cornerRadius = CornerRadius(corner, corner)
                drawRoundRect(
                    color = CaptureColors.Success.copy(alpha = .22f),
                    cornerRadius = cornerRadius,
                    style = Stroke(width = 5.dp.toPx())
                )
                drawRoundRect(
                    color = CaptureColors.Success,
                    cornerRadius = cornerRadius,
                    style = Stroke(width = 2.dp.toPx())
                )
            }
    )
}
