package com.rawr.camera.storage

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.MediaStore
import androidx.core.graphics.createBitmap
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.pow

/**
 * Debug-only exporter for still-pipeline artifacts.
 *
 * Native owns the exact diagnostic capture/dump. This class only mirrors the completed files
 * into scoped-storage locations that are visible from the device's Files/Photos apps.
 */
class ArtifactDiagnosticExporter(private val context: Context) {
    private val privateFiles = context.filesDir

    fun clearPrivateLatest() {
        PRIVATE_ARTIFACTS.forEach { File(privateFiles, it).delete() }
        File(privateFiles, ZSL_BUNDLE).delete()
        File(privateFiles, ZSL_READY).delete()
        File(privateFiles, ZSL_FAILED).delete()
    }

    data class ZslExportResult(val success: Boolean, val bytes: Long, val reason: String)

    /** Wait for native to finish the single-file bundle, then publish it next to the save location. */
    fun exportZslLatest(baseName: String, timeoutMs: Long, saveLocationId: String): ZslExportResult {
        val safeBase = baseName.removeSuffix(".jpg").removeSuffix(".jpeg")
        val target =
            resolveTarget(saveLocationId, safeBase, ZSL_FOLDER) ?: run {
                val nativeReason = readNativeFailure().ifBlank { "No usable save location" }
                return ZslExportResult(false, 0L, nativeReason)
            }
        val wait = waitForZslReady(timeoutMs)
        if (wait != ZslWaitResult.Ready) {
            val nativeReason = readNativeFailure()
            val reason =
                when (wait) {
                    ZslWaitResult.Failed -> nativeReason.ifBlank { "Native ZSL bundle creation failed" }
                    ZslWaitResult.Timeout -> "Timed out waiting for native ZSL bundle creation"
                    else -> "Native ZSL bundle was not ready"
                }
            return ZslExportResult(false, 0L, reason)
        }
        val zsl = File(privateFiles, ZSL_BUNDLE)
        if (!zsl.isFile ||
            zsl.length() <= 0L
        ) {
            return ZslExportResult(false, 0L, "Native ready marker exists but bundle is missing or empty")
        }
        val bytes = zsl.length()
        return if (target.publish(zsl, "$safeBase-zsl.rzsl", "application/octet-stream")) {
            ZslExportResult(true, bytes, "")
        } else {
            ZslExportResult(false, 0L, "Artifact publication failed")
        }
    }

    fun exportLatest(baseName: String, waitForZsl: Boolean = false, saveLocationId: String) {
        val safeBase = baseName.removeSuffix(".jpg").removeSuffix(".jpeg")
        val target = resolveTarget(saveLocationId, safeBase) ?: return

        val tone = File(privateFiles, POST_TONEMAP)
        val packed = File(privateFiles, PACKED_CFA)
        val demosaic = File(privateFiles, DEMOSAIC_RGB)
        val dualMask = File(privateFiles, DUAL_MASK)
        val preWb = File(privateFiles, PRE_WB_RGB)
        val preFcc = File(privateFiles, PRE_FCC_RGB)
        val preTonemap = File(privateFiles, PRE_TONEMAP_RGB)
        val manifest = File(privateFiles, MANIFEST)
        val log = File(privateFiles, "rawrcam_validation_current.txt")
        if (waitForZsl) waitForZslReady(30_000L)

        if (packed.isFile) target.publish(packed, "$safeBase-packed-cfa-rg1g2b.rgba16f", "application/octet-stream")
        if (demosaic.isFile) target.publish(demosaic, "$safeBase-post-demosaic.rgba16f", "application/octet-stream")
        if (dualMask.isFile) target.publish(dualMask, "$safeBase-dual-blend-mask.r32f", "application/octet-stream")
        if (preWb.isFile) target.publish(preWb, "$safeBase-pre-wb-pre-highlight.rgba16f", "application/octet-stream")
        if (preFcc.isFile) {
            target.publish(
                preFcc,
                "$safeBase-post-wb-highlight-pre-fcc.rgba16f",
                "application/octet-stream"
            )
        }
        if (preTonemap.isFile) {
            target.publish(
                preTonemap,
                "$safeBase-pre-tonemap-post-fcc.rgba16f",
                "application/octet-stream"
            )
        }
        if (tone.isFile) target.publish(tone, "$safeBase-post-tonemap.ppm", "image/x-portable-pixmap")
        if (manifest.isFile) target.publish(manifest, "$safeBase-manifest.txt", "text/plain")
        if (log.isFile) target.publish(log, "$safeBase-diagnostic-log.txt", "text/plain")

        // Companion PNGs are for convenient device-side inspection only. The binary files above
        // remain the exact evidence and use a fixed linear-to-sRGB visualization mapping.
        if (demosaic.isFile) rgba16fToPng(demosaic, manifest, "$safeBase-post-demosaic.png", target)
        if (tone.isFile) ppmToPng(tone, "$safeBase-post-tonemap.png", target)
    }

