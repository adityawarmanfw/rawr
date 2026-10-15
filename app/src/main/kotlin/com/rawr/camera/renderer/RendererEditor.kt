package com.rawr.camera.renderer

import androidx.compose.ui.draw.rotate
import com.rawr.camera.ui.icons.rounded.ChevronRight
import android.net.Uri
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.Info
import com.rawr.camera.ui.CaptureColors
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.storage.PickFile
import com.rawr.camera.storage.launchPicker

@Composable
internal fun RendererEditor(job: RenderJob, session: RendererEditorSession, previewSession: RendererPreviewSession,
    onSaveResolution: (RenderJob,Double)->Unit, onImportLut: (String?,Uri)->Unit,
    busy: Boolean, onBackToQueue: ()->Unit, onExport: ()->Unit,
    onShadowLift: (RenderJob, com.rawr.camera.settings.model.AutoKeyExposure)->Unit = { _, _ -> },
    onLocalTone: (RenderJob, Float)->Unit = { _, _ -> }) {
    val controller by session.controller.collectAsStateWithLifecycle()
    val settings by controller.state.collectAsStateWithLifecycle()
    val preview by previewSession.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    var lutTarget by remember { mutableStateOf<String?>(null) }
    val importer = rememberLauncherForActivityResult(PickFile()) { uri -> if (uri != null) onImportLut(lutTarget, uri) }
    RendererEditorContent(job, settings.values, SettingsDispatch { if (!busy) controller.dispatch(it) }, preview,
        busy, onBackToQueue, onExport, previewSession::updateViewport, previewSession::setWaveformEnabled,
        onSaveResolution = { onSaveResolution(job, it) },
        onImportLut = { lutTarget = it; importer.launchPicker(context, arrayOf("*/*")) },
        metadata = { dismiss -> RendererMetadata(job, previewSession, dismiss) },
        onShadowLift = { if (!busy) onShadowLift(job, it) },
        onLocalTone = { if (!busy) onLocalTone(job, it) })
}

