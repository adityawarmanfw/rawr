package com.rawr.camera.integration

import com.rawr.camera.model.CaptureOutputFormat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** Serializes provider preparation and completion publication away from the shutter caller. */
internal class StillCaptureDispatchQueue(
    scope: CoroutineScope,
    private val onQueued: (CaptureOutputFormat) -> Unit,
    private val capture: (CaptureOutputFormat) -> Unit,
    private val onRejected: (CaptureOutputFormat, RuntimeException?) -> Unit
) {
    private val requests = Channel<CaptureOutputFormat>(Channel.UNLIMITED)
    private val mutex = Mutex()

    init {
        scope.launch {
            for (format in requests) mutex.withLock { capture(format) }
        }
    }

    /** Admits synchronously in press order when [owned] still holds; returns whether the request was queued. */
    @Synchronized
    fun enqueue(format: CaptureOutputFormat, owned: () -> Boolean = { true }): Boolean {
        try {
            if (!owned()) {
                onRejected(format, null)
                return false
            }
            onQueued(format)
            check(requests.trySend(format).isSuccess)
            return true
        } catch (error: RuntimeException) {
            onRejected(format, error)
            return false
        }
    }

    suspend fun poll(completion: () -> Unit) {
        // A fast native completion must not overtake output registration or UI enqueue.
        mutex.withLock { completion() }
    }
}
