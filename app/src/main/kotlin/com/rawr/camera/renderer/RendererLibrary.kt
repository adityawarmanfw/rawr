package com.rawr.camera.renderer

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Bitmap
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rawr.camera.renderer.library.DngConversion
import com.rawr.camera.renderer.library.DngConvertQueue
import com.rawr.camera.renderer.library.DngConvertState
import com.rawr.camera.renderer.library.DngConvertTask
import com.rawr.camera.renderer.library.DngEntry
import com.rawr.camera.renderer.library.DngLibrary
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.Check
import com.rawr.camera.ui.icons.rounded.ArrowDropDown
import com.rawr.camera.ui.icons.rounded.DeleteOutline
import com.rawr.camera.ui.icons.rounded.Download
import com.rawr.camera.ui.icons.rounded.Edit
import com.rawr.camera.ui.icons.rounded.Image

/** Library order; persisted per install. */
internal enum class LibrarySort(val label: String, val comparator: Comparator<DngEntry>) {
    Newest("Newest first", compareByDescending<DngEntry> { it.taken }.thenByDescending { it.id }),
    Oldest("Oldest first", compareBy<DngEntry> { it.taken }.thenBy { it.id }),
    Name("Name", compareBy(String.CASE_INSENSITIVE_ORDER) { it.name }),
    Largest("Largest file", compareByDescending<DngEntry> { it.size }),
    Resolution("Most megapixels", compareByDescending<DngEntry> { it.width.toLong() * it.height }.thenByDescending { it.taken })
}

/** Library tabs; each is a filter over the same scan. */
internal enum class LibraryTab(val label: String) {
    All("All"),
    Rawr("Rawr"),  // Rawr DNGs and unfinished renders
    Other("Other"),
    HighRes("High Res");

    fun accepts(entry: DngEntry): Boolean = when (this) {
        All -> true
        Rawr -> entry.rawr
        Other -> !entry.rawr
        HighRes -> entry.highRes
    }

    /** Unfinished renders are listed on the tabs where Rawr's own work shows. */
    val showsJobs: Boolean get() = this == All || this == Rawr
}

/**
 * Renderer home: unfinished renders pinned on top, then every DNG on the
 * device (auto-scanned) with thumbnails, filtered by tab. Long-press selects
 * files for bulk conversion.
 */
