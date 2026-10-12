package com.rawr.camera.renderer

import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rawr.camera.settings.architecture.ConfirmReset
import com.rawr.camera.settings.architecture.DismissReset
import com.rawr.camera.settings.architecture.NavigateBack
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.settings.model.ResetTarget
import com.rawr.camera.storage.PickFile
import com.rawr.camera.storage.launchPicker
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.ArrowDropDown
import com.rawr.camera.ui.icons.rounded.Check
import com.rawr.camera.ui.icons.rounded.Download
import com.rawr.camera.ui.icons.rounded.Image
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

private fun resetTitle(target: ResetTarget): String = when (target) {
    ResetTarget.ExposureTonality -> "Reset Exposure & Tonality?"
    ResetTarget.Color -> "Reset Color?"
    ResetTarget.Output -> "Reset Output?"
    ResetTarget.AllImageTone -> "Reset all Image / Tone?"
}

@Composable
internal fun RendererEditor(
    job: RenderJob,
    session: RendererEditorSession,
    previewSession: RendererPreviewSession,
    onSaveResolution: (RenderJob, Double) -> Unit,
    onImportLut: (String?, Uri) -> Unit,
    busy: Boolean,
    onBackToQueue: () -> Unit,
    onExport: () -> Unit
) {
    val controller by session.controller.collectAsStateWithLifecycle()
    val stagedDirty by session.dirty.collectAsStateWithLifecycle()
    val appliedValues by session.applied.collectAsStateWithLifecycle()
    var confirmDiscard by remember(job.id) { mutableStateOf(false) }
    var lutTarget by remember(session) { mutableStateOf<String?>(null) }
    val context = LocalContext.current
    val lutImporter = rememberLauncherForActivityResult(PickFile()) { uri ->
        if (uri != null) onImportLut(lutTarget, uri)
    }
    val settings by controller.state.collectAsStateWithLifecycle()
    val gatedDispatch = SettingsDispatch { if (!busy) controller.dispatch(it) }

    // In-tab back first pops settings navigation, then leaves the editor.
    // Staged Develop changes need an explicit Apply/Discard decision first.
    BackHandler {
        if (stagedDirty) {
            confirmDiscard = true
        } else {
            val destination = controller.state.value.presentation.destination
            if (!isRendererTabRoot(destination) || controller.state.value.presentation.backStack.isNotEmpty()) {
                if (!busy) controller.dispatch(NavigateBack)
            } else {
                onBackToQueue()
            }
        }
    }

    val preview by previewSession.state.collectAsStateWithLifecycle()
    var zoom by remember(job.id) { mutableFloatStateOf(1f) }
    var viewport by remember(job.id) { mutableStateOf(IntSize(1, 1)) }
    var offset by remember(job.id) { mutableStateOf(Offset.Zero) }
    LaunchedEffect(previewSession, zoom, offset, viewport) {
        previewSession.updateViewport(RendererViewport(zoom, offset.x, offset.y, viewport.width, viewport.height))
    }
    val hdFresh = preview.hdBitmap != null && preview.hdKey == job.previewKey() && !stagedDirty
    val displayBitmap = if (hdFresh) preview.hdBitmap else null

    Column(Modifier.fillMaxSize().testTag("renderer_editor")) {
        if (job.error.isNotBlank()) {
            Card(
                Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)
            ) {
                Text(job.error, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onErrorContainer, modifier = Modifier.padding(12.dp))
            }
        }
        RendererPreviewCard(
            job = job,
            bitmap = displayBitmap,
            surfaceHasFrame = preview.surfaceHasFrame,
            previewFailed = preview.failed,
            detail = preview.detail,
            previewBusy = preview.busy || preview.hdBusy,
            hdShowing = hdFresh,
            approxShowing = stagedDirty,
            zoom = zoom,
            offset = offset,
            onZoomChange = { z, o -> zoom = z; offset = o },
            onViewportChange = { viewport = it },
            onSurfaceAvailable = previewSession::surfaceAvailable,
            onSurfaceDestroyed = previewSession::surfaceDestroyed
        )
        RendererActionBar(
            job = job,
            busy = busy,
            hdBusy = preview.hdBusy,
            hdEnabled = !busy && !preview.hdBusy && !stagedDirty,
            hdShowing = hdFresh,
            onHd = previewSession::requestHd,
            onFit = { zoom = 1f; offset = Offset.Zero },
            onSaveResolution = onSaveResolution
        )
        RendererSectionTabs(settings = settings, busy = busy, dispatch = gatedDispatch)
        RendererTabContent(
            settings = settings,
            dispatch = gatedDispatch,
            busy = busy,
            onImportLut = { target -> lutTarget = target; lutImporter.launchPicker(context, arrayOf("*/*")) },
            modifier = Modifier.weight(1f)
        )
        if (stagedDirty) {
            val labels = RendererDevelopStaging.stagedLabels(appliedValues, settings.values)
            RendererStagedBar(
                summary = if (labels.isEmpty()) "Develop changes staged" else "Staged · ${labels.joinToString(", ")}",
                busy = busy,
                onDiscard = session::discard,
                onApply = {
                    session.apply()
                    previewSession.requestHd()
                }
            )
        }
        RendererExportBar(job = job, busy = busy, enabled = !stagedDirty, onExport = onExport)
    }
    if (confirmDiscard) AlertDialog(
        onDismissRequest = { confirmDiscard = false },
        title = { Text("Discard staged changes?") },
        text = { Text("Develop changes have not been applied. Discard them and leave the editor?") },
        confirmButton = { OutlinedButton(onClick = { confirmDiscard = false; session.discard(); onBackToQueue() }) { Text("Discard") } },
        dismissButton = { FilledTonalButton(onClick = { confirmDiscard = false }) { Text("Keep editing") } }
    )
    settings.presentation.pendingReset?.let { target ->
        AlertDialog(
            onDismissRequest = { gatedDispatch.invoke(DismissReset) },
            title = { Text(resetTitle(target)) },
            text = { Text("Restore the selected Image / Tone controls to their defaults?") },
            confirmButton = { OutlinedButton(onClick = { gatedDispatch.invoke(ConfirmReset(target)) }) { Text("Reset") } },
            dismissButton = { FilledTonalButton(onClick = { gatedDispatch.invoke(DismissReset) }) { Text("Cancel") } }
        )
    }
}