/** UI-only editor. Tests drive this exact layout without native rendering or job mutation. */
@Composable
internal fun RendererEditorContent(
    job: RenderJob, values: com.rawr.camera.settings.model.SettingsValues, dispatch: SettingsDispatch,
    preview: RendererPreviewState, busy: Boolean, onBackToQueue: () -> Unit, onExport: () -> Unit,
    onViewport: (RendererViewport) -> Unit, onWaveform: (Boolean) -> Unit,
    onSaveResolution: (Double) -> Unit, onImportLut: (String?) -> Unit,
    metadata: @Composable (() -> Unit) -> Unit,
    onShadowLift: (com.rawr.camera.settings.model.AutoKeyExposure) -> Unit = {},
    onLocalTone: (Float) -> Unit = {}
) {
    var waveformVisible by remember(job.id) { mutableStateOf(false) }
    var metadataVisible by remember(job.id) { mutableStateOf(false) }
    val waveform = preview.frame?.waveform
    var stage by remember(job.id){mutableStateOf(RendererStage.RENDER)}
    var pane by remember(job.id){mutableIntStateOf(1)}
    val waveformShown = waveformVisible && pane != 2
    LaunchedEffect(waveformShown) { onWaveform(waveformShown) }
    var paneFraction by remember(job.id){mutableFloatStateOf(.45f)}
    var sideFraction by remember(job.id){mutableFloatStateOf(.38f)}
    val density=LocalDensity.current
    var zoom by remember(job.id){mutableFloatStateOf(1f)}
    var offset by remember(job.id){mutableStateOf(Offset.Zero)}
    var canvasSize by remember(job.id){mutableStateOf(IntSize(1,1))}
    var viewport by remember(job.id){mutableStateOf(IntSize(1,1))}
    BackHandler(onBack = onBackToQueue)
    LaunchedEffect(zoom, offset, viewport, canvasSize) {
        onViewport(RendererViewport(zoom, offset.x, offset.y, viewport.width, viewport.height, canvasSize.width, canvasSize.height))
    }
    val selectedStage = stage
    val view = RendererViewport(zoom, offset.x, offset.y, viewport.width, viewport.height, canvasSize.width, canvasSize.height)
    Column(Modifier.fillMaxSize().testTag("renderer_editor")) {
        RendererToolbar(pane, busy, waveformVisible, metadataVisible, onBackToQueue,
            onFit = { zoom = 1f; offset = Offset.Zero },
            onTogglePane = { pane = if (pane == 0) 1 else 0 }, onExport = onExport,
            onWaveform = { waveformVisible = !waveformVisible },
            onMetadata = { metadataVisible = !metadataVisible })
        if (metadataVisible) metadata { metadataVisible = false }
        if (job.error.isNotBlank()) Text(job.error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        RendererWorkspace(
            modifier = Modifier.weight(1f).fillMaxWidth(),
            pane = pane, paneFraction = paneFraction, sideFraction = sideFraction,
            onSideResize = { sideFraction = it },
            canvas = { modifier ->
                RendererCanvas(modifier, job, preview, view, waveform.takeIf { waveformShown }, waveformShown,
                    onZoomChange = { z, o -> zoom = z; offset = o },
                    onViewportChange = { viewport = it }, onCanvasChange = { canvasSize = it })
            },
            controls = { modifier, height, landscape ->
                val paneDrag = if (landscape) Modifier else Modifier.pointerInput(height) {
                    detectVerticalDragGestures(onDragEnd = {
                        pane = if (paneFraction < .2f) 0 else if (paneFraction > .58f) 2 else 1
                        paneFraction = if (pane == 2) .72f else .45f
                    }) { event, dy ->
                        event.consume()
                        paneFraction = (paneFraction - dy / with(density) { height.toPx() }).coerceIn(.1f, .85f)
                    }
                }
                RendererControlPane(modifier, paneDrag, selectedStage, pane, landscape,
                    onStage = { stage = it },
                    onExpand = {
                        pane = if (pane == 1) 2 else 1
                        paneFraction = if (pane == 2) .72f else .45f
                        sideFraction = if (pane == 2) .55f else .38f
                    }) {
                    RendererControls(selectedStage, values, dispatch, job, onSaveResolution, onImportLut, onShadowLift, onLocalTone)
                }
            })
    }
}

/** Explicit sibling layout: toolbar, image, pane header, and scroll area have
 * disjoint bounds. No native View or pointer handler spans these regions.
 */
@Composable
private fun RendererWorkspace(
    modifier: Modifier, pane: Int, paneFraction: Float, sideFraction: Float,
    onSideResize: (Float) -> Unit,
    canvas: @Composable (Modifier) -> Unit,
    controls: @Composable (Modifier, androidx.compose.ui.unit.Dp, Boolean) -> Unit
) {
    val density = LocalDensity.current
    val latestFraction by rememberUpdatedState(sideFraction)
    val latestResize by rememberUpdatedState(onSideResize)
    var workspaceSize by remember { mutableStateOf(IntSize.Zero) }
    val landscape = workspaceSize.width > workspaceSize.height
    val height = with(density) { workspaceSize.height.toDp() }
    // Keep editor state in ordinary composition. Measurement only allocates sibling
    // bounds; it never owns/subcomposes the tab header or the selected control page.
    Layout(modifier = modifier.clipEditorViewport().onSizeChanged { workspaceSize = it }, content = {
        canvas(Modifier.fillMaxSize())
        if (pane > 0) {
            Box(Modifier.fillMaxSize().pointerInput(workspaceSize.width) {
                detectHorizontalDragGestures { event, dx ->
                    event.consume()
                    latestResize((latestFraction - dx / workspaceSize.width.coerceAtLeast(1)).coerceIn(.28f, .65f))
                }
            })
            controls(Modifier.fillMaxSize(), height, landscape)
        }
    }) { measurables, constraints ->
        val widthPx = constraints.maxWidth
        val heightPx = constraints.maxHeight
        val horizontal = widthPx > heightPx
        val divider = if (pane > 0 && horizontal) 8.dp.roundToPx() else 0
        val controlWidth = if (pane == 0) 0 else if (horizontal)
            (widthPx * sideFraction).toInt().coerceAtMost(520.dp.roundToPx()) else widthPx
        val controlHeight = if (pane == 0) 0 else if (horizontal) heightPx else (heightPx * paneFraction).toInt()
        val previewWidth = if (horizontal) (widthPx - controlWidth - divider).coerceAtLeast(0) else widthPx
        val previewHeight = if (horizontal) heightPx else heightPx - controlHeight
        val image = measurables[0].measure(Constraints.fixed(previewWidth, previewHeight))
        val separator = if (pane > 0) measurables[1].measure(Constraints.fixed(divider, if (horizontal) heightPx else 0)) else null
        val editor = if (pane > 0) measurables[2].measure(Constraints.fixed(controlWidth, controlHeight)) else null
        layout(widthPx, heightPx) {
            image.placeRelative(0, 0)
            separator?.placeRelative(previewWidth, 0)
            editor?.placeRelative(if (horizontal) previewWidth + divider else 0, if (horizontal) 0 else previewHeight)
        }
    }
}

@Composable
private fun RendererCanvas(
    modifier: Modifier, job: RenderJob, preview: RendererPreviewState, view: RendererViewport,
    waveform: android.graphics.Bitmap?, waveformShown: Boolean,
    onZoomChange: (Float, Offset) -> Unit, onViewportChange: (IntSize) -> Unit, onCanvasChange: (IntSize) -> Unit
) {
    Box(modifier.clipEditorViewport()) {
        RendererPreviewCard(job, preview.frame?.bitmap, preview.failed,
            preview.frame?.tiles.orEmpty(), view.zoom, Offset(view.offsetX, view.offsetY),
            onZoomChange, onViewportChange, onCanvasChange)
        if (preview.activity.isNotBlank()) Text(preview.activity, style = EditorHeadingStyle,
            color = CaptureColors.AccentSoft, maxLines = 1,
            modifier = Modifier.align(Alignment.TopStart).padding(8.dp)
                .background(CaptureColors.Background.copy(alpha = .8f), RoundedCornerShape(4.dp))
                .padding(horizontal = 8.dp, vertical = 4.dp))
        if (waveformShown) RendererWaveform(waveform, Modifier.align(Alignment.BottomEnd).padding(end = 10.dp, bottom = 10.dp))
    }
}

@Composable
private fun RendererToolbar(pane: Int, busy: Boolean, waveform: Boolean, metadata: Boolean,
    onBack: () -> Unit, onFit: () -> Unit, onTogglePane: () -> Unit, onExport: () -> Unit,
    onWaveform: () -> Unit, onMetadata: () -> Unit) {
    Row(Modifier.fillMaxWidth().height(48.dp).background(CaptureColors.Background),
        verticalAlignment = Alignment.CenterVertically) {
        TextButton(onClick = onBack, contentPadding = PaddingValues(horizontal = 8.dp),
            modifier = Modifier.weight(1f).testTag("renderer_editor_back")) { Text("BACK", style = EditorHeadingStyle) }
        TextButton(onClick = onFit, contentPadding = PaddingValues(horizontal = 8.dp),
            modifier = Modifier.weight(.8f).testTag("renderer_fit")) { Text("FIT", style = EditorHeadingStyle) }
        EditorToolToggle("Waveform", waveform, onWaveform) {
            Canvas(Modifier.size(20.dp)) {
                val levels = floatArrayOf(.25f, .5f, .8f, .4f, .65f, .95f, .5f)
                levels.forEachIndexed { i, level ->
                    val x = size.width * (i + .5f) / levels.size
                    drawLine(it, Offset(x, size.height * (1f - level)), Offset(x, size.height), 1.5.dp.toPx())
                }
            }
        }
        EditorToolToggle("Metadata", metadata, onMetadata) {
            Icon(Icons.Rounded.Info, contentDescription = null, tint = it, modifier = Modifier.size(20.dp))
        }
        Spacer(Modifier.weight(.2f))
        TextButton(onClick = onTogglePane, contentPadding = PaddingValues(horizontal = 8.dp),
            modifier = Modifier.weight(1f).testTag("renderer_toggle_pane")) { Text(if (pane == 0) "EDIT" else "HIDE", style = EditorHeadingStyle) }
        TextButton(enabled = !busy, onClick = onExport, contentPadding = PaddingValues(horizontal = 8.dp),
            modifier = Modifier.weight(1.3f)) { Text(if (busy) "BUSY" else "EXPORT", style = EditorHeadingStyle) }
    }
}

@Composable
private fun RendererControlPane(modifier: Modifier, drag: Modifier, stage: RendererStage, pane: Int, landscape: Boolean,
    onStage: (RendererStage) -> Unit, onExpand: () -> Unit, content: @Composable () -> Unit) {
    val pages = rememberSaveableStateHolder()
    Column(modifier.clipEditorViewport().background(CaptureColors.Panel)) {
        Box(Modifier.fillMaxWidth().height(20.dp).then(drag), contentAlignment = Alignment.Center) {
            Box(Modifier.size(28.dp, 2.dp).background(CaptureColors.StrongLine, RoundedCornerShape(2.dp)))
        }
        Row(Modifier.fillMaxWidth().height(48.dp).selectableGroup(), verticalAlignment = Alignment.CenterVertically) {
            RendererStage.entries.forEach { item ->
                val selected = stage == item
                Box(Modifier.weight(1f).fillMaxHeight().testTag("renderer_stage_${item.name}")
                    .selectable(selected = selected, role = Role.Tab, onClick = { onStage(item) }),
                    contentAlignment = Alignment.Center) {
                    Text(item.name, style = EditorHeadingStyle,
                        color = if (selected) CaptureColors.AccentSoft else Color.White.copy(alpha = .46f))
                    if (selected) Box(Modifier.align(Alignment.BottomCenter).padding(horizontal = 12.dp)
                        .fillMaxWidth().height(2.dp).background(CaptureColors.Accent))
                }
            }
            Box(Modifier.width(36.dp).fillMaxHeight().testTag("renderer_expand_pane").clickable(onClick = onExpand), contentAlignment = Alignment.Center) {
                Icon(com.rawr.camera.ui.icons.Icons.Rounded.ChevronRight, if (pane == 1) "Expand" else "Collapse",
                    tint = CaptureColors.AccentSoft, // The pane grows up in portrait and leftward from the right edge in landscape.
                    modifier = Modifier.size(20.dp).rotate(when {
                        landscape -> if (pane == 1) 180f else 0f
                        else -> if (pane == 1) -90f else 90f
                    }))
            }
        }
        HorizontalDivider(color = CaptureColors.Line)
        // Saved state is retained outside the page identity. Replacing the entire
        // clipped viewport prevents cross-stage LayoutNode/graphics-layer reuse;
        // the tab highlight and page must never display different stages.
        key(stage) {
            Box(Modifier.weight(1f).fillMaxWidth().clipEditorViewport()) {
                pages.SaveableStateProvider(stage) { content() }
            }
        }
    }
}

@Composable
private fun EditorToolToggle(label: String, active: Boolean, onClick: () -> Unit,
    icon: @Composable (Color) -> Unit) {
    // Small glyphs retain a full touch target and an accessible selected state.
    Box(Modifier.size(48.dp).testTag("renderer_tool_${label.lowercase()}")
        .semantics { contentDescription = label }
        .selectable(active, role = Role.Checkbox, onClick = onClick), contentAlignment = Alignment.Center) {
        Box(Modifier.size(32.dp).background(
            if (active) CaptureColors.Accent.copy(alpha = .18f) else Color.Transparent,
            RoundedCornerShape(6.dp)), contentAlignment = Alignment.Center) {
            icon(if (active) CaptureColors.AccentSoft else Color.White.copy(alpha = .55f))
        }
    }
}