@OptIn(ExperimentalFoundationApi::class, androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
internal fun RendererLibrary(
    jobs: List<RenderJob>,
    busy: Boolean,
    loading: Boolean,
    onImport: () -> Unit,
    onOpen: (DngEntry) -> Unit,
    onRenderAll: (List<DngEntry>) -> Unit,
    bulk: BulkRenderState,
    onCancelBulk: () -> Unit,
    onDismissBulk: () -> Unit,
    onEdit: (RenderJob) -> Unit,
    onRenderOriginal: (RenderJob) -> Unit,
    onRemove: (RenderJob) -> Unit
) {
    val context = LocalContext.current
    val library = remember { DngLibrary.get(context) }
    val queue = remember { DngConvertQueue.get(context) }
    val entries by library.entries.collectAsStateWithLifecycle()
    val scanning by library.scanning.collectAsStateWithLifecycle()
    val convert by queue.state.collectAsStateWithLifecycle()
    var tab by rememberSaveable { mutableStateOf(LibraryTab.All) }
    val prefs = remember { context.getSharedPreferences("dng_library_ui", android.content.Context.MODE_PRIVATE) }
    var sort by remember {
        mutableStateOf(runCatching { LibrarySort.valueOf(prefs.getString("sort", null).orEmpty()) }.getOrDefault(LibrarySort.Newest))
    }
    val deleteOriginals = rememberLauncherForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) { result ->
        if (result.resultCode == android.app.Activity.RESULT_OK) {
            queue.dismissResults()
            library.requestScan()
        }
    }
    var selected by remember { mutableStateOf(emptySet<Long>()) }
    var access by remember { mutableStateOf(mediaAccess(context)) }
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        access = mediaAccess(context)
        if (access != MediaAccess.None) library.watch()
    }
    LaunchedEffect(access) {
        if (access != MediaAccess.None) {
            library.watch()
            queue.resume()
        }
    }
    val visible = entries.filter(tab::accepts).sortedWith(sort.comparator)
    val selection = entries.filter { it.id in selected }

    Column(Modifier.fillMaxSize().testTag("renderer_library")) {
        PrimaryTabRow(selectedTabIndex = tab.ordinal) {
            LibraryTab.entries.forEach { candidate ->
                val count = entries.count(candidate::accepts) + if (candidate.showsJobs) jobs.size else 0
                Tab(selected = tab == candidate, onClick = { tab = candidate },
                    text = {
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            Text(candidate.label, maxLines = 1)
                            Text("$count", style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    })
            }
        }
        // Fixed slot: rescans run after every published conversion, and an
        // indicator that came and went would nudge the cards below it.
        Box(Modifier.fillMaxWidth().height(2.dp)) {
            if (scanning) LinearProgressIndicator(Modifier.fillMaxSize())
        }
        ConvertStatus(
            convert,
            deletable = if (convert.running) 0 else queue.deletableOriginals().size,
            onCancel = queue::cancelAll,
            onDismiss = queue::dismissResults,
            onDeleteOriginals = {
                val uris = queue.deletableOriginals().map { it.uri }
                if (uris.isNotEmpty()) deleteOriginals.launch(
                    androidx.activity.result.IntentSenderRequest.Builder(
                        android.provider.MediaStore.createDeleteRequest(context.contentResolver, uris).intentSender
                    ).build()
                )
            }
        )
        BulkRenderStatus(bulk, onCancel = onCancelBulk, onDismiss = onDismissBulk)
        Box(Modifier.weight(1f)) {
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                if (access != MediaAccess.Full) item(key = "access") {
                    AccessCard(access) {
                        permission.launch(arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED))
                    }
                }
                if (tab.showsJobs) items(jobs, key = { "job_${it.id}" }) { job ->
                    JobRow(job = job, enabled = !busy && !loading && job.status != "Processing capture",
                        onEdit = { onEdit(job) }, onRenderOriginal = { onRenderOriginal(job) }, onRemove = { onRemove(job) })
                }
                item(key = "toolbar") {
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        SortMenu(sort) {
                            sort = it
                            prefs.edit().putString("sort", it.name).apply()
                        }
                        Spacer(Modifier.weight(1f))
                        if (tab == LibraryTab.HighRes && visible.isNotEmpty()) TextButton(onClick = {
                            selected = visible.filter { it.convertible && it.converted.isEmpty() }.map { it.id }.toSet()
                        }) { Text("Select unconverted") }
                    }
                }
                items(visible, key = { "dng_${it.id}" }) { entry ->
                    val queued = convert.current?.entry?.id == entry.id || convert.pending.any { it.entry.id == entry.id }
                    LibraryRow(
                        entry = entry,
                        library = library,
                        selecting = selected.isNotEmpty(),
                        selected = entry.id in selected,
                        queued = queued,
                        enabled = !busy && !loading,
                        onClick = {
                            if (selected.isNotEmpty()) selected = selected.toggle(entry.id) else onOpen(entry)
                        },
                        onLongClick = { selected = selected.toggle(entry.id) }
                    )
                }
                if (visible.isEmpty() && !(tab.showsJobs && jobs.isNotEmpty())) item(key = "empty") {
                    EmptyLibrary(tab, scanning, onImport)
                }
            }
        }
        if (selection.isNotEmpty()) ConvertBar(
            selection = selection,
            renderEnabled = !busy && !loading && !bulk.running,
            onRender = {
                onRenderAll(selection.filter { it.error.isBlank() })
                selected = emptySet()
            },
            onClear = { selected = emptySet() },
            onConvert = { conversion ->
                queue.enqueue(selection.filter { it.convertible }.map { DngConvertTask(it, conversion) })
                selected = emptySet()
            }
        )
    }
}

private fun Set<Long>.toggle(id: Long) = if (id in this) this - id else this + id

private enum class MediaAccess { None, Partial, Full }

private fun mediaAccess(context: android.content.Context): MediaAccess {
    fun granted(p: String) = ContextCompat.checkSelfPermission(context, p) == PackageManager.PERMISSION_GRANTED
    return when {
        granted(Manifest.permission.READ_MEDIA_IMAGES) -> MediaAccess.Full
        granted(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED) -> MediaAccess.Partial
        else -> MediaAccess.None
    }
}

@Composable
private fun AccessCard(access: MediaAccess, onRequest: () -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)) {
        Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (access == MediaAccess.None) "Allow photo access to list the DNGs on this phone."
                else "Only selected photos are shared with Rawr.",
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.weight(1f)
            )
            FilledTonalButton(onClick = onRequest) { Text(if (access == MediaAccess.None) "Allow" else "Select more") }
        }
    }
}