@Composable
private fun RendererActionBar(
    job: RenderJob,
    busy: Boolean,
    hdBusy: Boolean = false,
    hdEnabled: Boolean = false,
    hdShowing: Boolean = false,
    onHd: () -> Unit = {},
    onFit: () -> Unit,
    onSaveResolution: (RenderJob, Double) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    val (w, h) = RenderResolution.dimensions(job.width, job.height, job.megapixels)
    val resolutionLabel = if (job.megapixels == 0.0) "Original · $w × $h" else "${job.megapixels} MP · $w × $h"
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp).testTag("renderer_actionbar"),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        // All controls share the 32dp chip height so the row reads as one strip.
        OutlinedButton(
            onClick = onFit,
            contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
            modifier = Modifier.testTag("renderer_fit").height(32.dp)
        ) { Text("Fit", maxLines = 1) }
        FilterChip(
            selected = hdShowing,
            onClick = onHd,
            enabled = hdEnabled || hdShowing,
            label = { Text(if (hdBusy) "HD…" else "HD", maxLines = 1) },
            modifier = Modifier.testTag("renderer_hd").height(32.dp)
        )
        Box(Modifier.weight(1f), contentAlignment = Alignment.CenterEnd) {
            FilterChip(
                selected = true,
                onClick = { if (!busy) expanded = true },
                enabled = !busy,
                label = { Text(resolutionLabel, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                trailingIcon = { Icon(Icons.Rounded.ArrowDropDown, contentDescription = null) },
                modifier = Modifier.testTag("renderer_resolution")
            )
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                RenderResolution.choices(job.width, job.height, job.rawr).forEach { mp ->
                    val dims = RenderResolution.dimensions(job.width, job.height, mp)
                    DropdownMenuItem(
                        text = {
                            Column {
                                Text(
                                    if (mp == 0.0) "Original" else "$mp MP",
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis
                                )
                                Text(
                                    "${dims.first} × ${dims.second}",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis
                                )
                            }
                        },
                        trailingIcon = { if (mp == job.megapixels) Icon(Icons.Rounded.Check, contentDescription = null) },
                        onClick = {
                            expanded = false
                            onSaveResolution(job, mp)
                        }
                    )
                }
            }
        }
    }
}

@Composable
private fun RendererStagedBar(summary: String, busy: Boolean, onDiscard: () -> Unit, onApply: () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp).testTag("renderer_staged_bar"),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(
                summary,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSecondaryContainer,
                modifier = Modifier.weight(1f),
                maxLines = 2,
                overflow = TextOverflow.Ellipsis
            )
            OutlinedButton(onClick = onDiscard, modifier = Modifier.testTag("renderer_discard").height(32.dp)) { Text("Discard") }
            FilledTonalButton(
                enabled = !busy,
                onClick = onApply,
                modifier = Modifier.testTag("renderer_apply").height(32.dp)
            ) { Text("Apply") }
        }
    }
}

@Composable
private fun RendererExportBar(job: RenderJob, busy: Boolean, enabled: Boolean = true, onExport: () -> Unit) {
    val (w, h) = RenderResolution.dimensions(job.width, job.height, job.megapixels)
    val resolutionLabel = if (job.megapixels == 0.0) "Original · $w × $h" else "${job.megapixels} MP · $w × $h"
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp)) {
        if (!enabled) {
            Text(
                "Apply Develop changes to enable export",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(bottom = 4.dp)
            )
        }
        androidx.compose.material3.Button(
            enabled = !busy && enabled,
            onClick = onExport,
            modifier = Modifier.fillMaxWidth().height(52.dp).testTag("renderer_export")
        ) {
            Icon(Icons.Rounded.Download, contentDescription = null)
            Spacer(Modifier.size(8.dp))
            Text(
                if (busy) "Rendering…" else "Export JPEG · $resolutionLabel",
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}
