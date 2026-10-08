package com.rawr.camera.storage

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import androidx.core.net.toUri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.MediaStore
import java.io.File
import android.system.Os
import android.system.OsConstants
import java.util.concurrent.ConcurrentHashMap

/**
 * Separated for tests: opens provider FDs for recovery. Production opens
 * through the ContentResolver; tests inject failures without Android.
 */
fun interface OutputFdOpener {
    fun open(uri: Uri, mode: String): ParcelFileDescriptor?
}

/**
 * True when the recovery output is gone for good (deleted gallery slot,
 * pruned pending row) as opposed to transiently unavailable. A missing
 * backing file never comes back, so retrying it every scan and every launch
 * is pure log spam — such entries are quarantined, never deleted.
 */
internal fun isOutputGonePermanently(error: Throwable): Boolean {
    var cause: Throwable? = error
    while (cause != null) {
        // Direct miss, or wrapped by the provider (e.g. MediaStore's
        // IllegalArgumentException("... is child of ...") caused by
        // FileNotFoundException("Missing file ...")).
        if (cause is java.io.FileNotFoundException) return true
        // Some providers flatten the missing-file signal into the message
        // text (DatabaseUtils parcel decoding) with no typed cause to walk.
        if (cause.message?.contains("Missing file", ignoreCase = true) == true) return true
        cause = cause.cause
    }
    return false
}

