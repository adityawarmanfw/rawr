package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.EXTENDED_ISO_MAX
import com.rawr.camera.settings.model.EXTENDED_SHUTTER_MAX_NS
import com.rawr.camera.settings.model.LensExposure
import com.rawr.camera.settings.model.LensLevels
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

    fun encode(lenses: List<LensProfile>, enabledOnly: Boolean = false): String {
        val array = JSONArray()
        lenses.filter { !enabledOnly || it.enabled }.forEach { array.put(encodeLens(it)) }
        return JSONObject().put("id", PROFILE_ID).put("lenses", array).toString()
    }

    /** Null for a missing or unreadable document; unreadable lenses/keys are dropped. */
    fun decode(raw: String?): List<LensProfile>? {
        if (raw.isNullOrBlank()) return null
        val root = runCatching { JSONObject(raw) }.getOrNull() ?: return null
        val lenses = root.optJSONArray("lenses") ?: return null
        return (0 until lenses.length()).mapNotNull { i -> runCatching { decodeLens(lenses.getJSONObject(i)) }.getOrNull() }
    }

    private fun encodeLens(lens: LensProfile): JSONObject = JSONObject()
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
                    height = stream?.optInt("height") ?: 0
                ),
            exposure = o.optJSONObject("exposure").let { e ->
                LensExposure(
                    extended = e?.optBoolean("extended") ?: false,
                    isoMax = (e?.optInt("isoMax") ?: 0).coerceIn(0, EXTENDED_ISO_MAX),
                    shutterMaxNs = (e?.optLong("shutterMaxNs") ?: 0L).coerceIn(0L, EXTENDED_SHUTTER_MAX_NS)
                )
            },
            levels = if (!levels.isStatic && legacyLevels?.isStatic == true) legacyLevels else levels,
            vendorKeys = decodeKeys(o.optJSONArray("keys")) + decodeKeys(legacyDcg?.optJSONArray("keys"))
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
