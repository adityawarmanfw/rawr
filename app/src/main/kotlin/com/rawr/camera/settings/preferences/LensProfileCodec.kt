package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.validateVendorKey
import com.rawr.camera.settings.model.CFA_BLOCKS
import com.rawr.camera.settings.model.CFA_LIVE_QUALITIES
import com.rawr.camera.settings.model.CFA_ORDERS
import com.rawr.camera.settings.model.CFA_PHOTO_QUALITIES
import com.rawr.camera.settings.model.EXTENDED_ISO_MAX
import com.rawr.camera.settings.model.EXTENDED_SHUTTER_MAX_NS
import com.rawr.camera.settings.model.LensDcp
import com.rawr.camera.settings.model.LensExposure
import com.rawr.camera.settings.model.LensLevels
import com.rawr.camera.settings.model.LensMirror
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.RawStreamChoice
import com.rawr.camera.settings.model.RawStreamFormat
import com.rawr.camera.settings.model.VendorKey
import com.rawr.camera.settings.model.VendorKeyScope
import com.rawr.camera.settings.model.VendorKeyType
import org.json.JSONArray
import org.json.JSONObject

/**
 * JSON form of the lens profiles, shared with the native camera profile
 * (camera/CameraProfileJson.h). Stored as-is in settings, with each lens's
 * `enabled` flag; the camera receives only the enabled lenses.
 */
internal object LensProfileCodec {
    const val PROFILE_ID = "user"

    /** [dcpDir]: the camera profile directory, for the native form (each "dcp" gets its absolute "path"). */
    fun encode(lenses: List<LensProfile>, enabledOnly: Boolean = false, dcpDir: java.io.File? = null): String {
        val array = JSONArray()
        lenses.filter { !enabledOnly || it.enabled }.forEach { array.put(encodeLens(it, dcpDir)) }
        return JSONObject().put("id", PROFILE_ID).put("lenses", array).toString()
    }

    /** Null for a missing or unreadable document; unreadable lenses/keys are dropped. */
    fun decode(raw: String?): List<LensProfile>? {
        if (raw.isNullOrBlank()) return null
        val root = runCatching { JSONObject(raw) }.getOrNull() ?: return null
        val lenses = root.optJSONArray("lenses") ?: return null
        return (0 until lenses.length()).mapNotNull { i -> runCatching { decodeLens(lenses.getJSONObject(i)) }.getOrNull() }
    }

    /** Backup import must reject damaged nested records rather than dropping them. */
    fun decodeBackup(raw: String): List<LensProfile> = try {
        val lenses = JSONObject(raw).getJSONArray("lenses")
        (0 until lenses.length()).map { index ->
            val lens = lenses.getJSONObject(index)
            validateBackupLens(lens)
            decodeLens(lens)
        }
    } catch (failure: Exception) {
        throw IllegalArgumentException("Backup contains malformed lens profiles", failure)
    }

    private fun objectField(o: JSONObject, name: String): JSONObject? =
        if (o.has(name)) o.getJSONObject(name) else null

    private fun number(o: JSONObject, name: String, range: ClosedFloatingPointRange<Double>, integral: Boolean = false) {
        if (!o.has(name)) return
        val value = o.get(name)
        require(value is Number && value.toDouble().isFinite() && value.toDouble() in range &&
            (!integral || value.toDouble() == Math.rint(value.toDouble()))) { "Invalid $name" }
    }

    private fun choice(o: JSONObject, name: String, choices: List<String>) {
        if (o.has(name)) require(o.get(name) is String && o.getString(name) in choices) { "Invalid $name" }
    }

