package com.rawr.camera.renderer

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.rawr.camera.settings.model.SettingsCatalog
import com.rawr.camera.settings.preferences.SettingsPreferencesStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal data class RendererUiState(
    val selectedId: String? = null,
    val loading: Boolean = false,
    val removing: RenderJob? = null,
    val confirmCancel: Boolean = false
)

internal sealed interface RendererEffect {
    data class Message(val text: String) : RendererEffect
    data object Exit : RendererEffect
}

/** Screen state and commands; durable export jobs remain owned by RendererStore/RendererService. */
internal class RendererViewModel(application: Application) : AndroidViewModel(application) {
    val store = RendererStore.get(application)
    private val settings = SettingsPreferencesStore.editor(application, SettingsCatalog.initialState().values)
    private val mutableState = MutableStateFlow(RendererUiState())
    val state = mutableState.asStateFlow()
    private val effectQueue = Channel<RendererEffect>(Channel.BUFFERED)
    val effects = effectQueue.receiveAsFlow()
    private val mutableEditor = MutableStateFlow<RendererEditorSession?>(null)
    val editor = mutableEditor.asStateFlow()
    private val mutablePreview = MutableStateFlow<RendererPreviewSession?>(null)
    val preview = mutablePreview.asStateFlow()
    private var editorJobId: String? = null
    private var refreshJob: Job? = null

    init {
        viewModelScope.launch {
            store.jobs.collect { jobs ->
                val selected = state.value.selectedId
                if (selected != null && jobs.none { it.id == selected } && !state.value.loading) select(null)
                else synchronizeEditor()
            }
        }
        viewModelScope.launch {
            store.completion.filterNotNull().collect {
                select(null)
                report("Exported to gallery")
            }
        }
    }

    fun onVisible() {
        store.visible = true
        mutablePreview.value?.setVisible(true)
        refreshJob?.cancel()
        refreshJob = viewModelScope.launch {
            store.awaitReady()
            withContext(Dispatchers.IO) { store.refresh() }
            runCatching { store.resumeInterrupted() }.onFailure { report(it.message) }
            while (isActive) {
                delay(1_000)
                withContext(Dispatchers.IO) { store.refresh() }
            }
        }
    }

    fun onHidden() {
        store.visible = false
        refreshJob?.cancel()
        refreshJob = null
        mutablePreview.value?.setVisible(false)
        store.releasePreview()
    }

    private fun select(id: String?) {
        mutableState.update { it.copy(selectedId = id) }
        synchronizeEditor()
    }

    private fun synchronizeEditor() {
        val selected = store.jobs.value.firstOrNull { it.id == state.value.selectedId && it.width > 0 }
        if (selected?.id != editorJobId) {
            mutablePreview.value?.close()
            editorJobId = selected?.id
            mutableEditor.value = selected?.let { job ->
                RendererEditorSession(RendererRecipe.decode(job.draft, settings.state.value)) {
                    store.updateDraft(job.id, it)
                }
            }
            mutablePreview.value = selected?.let { RendererPreviewSession(it, store, ::report) }
            if (!store.visible) mutablePreview.value?.setVisible(false)
        }
        if (selected != null) mutablePreview.value?.updateJob(selected)
    }

    private fun operation(work: suspend () -> Unit) {
        if (state.value.loading || store.busy.value) return
        mutableState.update { it.copy(loading = true) }
        viewModelScope.launch {
            try { store.awaitReady(); work() }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) { report(e.message) }
            finally { mutableState.update { it.copy(loading = false) }; synchronizeEditor() }
        }
    }

    fun import(uri: Uri) = operation { select(store.import(uri, settings.values.first()).id) }
    fun edit(job: RenderJob) = operation { select(store.edit(job, settings.values.first()).id) }
    fun renderOriginal(job: RenderJob) = operation {
        val ready = store.edit(job, settings.values.first())
        store.export(ready.id, original = true)
    }
    fun export(id: String) {
        if (state.value.loading || store.busy.value) return
        runCatching { store.export(id) }.onFailure { report(it.message) }
    }
    fun backToQueue() { select(null); store.cancel() }
    fun back() {
        when {
            store.busy.value -> mutableState.update { it.copy(confirmCancel = true) }
            state.value.loading -> Unit
            state.value.selectedId != null -> backToQueue()
            else -> operation { store.closePreview(); effectQueue.send(RendererEffect.Exit) }
        }
    }
    fun askRemove(job: RenderJob?) { mutableState.update { it.copy(removing = job) } }
    fun remove(job: RenderJob) { askRemove(null); operation { store.remove(job) } }
    fun dismissCancel() { mutableState.update { it.copy(confirmCancel = false) } }
    fun cancelExport() { store.cancel(); dismissCancel() }
    fun report(message: String?) {
        if (!message.isNullOrBlank()) viewModelScope.launch { effectQueue.send(RendererEffect.Message(message)) }
    }
    fun importEditorLut(target: String?, uri: Uri) {
        val session = mutableEditor.value ?: return
        if (store.busy.value) return
        viewModelScope.launch {
            try {
                val stage = withContext(Dispatchers.IO) {
                    com.rawr.camera.settings.preferences.LutProfileFileStore(getApplication()).importCube(uri)
                }
                if (!store.busy.value && mutableEditor.value === session) {
                    session.controller.value.dispatch(com.rawr.camera.settings.architecture.ImportLutStage(target, stage))
                }
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { report(e.message) }
        }
    }

    fun setAdaptiveToneMode(job: RenderJob, mode: com.rawr.camera.settings.model.AutoKeyExposure) {
        if (state.value.loading || store.busy.value) return
        store.updateAdaptiveToneMode(job.id, mode)
    }

    fun saveResolution(job: RenderJob, megapixels: Double) = operation {
        withContext(Dispatchers.IO) { store.save(store.current(job.id).copy(megapixels = megapixels)) }
    }

    override fun onCleared() {
        mutablePreview.value?.close()
        store.releasePreview()
    }
}