/** Kotlin/MediaStore owner for one shutter's base/merged DNGs and optional JPEG. */
class StillOutputStore(
    private val context: Context,
    private val fdOpener: OutputFdOpener = OutputFdOpener { uri, mode ->
        context.contentResolver.openFileDescriptor(uri, mode)
    }
) {
    data class PreparedFile(val uri: Uri, val fd: Int, val displayName: String)

    data class PreparedCapture(
        val dng: PreparedFile?, val mergedDng: PreparedFile? = null, val jpeg: PreparedFile?,
        val jobName: String = requireNotNull(dng).displayName,
        val multiframe: Boolean = mergedDng != null,
        val conditionalDng: Boolean = false,
        val capturedAt: Long = 0,
        val recipe: String = "",
        val fallback: Boolean = false
    ) {
        val primaryDng get() = if (multiframe) mergedDng else dng
        val requestedDngCount get() = listOfNotNull(dng, mergedDng).size - if (conditionalDng) 1 else 0
        val baseName get() = jobName.removeSuffix(".dng")
    }

    private data class PendingCapture(
        val output: PreparedCapture,
        var journal: CaptureJournal.Entry,
        val done: MutableSet<String> = mutableSetOf(),
        val scratch: List<File> = emptyList(),
        var conditionalFd: Int = if (output.conditionalDng) output.primaryDng?.fd ?: -1 else -1
    )

    private val journal = CaptureJournal(File(context.filesDir, "still_jobs")) { dir ->
        val fd = Os.open(dir.absolutePath, OsConstants.O_RDONLY, 0)
        try { Os.fsync(fd) } finally { Os.close(fd) }
    }
    private val recoveredEntries = ConcurrentHashMap<String, CaptureJournal.Entry>()
    private val recoveryScratch = ConcurrentHashMap<String, List<File>>()
    private val attemptedRecovery = mutableSetOf<String>()
    // Entries settled as permanently unrecoverable this launch (mirrors the
    // on-disk skip marker for entries quarantined before this process read
    // them). Consulted before any provider open attempt.
    private val quarantinedThisLaunch = mutableSetOf<String>()
    private val orphanEntries by lazy { journal.readAll() }
    private val lastNameTimestamp = java.util.concurrent.atomic.AtomicLong(0)
    private val pending = ConcurrentHashMap<Long, PendingCapture>()

    fun prepare(
        saveLocationId: String,
        wallClockMillis: Long,
        jpegEnabled: Boolean,
        multiframeFrames: Int = 0,
        dngEnabled: Boolean = true,
        saveBaseDng: Boolean = true,
        filmFallback: Boolean = false,
        recipe: String = ""
    ): PreparedCapture? {
        val base = captureBaseName(wallClockMillis)
        val destination = CaptureSaveLocation.fromId(saveLocationId)
        require(dngEnabled || jpegEnabled)
        val multiframe = multiframeFrames >= 2
        val conditional = !dngEnabled && jpegEnabled && filmFallback
        val suffix = if (multiframe) "_MF$multiframeFrames" else ""
        val opened = mutableListOf<PreparedFile>()
        fun target(name: String, mime: String): PreparedFile =
            checkNotNull(prepareFile(name, mime, destination)).also { opened.add(it) }
        val output = try {
            PreparedCapture(
                dng = if ((!multiframe && (dngEnabled || conditional)) || (multiframe && dngEnabled && saveBaseDng))
                    target("$base.dng", "image/x-adobe-dng") else null,
                mergedDng = if (multiframe && (dngEnabled || conditional)) target("$base$suffix.dng", "image/x-adobe-dng") else null,
                jpeg = if (jpegEnabled) target("$base$suffix.jpg", "image/jpeg") else null,
                jobName = "$base.dng", multiframe = multiframe, conditionalDng = conditional,
                capturedAt = wallClockMillis, recipe = recipe
            )
        } catch (error: Exception) {
            opened.forEach(::abandonFile)
            return null
        }
        val entry = entryFor(output)
        try { claimed.add(entry.name); journal.write(entry) }
        catch (error: Exception) { claimed.remove(entry.name); opened.forEach(::abandonFile); throw error }
        return output
    }

    fun register(requestId: Long, output: PreparedCapture) {
        pending[requestId] = PendingCapture(output,
            recoveredEntries.remove(output.jobName) ?: entryFor(output),
            scratch = recoveryScratch.remove(output.jobName).orEmpty())
    }

    data class DngCompletion(val allDone: Boolean, val success: Boolean)
    data class JpegCompletion(val uri: Uri?, val success: Boolean)

    @Synchronized
    fun completeDng(requestId: Long, displayName: String, success: Boolean): DngCompletion {
        val item = pending[requestId] ?: return DngCompletion(false, false)
        val target = item.journal.targets.firstOrNull { it.name == displayName && it.role != "jpeg" && !it.skipped }
            ?: item.journal.targets.firstOrNull { !success && displayName == "merged" && it.role == "merged" && !it.skipped }
            ?: return DngCompletion(false, false)
        if (!item.done.add(target.name)) return DngCompletion(dngDone(item), target.published)
        val published = finishTarget(item, target.uri.toUri(), success)
        val allDone = dngDone(item)
        retireIfDone(requestId, item)
        return DngCompletion(allDone, published)
    }

    private fun dngDone(item: PendingCapture) = item.journal.targets.filter { it.role != "jpeg" }
        .all { it.published || it.skipped || it.name in item.done }

    /** Persist substitution before native is asked to produce the emergency DNG. */
    @Synchronized
    fun useDngFallback(requestId: Long): PreparedCapture? {
        val item = pending[requestId] ?: return null
        val updated = item.journal.resolveForRecovery(memoryFallback = true)
        journal.write(updated)
        item.journal = updated
        item.output.jpeg?.let {
            runCatching { deleteUri(it.uri) }.onFailure { error ->
                android.util.Log.w("RawrCamCapture", "Skipped JPEG cleanup deferred", error)
            }
            item.done.add(it.displayName)
        }
        if (item.conditionalFd < 0) return null // Existing requested DNG is already in flight or published.
        val primary = requireNotNull(item.output.primaryDng).copy(fd = item.conditionalFd)
        item.conditionalFd = -1
        return item.output.copy(dng = if (item.output.multiframe) null else primary,
            mergedDng = if (item.output.multiframe) primary else null,
            jpeg = null, conditionalDng = false, fallback = true)
    }

    /** Stop this attempt after a journal/provider error; native's durable marker keeps recovery possible. */
    @Synchronized
    fun deferFallback(requestId: Long, reason: String) {
        val item = pending[requestId] ?: return
        if (item.conditionalFd >= 0) {
            runCatching { ParcelFileDescriptor.adoptFd(item.conditionalFd).close() }
            item.conditionalFd = -1
            item.output.primaryDng?.let { item.done.add(it.displayName) }
        }
        item.output.jpeg?.let { item.done.add(it.displayName) }
        runCatching { recordFailure(requestId, reason) }
        retireIfDone(requestId, item)
    }

    @Synchronized
    fun completeJpeg(requestId: Long, success: Boolean): JpegCompletion {
        val item = pending[requestId] ?: return JpegCompletion(null, false)
        val target = item.journal.targets.firstOrNull { it.role == "jpeg" }
        val published = if (target == null || target.skipped) false else if (item.done.add(target.name))
            finishTarget(item, target.uri.toUri(), success) else target.published
        if (item.conditionalFd >= 0) {
            ParcelFileDescriptor.adoptFd(item.conditionalFd).close()
            item.conditionalFd = -1
            if (published) {
                runCatching {
                    val updated = item.journal.copy(targets = item.journal.targets.map {
                        if (it.conditional) it.copy(skipped = true) else it
                    })
                    journal.write(updated)
                    item.journal = updated
                    item.journal.targets.filter { it.conditional }.forEach { deleteUri(it.uri.toUri()) }
                }.onFailure { android.util.Log.w("RawrCamCapture", "Conditional output cleanup deferred", it) }
            }
            item.journal.targets.filter { it.conditional }.forEach { item.done.add(it.name) }
        }
        retireIfDone(requestId, item)
        return JpegCompletion(if (published) item.output.jpeg?.uri else null, published)
    }

    private fun retireIfDone(id: Long, item: PendingCapture) {
        if (item.journal.targets.all { it.published || it.skipped || it.name in item.done }) retire(id, item)
    }

    fun abandon(output: PreparedCapture) {
        journal.remove(entryFor(output)); claimed.remove(output.jobName)
        listOfNotNull(output.dng, output.mergedDng, output.jpeg).forEach(::abandonFile)
    }

    /** Native owns requested descriptors; the conditional descriptor stays with Kotlin. */
    fun discardPublishedTargets(output: PreparedCapture) {
        journal.remove(entryFor(output)); claimed.remove(output.jobName)
        if (output.conditionalDng) output.primaryDng?.let { ParcelFileDescriptor.adoptFd(it.fd).close() }
        listOfNotNull(output.dng, output.mergedDng, output.jpeg).forEach { deleteUri(it.uri) }
    }

    private fun entryFor(output: PreparedCapture) = CaptureJournal.Entry(
        output.jobName,
        listOfNotNull(
            output.dng?.let { CaptureJournal.Target(it.uri.toString(), it.displayName,
                role = if (output.multiframe) "base" else "single", conditional = output.conditionalDng && !output.multiframe) },
            output.mergedDng?.let { CaptureJournal.Target(it.uri.toString(), it.displayName, role = "merged", conditional = output.conditionalDng) },
            output.jpeg?.let { CaptureJournal.Target(it.uri.toString(), it.displayName, role = "jpeg") }
        ), output.multiframe, output.capturedAt, output.recipe, output.fallback
    )

    @Synchronized
    fun recordFailure(requestId: Long, reason: String) {
        val item = pending[requestId] ?: return
        val updated = item.journal.copy(lastError = reason)
        journal.write(updated)
        item.journal = updated
    }

    private fun finishTarget(item: PendingCapture, uri: Uri, success: Boolean): Boolean {
        if (item.journal.targets.any { it.uri == uri.toString() && it.published }) return true
        // A committed input remains retryable after encoder/provider failure.
        if (!success && journal.hasInput(item.journal)) return false
        return try {
            finish(uri, success)
            if (success) {
                val updated = item.journal.copy(targets = item.journal.targets.map {
                    if (it.uri == uri.toString()) it.copy(published = true) else it
                })
                journal.write(updated)
                item.journal = updated
            }
            success
        } catch (error: Exception) {
            runCatching {
                val updated = item.journal.copy(lastError = error.message ?: "publication_failed")
                journal.write(updated); item.journal = updated
            }
            android.util.Log.e("RawrCamCapture", "Publication deferred; retaining capture input: $uri", error)
            false
        }
    }

    private fun retire(id: Long, item: PendingCapture) {
        pending.remove(id)
        item.scratch.forEach { it.delete() }
        if (item.journal.complete || !journal.hasInput(item.journal)) {
            runCatching {
                item.journal.targets.filter { it.skipped && !it.published }.forEach { runCatching { deleteUri(it.uri.toUri()) } }
                journal.remove(item.journal)
            }.onFailure {
                android.util.Log.e("RawrCamCapture", "Capture cleanup will be retried on restart", it)
            }
        }
        // Do not retry a failed job continuously in this process. A later app launch
        // can retry after permissions, free storage, or device state have changed.
        attemptedRecovery.add(item.journal.name)
        claimed.remove(item.journal.name)
    }

    /** Called on the IO dispatch lane after the foreground preview is ready. */
    fun nextRecovery(): PreparedCapture? {
        for (original in orphanEntries) {
            var entry = original
            if (File(context.filesDir, "still_jobs/${entry.name}.renderer-owner").exists() ||
                entry.name in attemptedRecovery || !claimed.add(entry.name)) continue
            attemptedRecovery.add(entry.name)
            val resolved = entry.resolveForRecovery(journal.hasFallback(entry))
            if (resolved != entry) {
                journal.write(resolved)
                entry = resolved
            }
            if (entry.complete || !journal.hasInput(entry)) {
                entry.targets.filter { it.skipped && !it.published }.forEach { runCatching { deleteUri(it.uri.toUri()) } }
                if (!entry.complete) entry.targets.filterNot { it.published || it.skipped }.forEach { deleteUri(it.uri.toUri()) }
                journal.remove(entry); claimed.remove(entry.name)
                continue
            }
            if (entry.name in quarantinedThisLaunch || journal.isSkipped(entry)) {
                // Settled previously: the outputs are permanently unavailable.
                // Silent by design — the journal + payload are preserved for a
                // future repair pass, but there is nothing new to report.
                quarantinedThisLaunch.add(entry.name)
                claimed.remove(entry.name)
                continue
            }
            val opened = mutableListOf<PreparedFile>()
            val scratch = mutableListOf<File>()
            try {
                entry.targets.filter { it.skipped && !it.published }.forEach { runCatching { deleteUri(it.uri.toUri()) } }
                // Optional native outputs let recovery omit published files entirely.
                // Never reopen, truncate, or reproduce an already-published photo.
                for (target in entry.targets.filterNot { it.skipped || it.published }) {
                    val fd = checkNotNull(fdOpener.open(target.uri.toUri(), "rwt")).detachFd()
                    opened.add(PreparedFile(target.uri.toUri(), fd, target.name))
                }
                fun file(role: String) = entry.targets.firstOrNull { it.role == role && !it.skipped && !it.published }
                    ?.let { target -> opened.firstOrNull { it.displayName == target.name } }
                val output = PreparedCapture(file(if (entry.multiframe) "base" else "single"), file("merged"), file("jpeg"),
                    entry.name, entry.multiframe, entry.targets.any { it.conditional && !it.skipped },
                    entry.capturedAt, entry.recipe, entry.fallback)
                recoveredEntries[entry.name] = entry
                recoveryScratch[entry.name] = scratch
                return output
            } catch (error: Exception) {
                opened.forEach { runCatching { ParcelFileDescriptor.adoptFd(it.fd).close() } }
                scratch.forEach { it.delete() }; claimed.remove(entry.name)
                if (isOutputGonePermanently(error)) {
                    // The backing file is gone for good (deleted gallery
                    // slot, pruned pending row); retrying every scan and
                    // every launch is pure log spam. Quarantine persistently
                    // but keep journal + payload for a future repair pass.
                    quarantinedThisLaunch.add(entry.name)
                    runCatching { journal.markSkipped(entry) }
                    android.util.Log.w("RawrCamCapture", "Skipping unrecoverable still ${entry.name}: output file gone")
                } else {
                    android.util.Log.e("RawrCamCapture", "Recovery output unavailable: ${entry.name}", error)
                }
            }
        }
        return null
    }

    fun releaseRecovery(output: PreparedCapture, closeConditional: Boolean = true) {
        // Native has consumed the descriptors, but the durable job must remain.
        if (closeConditional && output.conditionalDng) output.primaryDng?.let { ParcelFileDescriptor.adoptFd(it.fd).close() }
        recoveredEntries.remove(output.jobName)
        recoveryScratch.remove(output.jobName)?.forEach { it.delete() }
        claimed.remove(output.jobName)
    }

    internal companion object {
        fun claimForRenderer(filesDir: File, name: String): Boolean {
            require(name.isNotBlank() && '/' !in name && ".." !in name)
            if (!claimed.add(name)) return false
            return try {
                java.io.FileOutputStream(File(filesDir, "still_jobs/$name.renderer-owner")).use { it.write(1); it.fd.sync() }
                true
            } catch (error: Exception) { claimed.remove(name); throw error }
        }
        fun rendererRelease(filesDir: File, name: String) {
            File(filesDir, "still_jobs/$name.renderer-owner").delete()
            claimed.remove(name)
        }
        fun isClaimed(name: String) = name in claimed
        fun hasActiveCaptures(filesDir: File) = claimed.any { !File(filesDir, "still_jobs/$it.renderer-owner").exists() }
        private val claimed = ConcurrentHashMap.newKeySet<String>()
    }

    private fun captureBaseName(wallClockMillis: Long): String {
        val uniqueMillis = lastNameTimestamp.updateAndGet { previous -> maxOf(wallClockMillis, previous + 1) }
        return CaptureFileNames.baseName(uniqueMillis)
    }

    private fun prepareFile(displayName: String, mimeType: String, destination: CaptureSaveLocation): PreparedFile? {
        val resolver = context.contentResolver
        val treeUri = destination.treeUri?.toUri()
        val mediaUri =
            if (destination.relativePath.isEmpty()) {
                null
            } else {
                val values =
                    ContentValues().apply {
                        put(MediaStore.Images.Media.DISPLAY_NAME, displayName)
                        put(MediaStore.Images.Media.MIME_TYPE, mimeType)
                        put(MediaStore.Images.Media.RELATIVE_PATH, destination.relativePath)
                        put(MediaStore.Images.Media.IS_PENDING, 1)
                    }
                // A picked folder's volume may be unmounted; its tree grant is the fallback.
                if (treeUri == null) resolver.insert(destination.imagesCollection(), values) ?: return null
                else runCatching { resolver.insert(destination.imagesCollection(), values) }.getOrNull()
            }
        val uri =
            mediaUri ?: runCatching {
                val parent =
                    DocumentsContract.buildDocumentUriUsingTree(
                        treeUri,
                        DocumentsContract.getTreeDocumentId(treeUri)
                    )
                DocumentsContract.createDocument(resolver, parent, mimeType, displayName)
            }.getOrNull() ?: return null
        val pfd =
            resolver.openFileDescriptor(uri, "w") ?: run {
                deleteUri(uri)
                return null
            }
        return PreparedFile(uri, pfd.detachFd(), displayName)
    }

    private fun finish(uri: Uri, success: Boolean) {
        val resolver = context.contentResolver
        if (!success) {
            deleteUri(uri)
            return
        }
        if (uri.authority == MediaStore.AUTHORITY) {
            check(resolver.update(uri, ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }, null, null) > 0) {
                "Capture output disappeared before publication"
            }
        }
    }

    private fun deleteUri(uri: Uri) {
        if (uri.authority == MediaStore.AUTHORITY) {
            context.contentResolver.delete(uri, null, null)
        } else {
            runCatching { DocumentsContract.deleteDocument(context.contentResolver, uri) }
        }
    }

    private fun abandonFile(file: PreparedFile) {
        // detachFd() transferred the descriptor out of ParcelFileDescriptor. If native was never
        // invoked, Kotlin owns the raw fd and must close it before deleting the pending row.
        try {
            ParcelFileDescriptor.adoptFd(file.fd).close()
        } catch (_: Exception) {
        }
        deleteUri(file.uri)
    }
}