    private fun validateBackupLens(o: JSONObject) {
        for (name in listOf("id", "cameraId")) require(o.get(name) is String && o.getString(name).isNotBlank())
        for (name in listOf("enabled", "captureBurstSequential")) {
            if (o.has(name)) require(o.get(name) is Boolean)
        }
        objectField(o, "dcp")?.let { dcp ->
            require(dcp.get("file") is String && DcpFileStore.isFileName(dcp.getString("file")))
            if (dcp.has("name")) require(dcp.get("name") is String)
        }
        fun keys(parent: JSONObject) {
            if (!parent.has("keys")) return
            val array = parent.getJSONArray("keys")
            for (i in 0 until array.length()) {
                val key = array.getJSONObject(i)
                require(key.get("tag") is String && key.getString("tag").isNotBlank())
                choice(key, "type", VendorKeyType.entries.map { it.wireName })
                choice(key, "scope", VendorKeyScope.entries.map { it.wireName })
                if (key.has("enabled")) require(key.get("enabled") is Boolean)
                val values = key.getJSONArray("values")
                require(values.length() > 0)
                for (j in 0 until values.length()) {
                    val value = values.get(j)
                    require(value is Number && value.toDouble().isFinite())
                }
                val decoded = decodeKeys(JSONArray().put(key)).single()
                require(validateVendorKey(decoded) == null)
            }
        }
        fun levels(parent: JSONObject) {
            objectField(parent, "levels")?.let { levels ->
                if (levels.has("static")) require(levels.get("static") is Boolean)
                if (levels.has("black")) {
                    val black = levels.getJSONArray("black")
                    require(black.length() == 1 || black.length() == 4)
                    for (i in 0 until black.length()) {
                        val value = black.get(i)
                        require(value is Number && value.toDouble().isFinite() && value.toDouble() >= 0)
                    }
                }
                number(levels, "white", 0.0..Float.MAX_VALUE.toDouble())
            }
        }
        keys(o)
        levels(o)
        objectField(o, "dcg")?.let { keys(it); levels(it) }
        for (field in listOf("stream", "captureStream")) {
            objectField(o, field)?.let { stream ->
                choice(stream, "format", RawStreamFormat.entries.map { it.wireName })
                val min = if (field == "captureStream") 1.0 else 0.0
                number(stream, "width", min..Int.MAX_VALUE.toDouble(), true)
                number(stream, "height", min..Int.MAX_VALUE.toDouble(), true)
                if (field == "captureStream") require(stream.has("width") && stream.has("height"))
                if (stream.has("unlisted")) require(stream.get("unlisted") is Boolean)
            }
        }
        objectField(o, "exposure")?.let { exposure ->
            number(exposure, "isoMax", 0.0..EXTENDED_ISO_MAX.toDouble(), true)
            number(exposure, "shutterMaxNs", 0.0..EXTENDED_SHUTTER_MAX_NS.toDouble(), true)
            if (exposure.has("extended")) require(exposure.get("extended") is Boolean)
        }
        objectField(o, "cfa")?.let { cfa ->
            if (cfa.has("block")) {
                number(cfa, "block", 1.0..Int.MAX_VALUE.toDouble(), true)
                require(cfa.getInt("block") in CFA_BLOCKS)
            }
            choice(cfa, "order", CFA_ORDERS.toList())
            choice(cfa, "live", CFA_LIVE_QUALITIES.toList())
            choice(cfa, "photo", CFA_PHOTO_QUALITIES.toList())
        }
        objectField(o, "distortion")
        choice(o, "mirror", LensMirror.entries.map { it.wireName })
        number(o, "captureBurst", 0.0..30.0, true)
    }

    private fun encodeLens(lens: LensProfile, dcpDir: java.io.File?): JSONObject = JSONObject()
        .put("id", lens.name.trim())
        .put("enabled", lens.enabled)
        .put("cameraId", lens.cameraId)
        .put("physicalCameraId", lens.physicalCameraId)
        .put(
            "stream",
            JSONObject()
                .put("format", lens.stream.format.wireName)
                .put("width", lens.stream.width.coerceAtLeast(0))
                .put("height", lens.stream.height.coerceAtLeast(0))
                .apply { if (lens.stream.unlisted) put("unlisted", true) }
        )
        .put("levels", encodeLevels(lens.levels))
        .put(
            "exposure",
            JSONObject()
                .put("extended", lens.exposure.extended)
                .put("isoMax", lens.exposure.isoMax)
                .put("shutterMaxNs", lens.exposure.shutterMaxNs)
        )
        .put("keys", encodeKeys(lens.vendorKeys))
        .apply {
            lens.readerUsage?.let { put("readerUsage", it) }
            if (lens.vendorGate.isNotEmpty()) put("vendorGate", lens.vendorGate)
            lens.captureStream?.let {
                put(
                    "captureStream",
                    JSONObject().put("format", it.format.wireName).put("width", it.width).put("height", it.height)
                )
            }
            if (lens.captureBurst > 0) put("captureBurst", lens.captureBurst)
            if (lens.captureBurstSequential) put("captureBurstSequential", true)
            if (lens.cfaBlock > 1 || lens.cfaOrder != null) {
                put(
                    "cfa",
                    JSONObject().put("block", lens.cfaBlock).put("live", lens.cfaLive).put("photo", lens.cfaPhoto)
                        .apply { lens.cfaOrder?.let { put("order", it) } }
                )
            }
            lens.distortion?.let { put("distortion", JSONObject(it)) }
            if (lens.mirror != LensMirror.None) put("mirror", lens.mirror.wireName)
            lens.dcp?.let { dcp ->
                put(
                    "dcp",
                    JSONObject().put("file", dcp.file).put("name", dcp.name)
                        .apply { dcpDir?.let { put("path", java.io.File(it, dcp.file).absolutePath) } }
                )
            }
        }