/** Unfinished render in the library's compact row style. */
@Composable
private fun JobRow(job: RenderJob, enabled: Boolean, onEdit: () -> Unit, onRenderOriginal: () -> Unit, onRemove: () -> Unit) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surfaceContainer,
        modifier = Modifier.fillMaxWidth().testTag("renderer_job_${job.id}")
    ) {
        Row(Modifier.padding(start = 12.dp, end = 4.dp, top = 6.dp, bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(job.name, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f, fill = false))
                    Badge(job.status.ifBlank { "Editing" })
                }
                Text(
                    buildList {
                        if (job.width > 0) add("${job.width}×${job.height}") else add("Source not prepared yet")
                        if (job.rawr) add("Rawr")
                        if (job.error.isNotBlank()) add(job.error)
                    }.joinToString(" · "),
                    style = MaterialTheme.typography.bodySmall,
                    color = if (job.error.isNotBlank()) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1, overflow = TextOverflow.Ellipsis
                )
            }
            androidx.compose.material3.IconButton(enabled = enabled, onClick = onRenderOriginal) {
                Icon(Icons.Rounded.Download, contentDescription = "Render ${job.name}")
            }
            androidx.compose.material3.IconButton(enabled = enabled, onClick = onEdit) {
                Icon(Icons.Rounded.Edit, contentDescription = "Edit ${job.name}")
            }
            androidx.compose.material3.IconButton(enabled = enabled, onClick = onRemove) {
                Icon(Icons.Rounded.DeleteOutline, contentDescription = "Remove ${job.name}")
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun LibraryRow(
    entry: DngEntry,
    library: DngLibrary,
    selecting: Boolean,
    selected: Boolean,
    queued: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
    onLongClick: () -> Unit
) {
    // Starts as soon as the row is visible; the thumbnail reads its own orientation.
    val thumbnail by produceState<Bitmap?>(null, entry.id, entry.modified, entry.error.isBlank()) {
        value = if (entry.error.isBlank()) library.thumbnail(entry) else null
    }
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = if (selected) MaterialTheme.colorScheme.secondaryContainer else MaterialTheme.colorScheme.surfaceContainerLow,
        modifier = Modifier.fillMaxWidth().testTag("library_${entry.id}")
            .clip(RoundedCornerShape(12.dp))
            .combinedClickable(enabled = enabled || selecting, onClick = onClick, onLongClick = onLongClick)
    ) {
        Row(Modifier.padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(48.dp).clip(RoundedCornerShape(8.dp)).background(MaterialTheme.colorScheme.surfaceVariant),
                contentAlignment = Alignment.Center
            ) {
                thumbnail?.let {
                    Image(it.asImageBitmap(), contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
                } ?: Icon(Icons.Rounded.Image, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(entry.name, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f, fill = false))
                    if (entry.rawr) Badge("RAWR")
                    if (entry.conversionOutput) Badge("Converted")
                }
                Text(details(entry), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (entry.highRes || queued) Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    if (queued) Badge("Queued")
                    entry.converted.sortedDescending().forEach { mp -> Badge("${mp}MP", done = true) }
                }
            }
            if (selecting) Checkbox(checked = selected, onCheckedChange = { onClick() })
        }
    }
}

private fun details(entry: DngEntry): String = buildList {
    if (entry.width > 0) add("${entry.width}×${entry.height}")
    add("%.0f MB".format(entry.size / 1e6))
    if (entry.model.isNotBlank()) add(entry.model)
    if (entry.error.isNotBlank()) add(entry.error)
}.joinToString(" · ")

