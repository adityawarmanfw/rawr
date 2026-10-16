package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.AgxCatalog
import com.rawr.camera.settings.model.AgxLook
import com.rawr.camera.settings.model.AgxPreset
import com.rawr.camera.settings.model.AgxValues
import com.rawr.camera.settings.model.FilmPreset
import com.rawr.camera.settings.model.FilmSimLook
import org.json.JSONArray
import org.json.JSONObject

/** A look read back from an exported file. */
internal sealed interface ImportedLook {
    val name: String
    data class Film(override val name: String, val look: FilmSimLook) : ImportedLook
    data class Agx(override val name: String, val base: AgxPreset, val values: AgxValues) : ImportedLook
}

/**
 * Saved AgX looks (DataStore) and the shareable look file. Film looks reuse
 * [FilmSimCodec]'s versioned string so older exports keep decoding; AgX
 * controls are keyed by their recipe JSON key so the order can change.
 */
internal object LookFileCodec {
    private const val FORMAT = "rawr-look"
    private const val VERSION = 1
    const val MIME = "application/json"

    fun encodeAgxLooks(looks: List<AgxLook>): String = JSONArray().apply {
        looks.forEach { put(agxJson(it.name, it.base, it.values).put("id", it.id)) }
    }.toString()

    fun decodeAgxLooks(raw: String?): List<AgxLook> {
        if (raw.isNullOrBlank()) return emptyList()
        val array = runCatching { JSONArray(raw) }.getOrNull() ?: return emptyList()
        return (0 until array.length()).mapNotNull { i ->
            val j = array.optJSONObject(i) ?: return@mapNotNull null
            val id = j.optString("id").takeIf { it.isNotBlank() } ?: return@mapNotNull null
            val look = decodeAgx(j) ?: return@mapNotNull null
            AgxLook(id, look.name, look.base, look.values)
        }.distinctBy { it.id }
    }

    fun exportFilm(preset: FilmPreset): String = JSONObject()
        .put("format", FORMAT).put("version", VERSION).put("kind", "film")
        .put("name", preset.name).put("look", FilmSimCodec.encodeLook(preset.look))
        .toString(2)

    fun exportAgx(look: AgxLook): String = agxJson(look.name, look.base, look.values)
        .put("format", FORMAT).put("version", VERSION).put("kind", "agx")
        .toString(2)

    fun import(text: String): ImportedLook {
        val j = runCatching { JSONObject(text) }.getOrNull()
        require(j != null && j.optString("format") == FORMAT) { "Not a rawr look file" }
        require(j.optInt("version") in 1..VERSION) { "Look file is from a newer version" }
        val name = j.optString("name").trim().ifEmpty { "Imported look" }
        return when (j.optString("kind")) {
            "film" -> ImportedLook.Film(name, requireNotNull(FilmSimCodec.decodeLook(j.optString("look"))) {
                "Film look is damaged"
            })
            "agx" -> requireNotNull(decodeAgx(j)) { "AgX look is damaged" }
            else -> throw IllegalArgumentException("Unknown look kind")
        }
    }

    fun fileName(name: String): String =
        name.replace(Regex("[^A-Za-z0-9._ -]"), "_").trim().ifEmpty { "look" } + ".json"

    private fun agxJson(name: String, base: AgxPreset, values: AgxValues): JSONObject = JSONObject()
        .put("name", name).put("base", base.name)
        .put("values", JSONObject().apply { AgxCatalog.all.forEach { put(it.jsonKey, values.valueOf(it.parameter).toDouble()) } })

    /** Missing controls fall back to the base look's defaults. */
    private fun decodeAgx(j: JSONObject): ImportedLook.Agx? {
        val base = AgxPreset.entries.firstOrNull { it.name == j.optString("base") } ?: return null
        val stored = j.optJSONObject("values") ?: return null
        val values = AgxCatalog.all.fold(base.defaults) { acc, spec ->
            val value = stored.optDouble(spec.jsonKey, Double.NaN)
            if (value.isNaN()) acc else acc.with(spec.parameter, value.toFloat())
        }
        val name = j.optString("name").trim().ifEmpty { "Imported look" }
        return ImportedLook.Agx(name, base, AgxCatalog.sanitized(values, base.defaults))
    }
}
