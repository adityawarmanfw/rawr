package com.rawr.camera.renderer.library

import android.content.ContentUris
import android.content.Context
import android.database.ContentObserver
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject

/** One DNG in MediaStore, with what the native probe found. */
data class DngEntry(
    val id: Long,
    val uri: Uri,
    val name: String,
    val relativePath: String,
    val size: Long,
    val modified: Long,
    val taken: Long,
    val width: Int = 0,
    val height: Int = 0,
    val orientation: Int = 1,
    val rawr: Boolean = false,
    val convertible: Boolean = false,
    val model: String = "",
    /** EXIF DateTimeOriginal and OffsetTimeOriginal as written, may be empty. */
    val dateTimeOriginal: String = "",
    val offsetTimeOriginal: String = "",
    val error: String = "",
    /** Output sizes (MP, as in the file suffix) already converted from this file. */
    val converted: Set<Int> = emptySet(),
    /** This file is itself a converter output (…_RAWR<mp>.dng). */
    val conversionOutput: Boolean = false
) {
    val megapixels: Double get() = width.toDouble() * height / 1e6
    val highRes: Boolean get() = width.toLong() * height >= HIGH_RES_PIXELS && !conversionOutput

    companion object {
        const val HIGH_RES_PIXELS = 150_000_000L
    }
}

/** Conversion targets; mode matches cpp/jni/ConvertJni.cpp. */
enum class DngConversion(val nativeMode: Int, val label: String, val pixelFraction: Double) {
    Lossless(0, "Lossless", 1.0),
    Quarter(1, "1/4", 0.25),
    Eighth(2, "1/8", 0.125),
    Sixteenth(3, "1/16", 0.0625);

    /** Megapixels in the output name, e.g. 200MP -> 50 for [Quarter]. */
    /** Rounded down so a 200.5MP source names its outputs _RAWR200, _RAWR50, _RAWR25, _RAWR12. */
    fun outputMegapixels(entry: DngEntry): Int = (entry.megapixels * pixelFraction).toInt().coerceAtLeast(1)
}

/**
 * MediaStore DNG library: scans for DNGs, probes each new or changed file
 * once (cached by id, size and modification time), produces RAW thumbnails
 * (cached on disk), and rescans when MediaStore changes.
 */