    /**
     * Bundle the continuous diagnostics files (validation log, DCG / pipeline /
     * frame audits, internal trace dump if present) into one zip published to
     * `<save>/debug/`, mirroring the capture save location. Returns the published
     * display name, or null when there is nothing to export or no usable target.
     */
    /**
     * Zips the always-on diagnostics (native audit files, their rotated `.1`
     * halves, the runtime trace and this process's logcat) plus a
     * `bundle_info.txt` summary into [tree]. Returns the file name; throws
     * with a user-facing message on failure.
     */
    fun exportDiagnosticsBundle(tree: Uri, stamp: String): String {
        val present =
            DIAGNOSTIC_BUNDLE_FILES
                .flatMap { listOf(File(privateFiles, it), File(privateFiles, "$it.1")) }
                .filter { it.isFile && it.length() > 0L }
        // Testers on other devices usually have no adb, so the bundle carries
        // this process's own logcat (readable without any permission).
        val logcat = ownLogcat()
        val resolver = context.contentResolver
        val displayName = "RAWR_diagnostics_$stamp.zip"
        val parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        val target =
            checkNotNull(DocumentsContract.createDocument(resolver, parent, "application/zip", displayName)) {
                "Unable to create the zip in that folder"
            }
        try {
            checkNotNull(resolver.openOutputStream(target, "wt")) { "Unable to write the zip" }.use { output ->
                java.util.zip.ZipOutputStream(output.buffered()).use { z ->
                    z.putNextEntry(java.util.zip.ZipEntry("bundle_info.txt"))
                    z.write(bundleInfo(stamp, present, logcat).toByteArray())
                    z.closeEntry()
                    for (file in present) {
                        z.putNextEntry(java.util.zip.ZipEntry(file.name))
                        file.inputStream().buffered().use { it.copyTo(z) }
                        z.closeEntry()
                    }
                    if (logcat != null) {
                        z.putNextEntry(java.util.zip.ZipEntry("logcat.txt"))
                        z.write(deviceHeader().toByteArray())
                        z.write(logcat)
                        z.closeEntry()
                    }
                }
            }
        } catch (t: Throwable) {
            runCatching { DocumentsContract.deleteDocument(resolver, target) }
            throw t
        }
        return displayName
    }

    /** First file in the zip: what is in it and where it came from, so a thin bundle is obvious. */
    private fun bundleInfo(stamp: String, files: List<File>, logcat: ByteArray?): String {
        val time = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", java.util.Locale.US)
        return buildString {
            appendLine("RAWR diagnostic bundle $stamp")
            appendLine("exported=${time.format(java.util.Date())}")
            appendLine(
                "app=${com.rawr.camera.BuildConfig.APPLICATION_ID} version=${com.rawr.camera.BuildConfig.VERSION_NAME} " +
                    "code=${com.rawr.camera.BuildConfig.VERSION_CODE} build=${com.rawr.camera.BuildConfig.BUILD_TYPE}"
            )
            appendLine(
                "device=${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL} (${android.os.Build.DEVICE}) " +
                    "android=${android.os.Build.VERSION.RELEASE} sdk=${android.os.Build.VERSION.SDK_INT} soc=${android.os.Build.SOC_MODEL} " +
                    "fingerprint=${android.os.Build.FINGERPRINT}"
            )
            appendLine("files:")
            if (files.isEmpty()) appendLine("  (none: the camera has not run since install or data was cleared)")
            for (f in files) appendLine("  ${f.name} bytes=${f.length()} modified=${time.format(java.util.Date(f.lastModified()))}")
            appendLine(
                if (logcat != null) {
                    "  logcat.txt bytes=${logcat.size} lines=${logcat.count { it == '\n'.code.toByte() }}"
                } else {
                    "  logcat.txt missing (logcat unreadable)"
                }
            )
        }
    }