@Composable
private fun Badge(text: String, done: Boolean = false) {
    Row(
        Modifier.padding(start = 4.dp, top = 2.dp)
            .background(
                if (done) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceContainerHighest,
                RoundedCornerShape(6.dp)
            )
            .padding(horizontal = 6.dp, vertical = 1.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        if (done) Icon(Icons.Rounded.Check, contentDescription = null, modifier = Modifier.size(12.dp),
            tint = MaterialTheme.colorScheme.onPrimaryContainer)
        Text(text, style = MaterialTheme.typography.labelSmall,
            color = if (done) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun EmptyLibrary(tab: LibraryTab, scanning: Boolean, onImport: () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            when {
                scanning -> "Looking for DNGs…"
                tab == LibraryTab.HighRes -> "No DNGs of 150MP or more"
                else -> "No DNGs here"
            },
            style = MaterialTheme.typography.titleMedium
        )
        Spacer(Modifier.height(12.dp))
        OutlinedButton(onClick = onImport) { Text("Import DNG") }
    }
}

@Composable
private fun ConvertStatus(
    state: DngConvertState,
    deletable: Int,
    onCancel: () -> Unit,
    onDismiss: () -> Unit,
    onDeleteOriginals: () -> Unit
) {
    if (!state.running && state.done == 0 && state.failed.isEmpty()) return
    Card(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh)) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            if (state.running) {
                val position = (state.done + state.failed.size + 1).coerceAtMost(state.total)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("Converting $position of ${state.total} · ${state.current?.conversion?.let { c ->
                            state.current.let { "${c.outputMegapixels(it.entry)}MP" }
                        }}", style = MaterialTheme.typography.bodyMedium)
                        Text(state.current?.entry?.name.orEmpty(), style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    OutlinedButton(onClick = onCancel) { Text("Cancel") }
                }
                LinearProgressIndicator(progress = { state.permille / 1000f }, modifier = Modifier.fillMaxWidth())
                Text("You can leave this screen; conversion continues.", style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        buildString {
                            append("Converted ${state.done}")
                            if (state.failed.isNotEmpty()) append(", ${state.failed.size} failed")
                        },
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.weight(1f)
                    )
                    if (deletable > 0) TextButton(onClick = onDeleteOriginals) {
                        Text(if (deletable == 1) "Delete original" else "Delete $deletable originals")
                    }
                    TextButton(onClick = onDismiss) { Text("OK") }
                }
                state.failed.forEach { (task, reason) ->
                    Text("${task.entry.name}: $reason", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
                }
            }
        }
    }
}

@Composable
private fun ConvertBar(
    selection: List<DngEntry>,
    renderEnabled: Boolean,
    onRender: () -> Unit,
    onClear: () -> Unit,
    onConvert: (DngConversion) -> Unit
) {
    val reference = selection.maxByOrNull { it.megapixels }
    val convertible = selection.count { it.convertible }
    Surface(color = MaterialTheme.colorScheme.surfaceContainerHigh, tonalElevation = 3.dp) {
        Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    if (convertible == selection.size) "${selection.size} selected" else "${selection.size} selected · $convertible convertible",
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.weight(1f)
                )
                FilledTonalButton(enabled = renderEnabled, onClick = onRender, contentPadding = PaddingValues(horizontal = 12.dp),
                    modifier = Modifier.height(36.dp)) {
                    Icon(Icons.Rounded.Download, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text("Render JPEG")
                }
                TextButton(onClick = onClear) { Text("Clear") }
            }
            Text("Convert to DNG", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp, bottom = 4.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                DngConversion.entries.forEach { conversion ->
                    FilledTonalButton(
                        enabled = convertible > 0,
                        onClick = { onConvert(conversion) },
                        contentPadding = PaddingValues(horizontal = 8.dp),
                        modifier = Modifier.weight(1f).height(36.dp)
                    ) {
                        Text(
                            reference?.takeIf { it.width > 0 }?.let { "${conversion.outputMegapixels(it)}MP" } ?: conversion.label,
                            style = MaterialTheme.typography.labelMedium,
                            maxLines = 1
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun SortMenu(sort: LibrarySort, onSort: (LibrarySort) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        TextButton(onClick = { open = true }) {
            Text(sort.label)
            Icon(Icons.Rounded.ArrowDropDown, contentDescription = "Sort order")
        }
        androidx.compose.material3.DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            LibrarySort.entries.forEach { option ->
                androidx.compose.material3.DropdownMenuItem(
                    text = { Text(option.label) },
                    trailingIcon = { if (option == sort) Icon(Icons.Rounded.Check, contentDescription = null) },
                    onClick = {
                        open = false
                        onSort(option)
                    }
                )
            }
        }
    }
}

@Composable
private fun BulkRenderStatus(state: BulkRenderState, onCancel: () -> Unit, onDismiss: () -> Unit) {
    if (state.total == 0) return
    Card(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh)) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(
                        if (state.running) "Rendering ${(state.done + state.failed.size + 1).coerceAtMost(state.total)} of ${state.total}"
                        else buildString {
                            append("Rendered ${state.done} JPEG")
                            if (state.done != 1) append("s")
                            if (state.failed.isNotEmpty()) append(", ${state.failed.size} failed")
                        },
                        style = MaterialTheme.typography.bodyMedium
                    )
                    state.current?.let {
                        Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                }
                if (state.running) OutlinedButton(onClick = onCancel) { Text("Cancel") }
                else TextButton(onClick = onDismiss) { Text("OK") }
            }
            state.failed.forEach {
                Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            }
        }
    }
}