    private fun decodeLens(o: JSONObject): LensProfile {
        val stream = o.optJSONObject("stream")
        val levels = decodeLevels(o.optJSONObject("levels"))
        // Early builds kept DCG keys/levels in a separate "dcg" block; fold
        // them into the lens (its levels win when they are already static).
        val legacyDcg = o.optJSONObject("dcg")
        val legacyLevels = legacyDcg?.let { decodeLevels(it.optJSONObject("levels")) }
        return LensProfile(
            name = o.getString("id"),
            cameraId = o.getString("cameraId"),
            enabled = o.optBoolean("enabled", true),
            physicalCameraId = o.optString("physicalCameraId"),
            stream =
                RawStreamChoice(
                    format = RawStreamFormat.fromWire(stream?.optString("format")),
                    width = stream?.optInt("width") ?: 0,
                    height = stream?.optInt("height") ?: 0,
                    unlisted = stream?.optBoolean("unlisted") ?: false
                ),
            exposure = o.optJSONObject("exposure").let { e ->
                LensExposure(
                    extended = e?.optBoolean("extended") ?: false,
                    isoMax = (e?.optInt("isoMax") ?: 0).coerceIn(0, EXTENDED_ISO_MAX),
                    shutterMaxNs = (e?.optLong("shutterMaxNs") ?: 0L).coerceIn(0L, EXTENDED_SHUTTER_MAX_NS)
                )
            },
            levels = if (!levels.isStatic && legacyLevels?.isStatic == true) legacyLevels else levels,
            vendorKeys = decodeKeys(o.optJSONArray("keys")) + decodeKeys(legacyDcg?.optJSONArray("keys")),
            readerUsage = o.optString("readerUsage").ifEmpty { null },
            vendorGate = o.optString("vendorGate"),
            captureStream = o.optJSONObject("captureStream")?.let {
                RawStreamChoice(RawStreamFormat.fromWire(it.optString("format")), it.optInt("width"), it.optInt("height"))
            }?.takeIf { it.width > 0 && it.height > 0 },
            captureBurst = o.optInt("captureBurst").coerceIn(0, 30),
            captureBurstSequential = o.optBoolean("captureBurstSequential"),
            cfaBlock = o.optJSONObject("cfa")?.optInt("block", 1)?.takeIf { it in CFA_BLOCKS } ?: 1,
            cfaOrder = o.optJSONObject("cfa")?.optString("order")?.takeIf { it in CFA_ORDERS },
            cfaLive = o.optJSONObject("cfa")?.optString("live")?.takeIf { it in CFA_LIVE_QUALITIES } ?: "standard",
            cfaPhoto = o.optJSONObject("cfa")?.optString("photo")?.takeIf { it in CFA_PHOTO_QUALITIES } ?: "high",
            distortion = o.optJSONObject("distortion")?.toString(),
            mirror = LensMirror.fromWire(o.optString("mirror")),
            dcp = o.optJSONObject("dcp")?.let { d ->
                d.optString("file").takeIf { DcpFileStore.isFileName(it) }?.let { LensDcp(it, d.optString("name")) }
            }
        )
    }

    private fun encodeLevels(levels: LensLevels): JSONObject = JSONObject()
        .put("static", levels.isStatic)
        .put("black", JSONArray().apply { levels.blackRggb.forEach { put(it.toDouble()) } })
        .put("white", levels.white.toDouble())

    private fun decodeLevels(o: JSONObject?): LensLevels {
        if (o == null) return LensLevels()
        val black = o.optJSONArray("black")
        val values = if (black == null) emptyList() else (0 until black.length()).map { black.getDouble(it).toFloat() }
        return LensLevels(
            isStatic = o.optBoolean("static"),
            blackRggb =
                when (values.size) {
                    1 -> List(4) { values[0] }
                    4 -> values
                    else -> listOf(0f, 0f, 0f, 0f)
                },
            white = o.optDouble("white", 0.0).toFloat()
        )
    }

    private fun encodeKeys(keys: List<VendorKey>): JSONArray = JSONArray().apply {
        keys.forEach { key ->
            put(
                JSONObject()
                    .put("tag", key.tag.trim())
                    .put("type", key.type.wireName)
                    .put("scope", key.scope.wireName)
                    .put("values", JSONArray().apply { key.values.forEach { put(it) } })
                    .put("enabled", key.enabled)
            )
        }
    }

    private fun decodeKeys(array: JSONArray?): List<VendorKey> {
        if (array == null) return emptyList()
        return (0 until array.length()).mapNotNull { i ->
            runCatching {
                val o = array.getJSONObject(i)
                val values = o.optJSONArray("values")
                VendorKey(
                    tag = o.getString("tag").trim(),
                    scope = VendorKeyScope.fromWire(o.optString("scope")),
                    type = VendorKeyType.fromWire(o.optString("type")) ?: VendorKeyType.Int32,
                    values = if (values == null) emptyList() else (0 until values.length()).map { values.getDouble(it) },
                    enabled = o.optBoolean("enabled", true)
                )
            }.getOrNull()
        }
    }
}
