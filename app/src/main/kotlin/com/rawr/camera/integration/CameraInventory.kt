package com.rawr.camera.integration

import android.content.Context
import android.hardware.camera2.CameraManager
import com.rawr.camera.settings.model.RawStreamFormat
import com.rawr.camera.settings.model.VendorKeyType
import org.json.JSONArray
import org.json.JSONObject

enum class CameraFacing(val label: String) {
    Back("Back"),
    Front("Front"),
    External("External"),
    Unknown("Other");

    companion object {
        // ACAMERA_LENS_FACING: FRONT=0, BACK=1, EXTERNAL=2.
        fun fromNative(value: Int): CameraFacing = when (value) {
            0 -> Front
            1 -> Back
            2 -> External
            else -> Unknown
        }
    }
}

data class ProbedRawStream(
    /** Camera2 name: RAW_SENSOR, RAW10, RAW12 or RAW_PRIVATE. */
    val formatName: String,
    val width: Int,
    val height: Int,
    /** The capture pipeline can consume this stream. */
    val supported: Boolean
) {
    val format: RawStreamFormat? get() = when (formatName) {
        "RAW_SENSOR" -> RawStreamFormat.Raw16
        "RAW10" -> RawStreamFormat.Raw10
        else -> null
    }
    val label: String get() = "$formatName ${width}×$height"
}

data class ProbedCamera(
    val id: String,
    /** Logical camera this physical sub-camera belongs to; null for openable ids. */
    val parentId: String?,
    val enumerated: Boolean,
    val logical: Boolean,
    val physicalIds: List<String>,
    val facing: CameraFacing,
    val focalLengthsMm: List<Float>,
    val equivalentFocalMm: Float,
    val rawStreams: List<ProbedRawStream>,
    val blackLevels: List<Int>,
    val whiteLevel: Int,
    val sessionKeyTags: Set<Long>
) {
    /** Id to open: the logical parent for physical sub-cameras. */
    val openId: String get() = parentId ?: id
    val physicalId: String get() = if (parentId != null) id else ""
}

data class CameraInventory(val cameras: List<ProbedCamera>, val error: String? = null) {
    /** Cameras grouped by facing (back first), each group sorted by focal length. */
    fun grouped(): List<Pair<CameraFacing, List<ProbedCamera>>> = cameras
        .groupBy { it.facing }
        .toSortedMap(compareBy { it.ordinal })
        .map { (facing, list) ->
            facing to list.sortedWith(compareBy<ProbedCamera> { it.equivalentFocalMm.takeIf { f -> f > 0f } ?: Float.MAX_VALUE }
                .thenBy { it.id.toIntOrNull() ?: Int.MAX_VALUE }.thenBy { it.id })
        }

    fun find(cameraId: String, physicalCameraId: String): ProbedCamera? =
        if (physicalCameraId.isEmpty()) {
            cameras.firstOrNull { it.id == cameraId && it.parentId == null }
        } else {
            cameras.firstOrNull { it.id == physicalCameraId && it.parentId == cameraId }
        }
}

/** One request key the user can pick, merged from Camera2 key names and the native default request. */
data class CameraKeyInfo(
    val name: String?,
    val tag: Long?,
    /** Null when the key isn't in the default request and its type can't be detected. */
    val type: VendorKeyType?,
    val defaults: List<Double>,
    val sessionKey: Boolean,
    /** Reported type the app can't set (e.g. rational). */
    val unsupportedType: String? = null
) {
    /** Value stored in [com.rawr.camera.settings.model.VendorKey.tag]. */
    val reference: String get() = name ?: "0x" + java.lang.Long.toHexString(tag ?: 0L)
}

data class CameraKeyCatalog(val keys: List<CameraKeyInfo>, val error: String? = null) {
    fun find(reference: String): CameraKeyInfo? = keys.firstOrNull { it.name == reference } ?: run {
        val tag = parseTag(reference) ?: return null
        keys.firstOrNull { it.tag == tag }
    }

    fun search(query: String): List<CameraKeyInfo> {
        val q = query.trim()
        if (q.isEmpty()) return keys
        return keys.filter { key ->
            key.name?.contains(q, ignoreCase = true) == true ||
                key.tag?.let { "0x" + java.lang.Long.toHexString(it) }?.contains(q, ignoreCase = true) == true ||
                key.type?.label?.contains(q, ignoreCase = true) == true
        }
    }

    companion object {
        fun parseTag(text: String): Long? {
            val t = text.trim()
            return when {
                t.startsWith("0x", ignoreCase = true) -> t.substring(2).toLongOrNull(16)
                t.isNotEmpty() && t.all { it.isDigit() } -> t.toLongOrNull()
                else -> null
            }
        }
    }
}

