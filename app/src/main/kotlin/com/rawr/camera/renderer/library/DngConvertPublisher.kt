package com.rawr.camera.renderer.library

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.provider.MediaStore
import kotlinx.coroutines.CancellationException

/**
 * Runs one native conversion straight into a pending MediaStore DNG next to
 * the source, then publishes it with the dimensions, orientation and capture
 * time of the result so the gallery sorts and shows it like the original.
 */
class DngConvertPublisher(private val context: Context) {
    class Failure(message: String) : Exception(message)

    /** Blocking; [job] comes from [ConvertNative.newJob] and can be cancelled from another thread. */
    fun convert(entry: DngEntry, conversion: DngConversion, job: Long): Uri {
        require(entry.convertible) { entry.error.ifBlank { "This DNG cannot be converted" } }
        val resolver = context.contentResolver
        val name = DngLibrary.outputName(entry, conversion)
        val output = checkNotNull(resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, name)
            put(MediaStore.Images.Media.MIME_TYPE, "image/x-adobe-dng")
            put(MediaStore.Images.Media.RELATIVE_PATH, entry.relativePath.ifBlank { "DCIM/Camera/" })
            if (entry.taken > 0) put(MediaStore.Images.Media.DATE_TAKEN, entry.taken)
            put(MediaStore.Images.Media.IS_PENDING, 1)
        })) { "Cannot create $name" }
        try {
            val error = checkNotNull(resolver.openFileDescriptor(entry.uri, "r")) { "Cannot open ${entry.name}" }.use { source ->
                checkNotNull(resolver.openFileDescriptor(output, "rw")) { "Cannot write $name" }.use { target ->
                    ConvertNative.convert(job, "/proc/self/fd/${source.fd}", "/proc/self/fd/${target.fd}", conversion.nativeMode,
                        offsetTime(entry))
                }
            }
            if (error == "cancelled") throw CancellationException("Conversion cancelled")
            if (error.isNotEmpty()) throw Failure(error)
            // The scanner reads these from the DNG too; set them so the row is
            // complete the moment it is published.
            check(resolver.update(output, ContentValues().apply {
                put(MediaStore.Images.Media.WIDTH, outputSize(entry.width, conversion))
                put(MediaStore.Images.Media.HEIGHT, outputSize(entry.height, conversion))
                put(MediaStore.Images.Media.ORIENTATION, degrees(entry.orientation))
                put(MediaStore.Images.Media.IS_PENDING, 0)
            }, null, null) > 0) { "Publishing $name failed" }
            return output
        } catch (t: Throwable) {
            runCatching { resolver.delete(output, null, null) }
            throw t
        }
    }

    /** Output side length, as the native converter computes it. */
    private fun outputSize(value: Int, conversion: DngConversion): Int = when (conversion) {
        DngConversion.Lossless -> value
        DngConversion.Quarter -> value / 2
        DngConversion.Sixteenth -> value / 4
        DngConversion.Eighth -> ((value / 2) / Math.sqrt(2.0)).toInt() and 1.inv()
    }

    /**
     * EXIF offset recovered from the source's MediaStore capture time and its
     * local DateTimeOriginal, when the source has no offset of its own. The
     * scanner trusts an offset-less date only while the file's modification
     * time matches the capture, which a later conversion never does.
     */
    private fun offsetTime(entry: DngEntry): String {
        if (entry.offsetTimeOriginal.isNotBlank() || entry.taken <= 0) return ""
        val local = runCatching {
            java.text.SimpleDateFormat("yyyy:MM:dd HH:mm:ss", java.util.Locale.US)
                .apply { timeZone = java.util.TimeZone.getTimeZone("UTC") }
                .parse(entry.dateTimeOriginal.trim())?.time
        }.getOrNull() ?: return ""
        val minutes = Math.round((local - entry.taken) / 60_000.0 / 15.0) * 15
        if (Math.abs(minutes) > 14 * 60) return ""
        val sign = if (minutes < 0) '-' else '+'
        return "%c%02d:%02d".format(sign, Math.abs(minutes) / 60, Math.abs(minutes) % 60)
    }

    private fun degrees(orientation: Int): Int = when (orientation) {
        3, 4 -> 180
        5, 6 -> 90
        7, 8 -> 270
        else -> 0
    }
}
