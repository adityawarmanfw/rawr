package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.SettingsValues
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.launch

/** One optimistic snapshot for global settings; editor drafts deliberately use local controllers. */
class SettingsEditor(
    defaults: SettingsValues,
    persisted: Flow<SettingsValues>,
    private val save: suspend (SettingsValues) -> Unit,
    scope: CoroutineScope,
    private val onChanged: (SettingsValues) -> Unit = {},
    private val onPersisted: (SettingsValues, SettingsValues) -> Unit = { _, _ -> }
) {
    private val mutableState = MutableStateFlow(defaults)
    val state = mutableState.asStateFlow()
    private val ready = CompletableDeferred<Unit>()
    /** Backend consumers wait for durable initialization instead of applying provisional defaults. */
    val values: Flow<SettingsValues> = flow { ready.await(); emitAll(state) }
    private data class Write(val revision: Long, val values: SettingsValues)
    private val writes = Channel<Write>(Channel.CONFLATED)
    private var revision = 0L
    private val acknowledgements = mutableListOf<Pair<Long, CompletableDeferred<Unit>>>()

    private fun enqueue(values: SettingsValues) {
        writes.trySend(Write(++revision, values))
    }

    private fun acknowledge(savedRevision: Long, failure: Throwable? = null) {
        val finished = acknowledgements.filter { it.first <= savedRevision }
        acknowledgements.removeAll(finished.toSet())
        finished.forEach { (_, completion) ->
            if (failure == null) completion.complete(Unit) else completion.completeExceptionally(failure)
        }
    }
    private val pending = mutableListOf<(SettingsValues) -> SettingsValues>()
    private var initialized = false
    private val mutableSaveFailure = MutableStateFlow<Throwable?>(null)
    val saveFailure = mutableSaveFailure.asStateFlow()

    init {
        scope.launch {
            try {
                val loaded = persisted.first()
                synchronized(this@SettingsEditor) {
                    val next = pending.fold(loaded) { values, edit -> edit(values) }
                    onChanged(next)
                    mutableState.value = next
                    initialized = true
                    if (pending.isNotEmpty()) enqueue(next)
                    pending.clear()
                    ready.complete(Unit)
                }
            } catch (e: Exception) {
                ready.completeExceptionally(e)
                mutableSaveFailure.value = e
            }
        }
        scope.launch {
            for (write in writes) {
                try {
                    save(write.values)
                    synchronized(this@SettingsEditor) {
                        onPersisted(write.values, mutableState.value)
                        mutableSaveFailure.value = null
                        acknowledge(write.revision)
                    }
                } catch (e: kotlinx.coroutines.CancellationException) {
                    throw e
                } catch (e: Exception) {
                    synchronized(this@SettingsEditor) {
                        mutableSaveFailure.value = e
                        acknowledge(write.revision, e)
                    }
                }
            }
        }
    }

    /** Waits for this edit (or a newer conflated snapshot) to reach durable storage. */
    suspend fun updateAndAwaitPersistence(edit: (SettingsValues) -> SettingsValues) {
        ready.await()
        val completion = CompletableDeferred<Unit>()
        synchronized(this) {
            val beforeRevision = revision
            update(edit)
            // Also retry a previously failed save when the edit leaves the live values unchanged.
            if (beforeRevision == revision) enqueue(mutableState.value)
            acknowledgements += revision to completion
        }
        try {
            completion.await()
        } finally {
            synchronized(this) { acknowledgements.removeAll { it.second === completion } }
        }
    }

    /** Transforms the latest snapshot atomically, so separate screens cannot overwrite each other. */
    @Synchronized
    fun update(edit: (SettingsValues) -> SettingsValues): SettingsValues {
        if (!initialized) pending += edit
        val before = mutableState.value
        val next = edit(before)
        if (next != before) {
            if (initialized) onChanged(next)
            mutableState.value = next
            if (initialized) enqueue(next)
        }
        return next
    }
}
