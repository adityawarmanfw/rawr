package com.rawr.camera.renderer.library

import android.content.Context
import androidx.core.content.edit
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject

/** One file and size waiting to be converted. */
data class DngConvertTask(val entry: DngEntry, val conversion: DngConversion) {
    val key: String get() = "${entry.id}:${conversion.name}"
}

data class DngConvertState(
    val pending: List<DngConvertTask> = emptyList(),
    val current: DngConvertTask? = null,
    /** Current file progress, 0..1000. */
    val permille: Int = 0,
    val done: Int = 0,
    val failed: List<Pair<DngConvertTask, String>> = emptyList(),
    /** Tasks that finished, for "Delete originals". */
    val succeeded: List<DngConvertTask> = emptyList()
) {
    val running: Boolean get() = current != null
    val total: Int get() = done + failed.size + pending.size + (if (current != null) 1 else 0)
}

/**
 * Sequential conversion queue. Runs under [DngConvertService] so it survives
 * leaving the screen; pending tasks are persisted and resumed on next start.
 */
class DngConvertQueue private constructor(private val context: Context) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val prefs = context.getSharedPreferences("dng_convert_queue", Context.MODE_PRIVATE)
    private val publisher = DngConvertPublisher(context)
    private val mutableState = MutableStateFlow(DngConvertState())
    val state: StateFlow<DngConvertState> = mutableState.asStateFlow()
    private var worker: Job? = null
    @Volatile private var job = 0L

    /** Adds tasks not already queued or converted, and starts the queue. */
    fun enqueue(tasks: List<DngConvertTask>) {
        mutableState.update { state ->
            val known = (state.pending.map { it.key } + listOfNotNull(state.current?.key)).toSet()
            val fresh = tasks.filter { it.key !in known && it.conversion.outputMegapixels(it.entry) !in it.entry.converted }
            if (state.running) state.copy(pending = state.pending + fresh)
            else DngConvertState(pending = fresh)
        }
        persist()
        start()
    }

    /** Stops the current file and drops everything still pending. */
    fun cancelAll() {
        mutableState.update { it.copy(pending = emptyList()) }
        persist()
        job.takeIf { it != 0L }?.let(ConvertNative::cancel)
    }

    fun dismissResults() {
        if (!state.value.running) mutableState.update { DngConvertState(pending = it.pending) }
    }

    /**
     * Sources whose every conversion in this batch succeeded (converter
     * outputs excluded): the files "Delete originals" offers to remove.
     */
    fun deletableOriginals(): List<DngEntry> {
        val state = state.value
        val failedIds = state.failed.map { it.first.entry.id }.toSet()
        return state.succeeded.map { it.entry }.distinctBy { it.id }
            .filter { it.id !in failedIds && !it.conversionOutput }
    }

    /** Re-queues tasks left by a previous process. */
    fun resume() {
        if (state.value.running || state.value.pending.isNotEmpty()) return
        val saved = runCatching { JSONArray(prefs.getString(KEY, "[]")) }.getOrNull() ?: return
        if (saved.length() == 0) return
        val library = DngLibrary.get(context).entries.value.associateBy { it.id }
        val tasks = (0 until saved.length()).mapNotNull { i ->
            val o = saved.getJSONObject(i)
            val entry = library[o.getLong("id")] ?: return@mapNotNull null
            val conversion = runCatching { DngConversion.valueOf(o.getString("conversion")) }.getOrNull() ?: return@mapNotNull null
            DngConvertTask(entry, conversion)
        }
        if (tasks.isNotEmpty()) enqueue(tasks)
    }

    private fun start() {
        if (worker?.isActive == true || state.value.pending.isEmpty()) return
        DngConvertService.start(context)
        worker = scope.launch {
            try {
                while (isActive) {
                    val task = state.value.pending.firstOrNull() ?: break
                    mutableState.update { it.copy(pending = it.pending.drop(1), current = task, permille = 0) }
                    runTask(task)
                }
            } finally {
                mutableState.update { it.copy(current = null, permille = 0) }
                persist()
                DngConvertService.stop(context)
                DngLibrary.get(context).requestScan()
            }
        }
    }

    private suspend fun runTask(task: DngConvertTask) {
        val handle = ConvertNative.newJob()
        job = handle
        val ticker = scope.launch {
            while (isActive) {
                mutableState.update { it.copy(permille = ConvertNative.progress(handle)) }
                delay(250)
            }
        }
        try {
            publisher.convert(task.entry, task.conversion, handle)
            mutableState.update { it.copy(done = it.done + 1, succeeded = it.succeeded + task) }
        } catch (e: CancellationException) {
            // Cancelled by the user: not a failure.
        } catch (e: Exception) {
            mutableState.update { it.copy(failed = it.failed + (task to (e.message ?: "Conversion failed"))) }
        } finally {
            ticker.cancel()
            job = 0L
            ConvertNative.releaseJob(handle)
            persist()
        }
    }

    private fun persist() {
        val state = state.value
        val tasks = listOfNotNull(state.current) + state.pending
        prefs.edit {
            putString(KEY, JSONArray(tasks.map { JSONObject().put("id", it.entry.id).put("conversion", it.conversion.name) }).toString())
        }
    }

    companion object {
        private const val KEY = "tasks"
        @Volatile private var instance: DngConvertQueue? = null
        fun get(context: Context): DngConvertQueue =
            instance ?: synchronized(this) { instance ?: DngConvertQueue(context.applicationContext).also { instance = it } }
    }
}