    private fun ownLogcat(): ByteArray? =
        runCatching {
            val process =
                ProcessBuilder("logcat", "-d", "-v", "threadtime", "--pid=${android.os.Process.myPid()}")
                    .redirectErrorStream(true)
                    .start()
            val bytes = process.inputStream.use { it.readBytes() }
            process.waitFor()
            bytes.takeIf { it.isNotEmpty() }
        }.getOrNull()

    private fun deviceHeader(): String =
        "# ${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL} (${android.os.Build.DEVICE}) " +
            "Android ${android.os.Build.VERSION.RELEASE} SDK ${android.os.Build.VERSION.SDK_INT} " +
            "SoC ${android.os.Build.SOC_MODEL}\n"

    /**
     * Debug artifacts live in a `debug/` folder next to the capture save location,
     * mirroring [StillOutputStore]'s location resolution: MediaStore presets map to
     * `<preset>/debug`, SAF trees get a `debug` child created under the granted tree.
     * RZSL bursts go to a sibling `rzsl/` folder instead.
     */
    private sealed interface ArtifactTarget {
        fun publish(source: File, displayName: String, mime: String): Boolean

        fun publishBitmap(bitmap: Bitmap, displayName: String): Boolean
    }

    private fun resolveTarget(
        saveLocationId: String,
        safeBase: String,
        folder: String = DEBUG_FOLDER
    ): ArtifactTarget? {
        val treeUri =
            saveLocationId
                .takeIf { it.startsWith("storage.tree:") }
                ?.removePrefix("storage.tree:")
                ?.let(Uri::parse)
        if (treeUri != null) {
            val dir =
                runCatching {
                    val parent =
                        DocumentsContract.buildDocumentUriUsingTree(
                            treeUri,
                            DocumentsContract.getTreeDocumentId(treeUri)
                        )
                    val existing = findTreeChild(parent, folder)
                    existing ?: DocumentsContract.createDocument(
                        context.contentResolver,
                        parent,
                        DocumentsContract.Document.MIME_TYPE_DIR,
                        folder
                    )
                }.getOrNull() ?: return null
            return TreeTarget(dir)
        }
        val basePath =
            when (saveLocationId) {
                "storage.pictures_raw" -> "${Environment.DIRECTORY_PICTURES}/RAW Camera"
                else -> "${Environment.DIRECTORY_DCIM}/Camera"
            }
        return MediaStoreTarget("$basePath/$folder")
    }

    private fun findTreeChild(parent: Uri, displayName: String): Uri? {
        val resolver = context.contentResolver
        // ExternalStorageProvider document ids are paths ("primary:DCIM/Rawr/debug"):
        // probe the child directly. Listing a large capture folder missed the
        // existing child on device, and every miss created "debug (n)".
        runCatching {
            val direct =
                DocumentsContract.buildDocumentUriUsingTree(
                    parent,
                    DocumentsContract.getDocumentId(parent) + "/" + displayName
                )
            resolver
                .query(direct, arrayOf(DocumentsContract.Document.COLUMN_MIME_TYPE), null, null, null)
                ?.use { cursor ->
                    if (cursor.moveToFirst() && cursor.getString(0) == DocumentsContract.Document.MIME_TYPE_DIR) {
                        return direct
                    }
                }
        }
        val childrenUri =
            DocumentsContract.buildChildDocumentsUriUsingTree(parent, DocumentsContract.getDocumentId(parent))
        return runCatching {
            resolver
                .query(
                    childrenUri,
                    arrayOf(
                        DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        DocumentsContract.Document.COLUMN_DISPLAY_NAME
                    ),
                    null,
                    null,
                    null
                )?.use { cursor ->
                    val idCol = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                    val nameCol = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                    while (cursor.moveToNext()) {
                        if (cursor.getString(nameCol) == displayName) {
                            return DocumentsContract.buildDocumentUriUsingTree(parent, cursor.getString(idCol))
                        }
                    }
                    null
                }
        }.getOrNull()
    }

