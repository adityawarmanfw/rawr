package com.rawr.camera.renderer

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.Add
import com.rawr.camera.ui.icons.rounded.DeleteOutline
import com.rawr.camera.ui.icons.rounded.Download
import com.rawr.camera.ui.icons.rounded.Image
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.SuggestionChip
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

@Composable
internal fun RendererStatusBar(loading: Boolean, busy: Boolean, progress: Int, onCancel: () -> Unit) {
    if (loading) {
        LinearProgressIndicator(Modifier.fillMaxWidth().testTag("renderer_loading"))
        Text("Preparing RAW…", style = MaterialTheme.typography.bodyMedium, modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
    }
    if (busy) {
        Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("${when { progress == 0 -> "Preparing"; progress < 45 -> "Demosaicing"; progress < 70 -> "Resizing"; progress < 75 -> "Uploading"; progress < 82 -> "Processing image"; progress < 90 -> "Rendering look"; progress < 95 -> "Finishing pixels"; progress < 99 -> "Encoding JPEG"; else -> "Saving JPEG" }} · $progress%", style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f).testTag("renderer_progress_text"))
                OutlinedButton(onClick = onCancel, modifier = Modifier.testTag("renderer_cancel")) { Text("Cancel") }
            }
            Spacer(Modifier.height(8.dp))
            LinearProgressIndicator(progress = { progress / 100f }, modifier = Modifier.fillMaxWidth())
        }
    }
}

@Composable
internal fun RendererQueue(
    jobs: List<RenderJob>,
    busy: Boolean,
    loading: Boolean,
    onImport: () -> Unit,
    onEdit: (RenderJob) -> Unit,
    onRenderOriginal: (RenderJob) -> Unit,
    onRemove: (RenderJob) -> Unit
) {
    if (jobs.isEmpty()) {
        Column(
            Modifier.fillMaxSize().padding(24.dp).testTag("renderer_empty"),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Icon(Icons.Rounded.Image, contentDescription = null, modifier = Modifier.size(48.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(12.dp))
            Text("No unfinished renders", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(4.dp))
            Text(
                "Import a DNG to edit tone, Film Sim and JPEG output, then export.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(16.dp))
            FilledTonalButton(enabled = !busy && !loading, onClick = onImport) {
                Icon(Icons.Rounded.Add, contentDescription = null)
                Spacer(Modifier.size(8.dp))
                Text("Import DNG")
            }
        }
        return
    }
    LazyColumn(
        Modifier.fillMaxSize().testTag("renderer_queue"),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        items(jobs, key = { it.id }) { item ->
            RendererJobCard(item = item, busy = busy, loading = loading, onEdit = { onEdit(item) }, onRenderOriginal = { onRenderOriginal(item) }, onRemove = { onRemove(item) })
        }
    }
}

@Composable
private fun RendererJobCard(
    item: RenderJob,
    busy: Boolean,
    loading: Boolean,
    onEdit: () -> Unit,
    onRenderOriginal: () -> Unit,
    onRemove: () -> Unit
) {
    val actionsEnabled = !busy && !loading && item.status != "Processing capture"
    Card(
        modifier = Modifier.fillMaxWidth().testTag("renderer_job_${item.id}"),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow)
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(item.name, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                RendererStatusChip(item.status)
            }
            if (item.width > 0) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    SuggestionChip(onClick = {}, label = { Text("${item.width} × ${item.height}") })
                    if (item.rawr) SuggestionChip(onClick = {}, label = { Text("RAWR") })
                }
            } else {
                Text("Source not prepared yet", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (item.error.isNotBlank()) Text(item.error, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                androidx.compose.material3.Button(enabled = actionsEnabled, onClick = onRenderOriginal) {
                    Icon(Icons.Rounded.Download, contentDescription = null)
                    Spacer(Modifier.size(8.dp))
                    Text("Render original")
                }
                FilledTonalButton(enabled = actionsEnabled, onClick = onEdit) { Text("Edit") }
                Spacer(Modifier.weight(1f))
                IconButton(enabled = actionsEnabled, onClick = onRemove) {
                    Icon(Icons.Rounded.DeleteOutline, contentDescription = "Remove ${item.name}")
                }
            }
        }
    }
}

@Composable
private fun RendererStatusChip(status: String) {
    val (label, container, content) = when (status) {
        "Processing capture" -> Triple("Processing", MaterialTheme.colorScheme.surfaceContainerHighest, MaterialTheme.colorScheme.onSurfaceVariant)
        "Preparing RAW" -> Triple("Preparing", MaterialTheme.colorScheme.surfaceContainerHighest, MaterialTheme.colorScheme.onSurfaceVariant)
        "Rendering", "Publishing" -> Triple(status, MaterialTheme.colorScheme.primaryContainer, MaterialTheme.colorScheme.onPrimaryContainer)
        "Failed" -> Triple("Failed", MaterialTheme.colorScheme.errorContainer, MaterialTheme.colorScheme.onErrorContainer)
        "Complete" -> Triple("Complete", MaterialTheme.colorScheme.secondaryContainer, MaterialTheme.colorScheme.onSecondaryContainer)
        else -> Triple(status.ifBlank { "Editing" }, MaterialTheme.colorScheme.surfaceContainerHighest, MaterialTheme.colorScheme.onSurfaceVariant)
    }
    AssistChip(
        onClick = {},
        enabled = false,
        label = { Text(label) },
        colors = AssistChipDefaults.assistChipColors(containerColor = container, labelColor = content, disabledContainerColor = container, disabledLabelColor = content)
    )
}
