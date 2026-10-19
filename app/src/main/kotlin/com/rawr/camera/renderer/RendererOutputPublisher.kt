package com.rawr.camera.renderer

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.provider.MediaStore
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

/** MediaStore adapter; native renderers receive an output descriptor only. */
internal class RendererOutputPublisher(private val context: Context) {
    fun isPublished(uri: Uri): Boolean = context.contentResolver.query(uri,
        arrayOf(MediaStore.Images.Media.IS_PENDING), null, null, null)?.use {
        it.moveToFirst() && it.getInt(0) == 0
    } ?: false
    fun createPending(displayName: String): Uri = checkNotNull(context.contentResolver.insert(
        MediaStore.Images.Media.EXTERNAL_CONTENT_URI, ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, displayName)
            put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
            put(MediaStore.Images.Media.RELATIVE_PATH, "DCIM/Camera")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }))
    fun write(uri: Uri, render: (Int) -> Unit) {
        checkNotNull(context.contentResolver.openFileDescriptor(uri, "rwt")).use { render(it.fd) }
    }
    /** [takenMillis]: the source capture time; null leaves the scanner's EXIF reading. */
    fun publish(uri: Uri, takenMillis: Long? = null) {
        check(context.contentResolver.update(uri, ContentValues().apply {
            if (takenMillis != null) put(MediaStore.Images.Media.DATE_TAKEN, takenMillis)
            put(MediaStore.Images.Media.IS_PENDING, 0)
        }, null, null) > 0) { "JPEG publication failed" }
    }
    fun delete(uri: Uri) { context.contentResolver.delete(uri, null, null) }
}

/**
 * DATE_TAKEN for a source DNG's EXIF capture time. Without OffsetTimeOriginal
 * the local time is read in [zone]: the scanner trusts an offset-less date only
 * while the file's modification time matches the capture, which an export never does.
 */
internal object SourceCaptureTime {
    private val format = DateTimeFormatter.ofPattern("yyyy:MM:dd HH:mm:ss")
    fun takenMillis(dateTimeOriginal: String, offsetTimeOriginal: String, subsecTimeOriginal: String,
                    zone: ZoneId = ZoneId.systemDefault()): Long? {
        val local = runCatching { LocalDateTime.parse(dateTimeOriginal.trim(), format) }.getOrNull() ?: return null
        val offset = offsetTimeOriginal.trim().takeIf { it.isNotEmpty() }?.let { runCatching { ZoneOffset.of(it) }.getOrNull() }
        val instant = if (offset != null) local.toInstant(offset) else local.atZone(zone).toInstant()
        val subsec = subsecTimeOriginal.trim().takeIf { it.isNotEmpty() && it.all(Char::isDigit) }
        return instant.toEpochMilli() + (subsec?.take(3)?.padEnd(3, '0')?.toLong() ?: 0L)
    }
}