    private inner class MediaStoreTarget(private val relativePath: String) : ArtifactTarget {
        override fun publish(source: File, displayName: String, mime: String): Boolean {
            // PNG previews belong in Images; everything else goes through Files so
            // binaries stay next to the captures instead of a hardcoded Downloads dir.
            // MediaStore only accepts non-media files under Download/ or Documents/
            // (Android 11+ rejects DCIM/Pictures for them), so binaries go to
            // Download/RAWR/<same subpath> and PNGs stay next to the captures.
            val image = mime == "image/png"
            val collection =
                if (image) MediaStore.Images.Media.EXTERNAL_CONTENT_URI else MediaStore.Downloads.EXTERNAL_CONTENT_URI
            val resolver = context.contentResolver
            val values =
                ContentValues().apply {
                    put(MediaStore.MediaColumns.DISPLAY_NAME, displayName)
                    put(MediaStore.MediaColumns.MIME_TYPE, mime)
                    put(
                        MediaStore.MediaColumns.RELATIVE_PATH,
                        if (image) relativePath else "${Environment.DIRECTORY_DOWNLOADS}/RAWR/${relativePath.substringAfter('/')}"
                    )
                    put(MediaStore.MediaColumns.IS_PENDING, 1)
                }
            val uri = runCatching { resolver.insert(collection, values) }.getOrNull() ?: return false
            return try {
                resolver.openOutputStream(uri, "w")?.use { out ->
                    source.inputStream().buffered().use { it.copyTo(out) }
                } ?: error("openOutputStream returned null")
                resolver.update(uri, ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }, null, null)
                true
            } catch (_: Exception) {
                resolver.delete(uri, null, null)
                false
            }
        }

