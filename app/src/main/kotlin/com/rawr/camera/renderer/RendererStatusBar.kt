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