class DngLibrary private constructor(private val context: Context) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val scanLock = Mutex()
    // RAW thumbnails decode whole tile rows; two at a time bounds memory.
    private val thumbnailSlots = Semaphore(2)
    private val indexFile = File(context.filesDir, "dng_library/index.json")
    private val thumbDir = File(context.cacheDir, "dng_thumbs")
    private val probes = ConcurrentHashMap<Long, JSONObject>()
    private val mutableEntries = MutableStateFlow<List<DngEntry>>(emptyList())
    private val mutableScanning = MutableStateFlow(false)
    val entries: StateFlow<List<DngEntry>> = mutableEntries.asStateFlow()
    val scanning: StateFlow<Boolean> = mutableScanning.asStateFlow()
    private var observer: ContentObserver? = null
    private var pendingScan: Job? = null

    init {
        runCatching {
            val json = JSONObject(indexFile.readText())
            json.keys().forEach { key -> probes[key.toLong()] = json.getJSONObject(key) }
        }
    }

    /** Starts auto scanning (idempotent): one scan now, then on MediaStore changes. */
    fun watch() {
        if (observer == null) {
            observer = object : ContentObserver(Handler(Looper.getMainLooper())) {
                override fun onChange(selfChange: Boolean) = requestScan(delayMs = 1500)
            }.also {
                context.contentResolver.registerContentObserver(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, true, it)
            }
        }
        requestScan()
    }

    fun requestScan(delayMs: Long = 0) {
        pendingScan?.cancel()
        pendingScan = scope.launch {
            if (delayMs > 0) delay(delayMs)
            scan()
        }
    }

    suspend fun scan() = scanLock.withLock {
        mutableScanning.value = true
        try {
            val rows = query()
            // Publish the list immediately, then fill in probes as they finish.
            mutableEntries.value = withOutputs(rows.map { it.withProbe(probes[it.id], it) })
            var changed = false
            for (row in rows) {
                val cached = probes[row.id]
                if (cached != null && cached.optLong("size") == row.size && cached.optLong("modified") == row.modified &&
                    cached.optInt("version") == PROBE_VERSION) continue
                probes[row.id] = probe(row)
                changed = true
                mutableEntries.value = withOutputs(rows.map { it.withProbe(probes[it.id], it) })
            }
            val live = rows.map { it.id }.toSet()
            if (probes.keys.retainAll(live)) changed = true
            if (changed) saveIndex()
        } finally {
            mutableScanning.value = false
        }
    }

    /** Cached RAW thumbnail, rotated for display; generated on first use. */
    suspend fun thumbnail(entry: DngEntry, maxSide: Int = 256): Bitmap? = withContext(Dispatchers.IO) {
        val file = File(thumbDir, "${entry.id}_${entry.modified}_$maxSide.jpg")
        BitmapFactory.decodeFile(file.path)?.let { return@withContext it }
        thumbnailSlots.withPermit { BitmapFactory.decodeFile(file.path) ?: render(entry, maxSide, file) }
    }

    /** Uncached RAW thumbnail of any DNG URI, rotated for display (capture screen). */
    suspend fun thumbnail(uri: Uri, maxSide: Int = 256): Bitmap? = withContext(Dispatchers.IO) {
        thumbnailSlots.withPermit {
            runCatching {
                context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                    val path = "/proc/self/fd/${pfd.fd}"
                    val orientation = ConvertNative.probe(path).split('\n').let { if (it.firstOrNull() == "ok") it.getOrNull(5)?.toIntOrNull() else null } ?: 1
                    val pixels = ConvertNative.thumbnail(path, maxSide) ?: return@use null
                    orient(Bitmap.createBitmap(pixels, 2, pixels[0], pixels[0], pixels[1], Bitmap.Config.ARGB_8888), orientation)
                }
            }.getOrNull()
        }
    }

    private fun render(entry: DngEntry, maxSide: Int, file: File): Bitmap? {
        val pixels = runCatching {
            context.contentResolver.openFileDescriptor(entry.uri, "r")?.use { pfd ->
                ConvertNative.thumbnail("/proc/self/fd/${pfd.fd}", maxSide)
            }
        }.getOrNull() ?: return null
        val bitmap = Bitmap.createBitmap(pixels, 2, pixels[0], pixels[0], pixels[1], Bitmap.Config.ARGB_8888)
        val rotated = orient(bitmap, entry.orientation)
        runCatching {
            thumbDir.mkdirs()
            val temp = File(thumbDir, file.name + ".tmp")
            temp.outputStream().use { rotated.compress(Bitmap.CompressFormat.JPEG, 88, it) }
            temp.renameTo(file)
        }
        return rotated
    }

    private fun query(): List<DngEntry> {
        val collection = MediaStore.Images.Media.EXTERNAL_CONTENT_URI
        val projection = arrayOf(
            MediaStore.Images.Media._ID,
            MediaStore.Images.Media.DISPLAY_NAME,
            MediaStore.Images.Media.RELATIVE_PATH,
            MediaStore.Images.Media.SIZE,
            MediaStore.Images.Media.DATE_MODIFIED,
            MediaStore.Images.Media.DATE_TAKEN
        )
        val selection = "(${MediaStore.Images.Media.MIME_TYPE} IN (?, ?, ?) OR " +
            "${MediaStore.Images.Media.DISPLAY_NAME} LIKE ?) AND ${MediaStore.Images.Media.IS_PENDING} = 0"
        val args = arrayOf("image/x-adobe-dng", "image/dng", "image/x-dng", "%.dng")
        val order = "${MediaStore.Images.Media.DATE_TAKEN} DESC, ${MediaStore.Images.Media._ID} DESC"
        return context.contentResolver.query(collection, projection, selection, args, order)?.use { c ->
            buildList {
                while (c.moveToNext()) {
                    val id = c.getLong(0)
                    add(DngEntry(
                        id = id,
                        uri = ContentUris.withAppendedId(collection, id),
                        name = c.getString(1) ?: "DNG",
                        relativePath = c.getString(2) ?: "",
                        size = c.getLong(3),
                        modified = c.getLong(4),
                        taken = c.getLong(5)
                    ))
                }
            }
        } ?: emptyList()
    }

    private fun probe(row: DngEntry): JSONObject {
        val result = runCatching {
            context.contentResolver.openFileDescriptor(row.uri, "r")?.use { pfd ->
                ConvertNative.probe("/proc/self/fd/${pfd.fd}")
            } ?: "error\nCannot open file"
        }.getOrElse { "error\n${it.message}" }.split('\n')
        return JSONObject().apply {
            put("version", PROBE_VERSION)
            put("size", row.size)
            put("modified", row.modified)
            if (result.firstOrNull() == "ok" && result.size >= 10) {
                put("width", result[1].toInt())
                put("height", result[2].toInt())
                put("orientation", result[5].toInt())
                put("rawr", result[6] == "1")
                put("convertible", result[7] == "1")
                put("model", listOf(result[8], result[9]).filter { it.isNotBlank() }.let { parts ->
                    // Avoid "vivo vivo X300 Ultra".
                    if (parts.size == 2 && parts[1].startsWith(parts[0], ignoreCase = true)) parts[1] else parts.joinToString(" ")
                })
                put("dateTimeOriginal", result.getOrNull(10).orEmpty())
                put("offsetTimeOriginal", result.getOrNull(11).orEmpty())
            } else {
                put("error", result.getOrNull(1) ?: "Cannot read DNG")
            }
        }
    }

    private fun DngEntry.withProbe(json: JSONObject?, row: DngEntry): DngEntry {
        if (json == null || json.optLong("size") != row.size || json.optLong("modified") != row.modified) return this
        return copy(
            width = json.optInt("width"),
            height = json.optInt("height"),
            orientation = json.optInt("orientation", 1),
            rawr = json.optBoolean("rawr"),
            convertible = json.optBoolean("convertible"),
            model = json.optString("model"),
            dateTimeOriginal = json.optString("dateTimeOriginal"),
            offsetTimeOriginal = json.optString("offsetTimeOriginal"),
            error = json.optString("error")
        )
    }

    /** Marks converter outputs and records, per source, which sizes exist. */
    private fun withOutputs(list: List<DngEntry>): List<DngEntry> {
        val outputs = HashMap<String, MutableSet<Int>>()
        for (entry in list) {
            val match = OUTPUT_NAME.matchEntire(entry.name) ?: continue
            outputs.getOrPut(entry.relativePath + match.groupValues[1].lowercase()) { mutableSetOf() } +=
                match.groupValues[2].toInt()
        }
        return list.map { entry ->
            val output = OUTPUT_NAME.matches(entry.name)
            val converted = outputs[entry.relativePath + baseName(entry.name).lowercase()].orEmpty()
            if (output || converted.isNotEmpty()) entry.copy(converted = converted, conversionOutput = output) else entry
        }
    }

    private fun saveIndex() {
        runCatching {
            indexFile.parentFile?.mkdirs()
            val json = JSONObject()
            probes.forEach { (id, probe) -> json.put(id.toString(), probe) }
            val temp = File(indexFile.path + ".tmp")
            temp.writeText(json.toString())
            temp.renameTo(indexFile)
        }
    }

    companion object {
        /** Bump when the probe JSON gains fields so cached entries are re-probed. */
        private const val PROBE_VERSION = 2

        /** Converter output name: <base>_RAWR<mp>.dng */
        val OUTPUT_NAME = Regex("(.+)_RAWR(\\d+)\\.dng", RegexOption.IGNORE_CASE)

        fun baseName(name: String): String = name.substringBeforeLast('.')

        fun outputName(entry: DngEntry, conversion: DngConversion): String =
            "${baseName(entry.name)}_RAWR${conversion.outputMegapixels(entry)}.dng"

        @Volatile private var instance: DngLibrary? = null

        fun get(context: Context): DngLibrary =
            instance ?: synchronized(this) { instance ?: DngLibrary(context.applicationContext).also { instance = it } }

        internal fun orient(bitmap: Bitmap, orientation: Int): Bitmap {
            val m = Matrix()
            when (orientation) {
                2 -> m.setScale(-1f, 1f)
                3 -> m.setRotate(180f)
                4 -> m.setScale(1f, -1f)
                5 -> { m.setRotate(90f); m.postScale(-1f, 1f) }
                6 -> m.setRotate(90f)
                7 -> { m.setRotate(-90f); m.postScale(-1f, 1f) }
                8 -> m.setRotate(-90f)
                else -> return bitmap
            }
            return Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, m, true)
        }
    }
}
