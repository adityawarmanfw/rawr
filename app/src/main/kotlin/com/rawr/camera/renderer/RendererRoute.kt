package com.rawr.camera.renderer

import android.Manifest
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rawr.camera.storage.PickFile
import com.rawr.camera.storage.launchPicker
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.automirrored.rounded.ArrowBack
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun RendererRoute(viewModel: RendererViewModel, onExit: () -> Unit) {
    val store = viewModel.store
    val snackbar = remember { SnackbarHostState() }
    val jobs by store.jobs.collectAsStateWithLifecycle()
    val busy by store.busy.collectAsStateWithLifecycle()
    val progress by store.progress.collectAsStateWithLifecycle()
    val state by viewModel.state.collectAsStateWithLifecycle()
    val session by viewModel.editor.collectAsStateWithLifecycle()
    val preview by viewModel.preview.collectAsStateWithLifecycle()
    val loading = state.loading
    val removing = state.removing
    val cancelBack = state.confirmCancel
    val job = jobs.firstOrNull { it.id == state.selectedId }
    LaunchedEffect(viewModel) {
        viewModel.effects.collect { effect ->
            when (effect) {
                is RendererEffect.Message -> snackbar.showSnackbar(effect.text)
                RendererEffect.Exit -> onExit()
            }
        }
    }
    val notificationPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
    val context = LocalContext.current
    val importer = rememberLauncherForActivityResult(PickFile()) { uri ->
        uri?.let(viewModel::import)
    }
    val exit: () -> Unit = viewModel::back
    BackHandler(onBack = exit)
    Scaffold(
        modifier = Modifier.testTag("renderer_screen"),
        snackbarHost = { SnackbarHost(snackbar) },
        topBar = {
            if (job == null || job.width <= 0 || session == null || preview == null) TopAppBar(
                title = {
                    Column {
                        Text("Renderer")
                        Text(
                            if (job != null) job.name else if (jobs.isEmpty()) "DNG library" else "${jobs.size} unfinished",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                },
                navigationIcon = {
                    IconButton(onClick = exit, modifier = Modifier.testTag("renderer_back")) {
                        Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    FilledTonalButton(
                        enabled = !busy && !loading,
                        onClick = { importer.launchPicker(context, arrayOf("image/x-adobe-dng", "image/dng", "application/octet-stream")) },
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 0.dp),
                        modifier = Modifier.testTag("renderer_import").padding(end = 8.dp).height(32.dp)
                    ) { Text("Import DNG", maxLines = 1) }
                }
            )
        }
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            RendererStatusBar(loading = loading, busy = busy, progress = progress, onCancel = store::cancel)
            if (job != null && job.width > 0 && session != null && preview != null) {
                RendererEditor(
                    job = job,
                    session = requireNotNull(session),
                    previewSession = requireNotNull(preview),
                    onSaveResolution = viewModel::saveResolution,
                    onImportLut = viewModel::importEditorLut,
                    busy = busy || loading,
                    onBackToQueue = { if (busy) viewModel.back() else viewModel.backToQueue() },
                    onShadowLift = viewModel::setAdaptiveToneMode,
                    onLocalTone = viewModel::setLocalTone,
                    onExport = {
                        notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
                        viewModel.export(job.id)
                    }
                )
            } else {
                RendererLibrary(
                    jobs = jobs,
                    busy = busy,
                    loading = loading,
                    onImport = { importer.launchPicker(context, arrayOf("image/x-adobe-dng", "image/dng", "application/octet-stream")) },
                    onOpen = { entry -> viewModel.import(entry.uri) },
                    onEdit = viewModel::edit,
                    onRenderOriginal = { item ->
                        notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
                        viewModel.renderOriginal(item)
                    },
                    onRemove = viewModel::askRemove
                )
            }
        }
    }
    removing?.let { target -> AlertDialog(onDismissRequest = { viewModel.askRemove(null) }, title = { Text("Remove from queue?") },
        text = { Text("Private RAW inputs and edits will be removed. This may be the only remaining RAW copy of an unfinished capture. Imported originals and saved photos are kept.") },
        confirmButton = { OutlinedButton(onClick = { viewModel.remove(target) }) { Text("Remove") } },
        dismissButton = { FilledTonalButton(onClick = { viewModel.askRemove(null) }) { Text("Keep") } }) }
    if (cancelBack) AlertDialog(onDismissRequest = { viewModel.dismissCancel() }, title = { Text("Export in progress") }, text = { Text("Wait for export or cancel it before returning to capture.") },
        confirmButton = { OutlinedButton(onClick = { viewModel.cancelExport() }) { Text("Cancel export") } }, dismissButton = { FilledTonalButton(onClick = { viewModel.dismissCancel() }) { Text("Wait") } })
}