/** Parsers for the native CameraProbe JSON. */
object CameraInventoryParser {
    fun parseCameras(json: String): CameraInventory = runCatching {
        val root = JSONObject(json)
        val cameras = root.optJSONArray("cameras").objects().map { c ->
            ProbedCamera(
                id = c.getString("id"),
                parentId = c.optString("parentId").ifEmpty { null },
                enumerated = c.optBoolean("enumerated"),
                logical = c.optBoolean("logical"),
                physicalIds = c.optJSONArray("physicalIds").strings(),
                facing = CameraFacing.fromNative(c.optInt("facing", -1)),
                focalLengthsMm = c.optJSONArray("focalLengths").doubles().map { it.toFloat() },
                equivalentFocalMm = c.optDouble("equivalentFocalMm", 0.0).toFloat(),
                rawStreams = c.optJSONArray("rawStreams").objects().map { s ->
                    ProbedRawStream(s.getString("format"), s.getInt("width"), s.getInt("height"), s.optBoolean("supported"))
                },
                blackLevels = c.optJSONArray("black").doubles().map { it.toInt() },
                whiteLevel = c.optInt("white"),
                sessionKeyTags = c.optJSONArray("sessionKeys").doubles().map { it.toLong() }.toSet()
            )
        }
        CameraInventory(cameras, root.optString("error").ifEmpty { null })
    }.getOrElse { CameraInventory(emptyList(), "Camera probe failed: ${it.message}") }

    /**
     * Merges the native key probe with Camera2 key [names] (request, session
     * and physical request keys). Names come first, sorted; tags in the
     * default request without a known name follow as hex ids.
     */
    fun parseKeys(json: String, names: Collection<String>, sessionNames: Set<String>): CameraKeyCatalog = runCatching {
        val root = JSONObject(json)
        val error = root.optString("error").ifEmpty { null }
        if (error != null) return CameraKeyCatalog(emptyList(), error)
        data class Entry(val type: String, val values: List<Double>)
        val byTag = root.optJSONArray("tags").objects().associate { t ->
            t.getLong("tag") to Entry(t.optString("type"), t.optJSONArray("values").doubles())
        }
        val tagOfName = root.optJSONArray("names").objects().associate { n ->
            n.getString("name") to (if (n.isNull("tag")) null else n.getLong("tag"))
        }
        val sessionTags = root.optJSONArray("sessionKeys").doubles().map { it.toLong() }.toSet()
        fun info(name: String?, tag: Long?): CameraKeyInfo {
            val entry = tag?.let { byTag[it] }
            val type = entry?.let { VendorKeyType.fromWire(it.type) }
            return CameraKeyInfo(
                name = name,
                tag = tag,
                type = type,
                defaults = entry?.values.orEmpty(),
                sessionKey = (tag != null && tag in sessionTags) || (name != null && name in sessionNames),
                unsupportedType = entry?.type?.takeIf { type == null }
            )
        }
        val named = names.distinct().sorted().map { name -> info(name, tagOfName[name]) }
        val namedTags = named.mapNotNull { it.tag }.toSet()
        val unnamed = byTag.keys.filter { it !in namedTags }.sorted().map { info(null, it) }
        CameraKeyCatalog(named + unnamed)
    }.getOrElse { CameraKeyCatalog(emptyList(), "Key probe failed: ${it.message}") }

    private fun JSONArray?.objects(): List<JSONObject> =
        if (this == null) emptyList() else (0 until length()).map { getJSONObject(it) }

    private fun JSONArray?.strings(): List<String> =
        if (this == null) emptyList() else (0 until length()).map { getString(it) }

    private fun JSONArray?.doubles(): List<Double> =
        if (this == null) emptyList() else (0 until length()).map { getDouble(it) }
}

/** Runs the native probes and gathers Camera2 key names. Blocking: call off the main thread. */
class CameraInventorySource(context: Context) {
    private val manager = context.applicationContext.getSystemService(CameraManager::class.java)
    private val native = NativePreviewEngine()

    init { native.installCameraTagResolver(CameraTagResolver(context)) }

    fun cameras(): CameraInventory = CameraInventoryParser.parseCameras(native.probeCameras())

    fun keys(cameraId: String, physicalCameraId: String): CameraKeyCatalog {
        val (names, sessionNames) = keyNames(cameraId, physicalCameraId)
        val json = native.probeCameraKeys(cameraId, names.toTypedArray())
        return CameraInventoryParser.parseKeys(json, names, sessionNames)
    }

    // Hidden ids may be invisible to the Java CameraManager; the native probe
    // still reports their default-request tags.
    private fun keyNames(cameraId: String, physicalCameraId: String): Pair<List<String>, Set<String>> = runCatching {
        val chars = manager.getCameraCharacteristics(cameraId)
        val request = chars.availableCaptureRequestKeys.map { it.name }
        val session = chars.availableSessionKeys.orEmpty().map { it.name }
        val physical =
            if (physicalCameraId.isEmpty()) emptyList()
            else chars.availablePhysicalCameraRequestKeys.orEmpty().map { it.name }
        (request + session + physical).distinct() to session.toSet()
    }.getOrElse { emptyList<String>() to emptySet() }
}