        override fun publishBitmap(bitmap: Bitmap, displayName: String): Boolean {
            val resolver = context.contentResolver
            val values =
                ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, displayName)
                    put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                    put(MediaStore.Images.Media.RELATIVE_PATH, relativePath)
                    put(MediaStore.Images.Media.IS_PENDING, 1)
                }
            val uri = runCatching { resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) }.getOrNull()
                ?: return false
            return try {
                resolver.openOutputStream(uri, "w")?.use { out ->
                    check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, out))
                } ?: error("openOutputStream returned null")
                resolver.update(uri, ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }, null, null)
                true
            } catch (_: Exception) {
                resolver.delete(uri, null, null)
                false
            }
        }
    }

    private inner class TreeTarget(private val dirUri: Uri) : ArtifactTarget {
        override fun publish(source: File, displayName: String, mime: String): Boolean {
            return runCatching {
                val resolver = context.contentResolver
                val existing =
                    findTreeChild(dirUri, displayName)?.also {
                        resolver.delete(it, null, null)
                    }
                val doc = DocumentsContract.createDocument(resolver, dirUri, mime, displayName) ?: return false
                resolver.openOutputStream(doc, "w")?.use { out ->
                    source.inputStream().buffered().use { it.copyTo(out) }
                } ?: error("openOutputStream returned null")
                true
            }.getOrDefault(false)
        }

        override fun publishBitmap(bitmap: Bitmap, displayName: String): Boolean {
            return runCatching {
                val resolver = context.contentResolver
                findTreeChild(dirUri, displayName)?.also { resolver.delete(it, null, null) }
                val doc =
                    DocumentsContract.createDocument(resolver, dirUri, "image/png", displayName) ?: return false
                resolver.openOutputStream(doc, "w")?.use { out ->
                    check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, out))
                } ?: error("openOutputStream returned null")
                true
            }.getOrDefault(false)
        }
    }

    private fun readNativeFailure(): String = File(privateFiles, ZSL_FAILED)
        .takeIf { it.isFile }
        ?.runCatching { readText().trim() }
        ?.getOrNull()
        .orEmpty()

    private enum class ZslWaitResult { Ready, Failed, Timeout }

    private fun waitForZslReady(timeoutMs: Long): ZslWaitResult {
        val ready = File(privateFiles, ZSL_READY)
        val failed = File(privateFiles, ZSL_FAILED)
        val deadline = android.os.SystemClock.elapsedRealtime() + timeoutMs.coerceAtLeast(0L)
        while (android.os.SystemClock.elapsedRealtime() < deadline) {
            if (ready.isFile) return ZslWaitResult.Ready
            if (failed.isFile) return ZslWaitResult.Failed
            try {
                Thread.sleep(50)
            } catch (_: InterruptedException) {
                return ZslWaitResult.Failed
            }
        }
        return when {
            ready.isFile -> ZslWaitResult.Ready
            failed.isFile -> ZslWaitResult.Failed
            else -> ZslWaitResult.Timeout
        }
    }

    private fun rgba16fToPng(source: File, manifest: File, displayName: String, target: ArtifactTarget) {
        // Dimensions come from the native manifest; any RAW size is valid.
        val fields =
            runCatching { manifest.readLines() }.getOrDefault(emptyList()).mapNotNull { line ->
                line.split('=', limit = 2).takeIf { it.size == 2 }?.let { it[0] to it[1] }
            }.toMap()
        val actualWidth = fields["width"]?.toIntOrNull() ?: return
        val height = fields["height"]?.toIntOrNull() ?: return
        if (actualWidth <= 0 || height <= 0 || source.length() != actualWidth.toLong() * height.toLong() * 8L) return
        val bitmap = createBitmap(actualWidth, height, Bitmap.Config.ARGB_8888)
        val rowBytes = ByteArray(actualWidth * 8)
        val pixels = IntArray(actualWidth)
        BufferedInputStream(FileInputStream(source), rowBytes.size * 2).use { input ->
            for (y in 0 until height) {
                if (!input.readFully(rowBytes)) return
                val bb = ByteBuffer.wrap(rowBytes).order(ByteOrder.LITTLE_ENDIAN)
                for (x in 0 until actualWidth) {
                    val r = halfBitsToFloat(bb.short.toInt() and 0xffff)
                    val g = halfBitsToFloat(bb.short.toInt() and 0xffff)
                    val b = halfBitsToFloat(bb.short.toInt() and 0xffff)
                    bb.getShort() // alpha
                    pixels[x] = (0xff shl 24) or (srgb8(r) shl 16) or (srgb8(g) shl 8) or srgb8(b)
                }
                bitmap.setPixels(pixels, 0, actualWidth, 0, y, actualWidth, 1)
            }
        }
        writeBitmapPng(bitmap, displayName, target)
        bitmap.recycle()
    }

    private fun ppmToPng(source: File, displayName: String, target: ArtifactTarget) {
        BufferedInputStream(FileInputStream(source), 64 * 1024).use { input ->
            val magic = nextToken(input)
            val width = nextToken(input).toIntOrNull() ?: return
            val height = nextToken(input).toIntOrNull() ?: return
            val max = nextToken(input).toIntOrNull() ?: return
            if (magic != "P6" || max != 255 || width <= 0 || height <= 0) return
            val bitmap = createBitmap(width, height, Bitmap.Config.ARGB_8888)
            val row = ByteArray(width * 3)
            val pixels = IntArray(width)
            for (y in 0 until height) {
                if (!input.readFully(row)) {
                    bitmap.recycle()
                    return
                }
                var i = 0
                for (x in 0 until width) {
                    val r = row[i++].toInt() and 0xff
                    val g = row[i++].toInt() and 0xff
                    val b = row[i++].toInt() and 0xff
                    pixels[x] = (0xff shl 24) or (r shl 16) or (g shl 8) or b
                }
                bitmap.setPixels(pixels, 0, width, 0, y, width, 1)
            }
            writeBitmapPng(bitmap, displayName, target)
            bitmap.recycle()
        }
    }

    private fun writeBitmapPng(bitmap: Bitmap, displayName: String, target: ArtifactTarget) {
        target.publishBitmap(bitmap, displayName)
    }

    /** Decode one IEEE-754 binary16 value from its raw unsigned 16-bit representation. */
    private fun halfBitsToFloat(bits: Int): Float {
        val sign = (bits ushr 15) and 0x1
        val exponent = (bits ushr 10) and 0x1f
        val fraction = bits and 0x03ff

        val floatBits =
            when (exponent) {
                0 -> {
                    if (fraction == 0) {
                        sign shl 31
                    } else {
                        // Normalize the binary16 subnormal before rebiasing it to binary32.
                        var mantissa = fraction
                        var shift = 0
                        while ((mantissa and 0x0400) == 0) {
                            mantissa = mantissa shl 1
                            shift++
                        }
                        mantissa = mantissa and 0x03ff
                        val floatExponent = 127 - 14 - shift
                        (sign shl 31) or (floatExponent shl 23) or (mantissa shl 13)
                    }
                }

                0x1f -> {
                    (sign shl 31) or 0x7f800000 or (fraction shl 13)
                }

                else -> {
                    val floatExponent = exponent + (127 - 15)
                    (sign shl 31) or (floatExponent shl 23) or (fraction shl 13)
                }
            }
        return Float.fromBits(floatBits)
    }

    private fun srgb8(linear: Float): Int {
        val x = if (linear.isFinite()) linear.coerceIn(0f, 1f) else 0f
        val y = if (x <= 0.0031308f) 12.92f * x else 1.055f * x.toDouble().pow(1.0 / 2.4).toFloat() - 0.055f
        return (y * 255f + 0.5f).toInt().coerceIn(0, 255)
    }

    private fun InputStream.readFully(dst: ByteArray): Boolean {
        var offset = 0
        while (offset < dst.size) {
            val n = read(dst, offset, dst.size - offset)
            if (n <= 0) return false
            offset += n
        }
        return true
    }

    private fun nextToken(input: InputStream): String {
        val out = StringBuilder()
        var c: Int
        do {
            c = input.read()
        } while (c >= 0 && c.toChar().isWhitespace())
        while (c >= 0 && !c.toChar().isWhitespace()) {
            out.append(c.toChar())
            c = input.read()
        }
        return out.toString()
    }

    companion object {
        private const val POST_TONEMAP = "rawrcam_prejpeg_latest.ppm"
        private const val PACKED_CFA = "rawrcam_diag_packed_cfa_latest.rgba16f"
        private const val DEMOSAIC_RGB = "rawrcam_diag_demosaic_latest.rgba16f"
        private const val DUAL_MASK = "rawrcam_diag_dual_mask_latest.r32f"
        private const val PRE_WB_RGB = "rawrcam_diag_pre_wb_latest.rgba16f"
        private const val PRE_FCC_RGB = "rawrcam_diag_pre_fcc_latest.rgba16f"
        private const val PRE_TONEMAP_RGB = "rawrcam_diag_pre_tonemap_latest.rgba16f"
        private const val MANIFEST = "rawrcam_diag_manifest_latest.txt"
        private const val ZSL_BUNDLE = "rawrcam_zsl_bundle_latest.rzsl"
        private const val DEBUG_FOLDER = "debug"
        private const val ZSL_FOLDER = "rzsl"
        private const val ZSL_READY = "rawrcam_zsl_bundle_latest.ready"
        private const val ZSL_FAILED = "rawrcam_zsl_bundle_latest.failed"
        private val PRIVATE_ARTIFACTS =
            listOf(
                POST_TONEMAP,
                PACKED_CFA,
                DEMOSAIC_RGB,
                DUAL_MASK,
                PRE_WB_RGB,
                PRE_FCC_RGB,
                PRE_TONEMAP_RGB,
                MANIFEST
            )
        private val DIAGNOSTIC_BUNDLE_FILES =
            listOf(
                "rawrcam_validation_current.txt",
                "rawrcam_dcg_session_audit.jsonl",
                "rawrcam_post_session_pipeline_audit.jsonl",
                "rawrcam_frame_audit.jsonl",
                "rawrcam_internal_runtime_trace.txt"
            )
    }
}
