package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.validateVendorKey
import com.rawr.camera.settings.model.CFA_BLOCKS
import com.rawr.camera.settings.model.CFA_LIVE_QUALITIES
import com.rawr.camera.settings.model.CFA_ORDERS
import com.rawr.camera.settings.model.CFA_PHOTO_QUALITIES
import com.rawr.camera.settings.model.BACKEND_CHOICES
import com.rawr.camera.settings.model.CAMERA_QUIRKS
import com.rawr.camera.settings.model.COMPANION_FALLBACKS
import com.rawr.camera.settings.model.isReaderUsage
import com.rawr.camera.settings.model.GATE_HOLDS
import com.rawr.camera.settings.model.REQUEST_DEFAULTS
import com.rawr.camera.settings.model.SESSION_PARAMS_POLICIES
import com.rawr.camera.settings.model.EXTENDED_ISO_MAX
import com.rawr.camera.settings.model.EXTENDED_SHUTTER_MAX_NS
import com.rawr.camera.settings.model.AuxJpegSettings
import com.rawr.camera.settings.model.CaptureStreamSettings
import com.rawr.camera.settings.model.CompanionMode
import com.rawr.camera.settings.model.CompanionSettings
import com.rawr.camera.settings.model.LensCamera
import com.rawr.camera.settings.model.LensCfa
import com.rawr.camera.settings.model.LensDcp
import com.rawr.camera.settings.model.LensImage
import com.rawr.camera.settings.model.LensRequest
import com.rawr.camera.settings.model.LensSession
import com.rawr.camera.settings.model.LensStreams
import com.rawr.camera.settings.model.RawStreamSettings
import com.rawr.camera.settings.model.ReaderSettings
import com.rawr.camera.settings.model.LensExposure
import com.rawr.camera.settings.model.LensLevels
import com.rawr.camera.settings.model.LensMirror
import com.rawr.camera.settings.model.LensOutputSize
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.RawStreamChoice
import com.rawr.camera.settings.model.RawStreamFormat
import com.rawr.camera.settings.model.VendorKey
import com.rawr.camera.settings.model.VendorKeyScope
import com.rawr.camera.settings.model.VendorKeyType
import org.json.JSONArray
import org.json.JSONObject

/**
 * JSON form of the lens profiles, shared with the native camera profile (camera/CameraProfileJson.h): schema 2,
 * each lens grouped into camera, streams, session, request, keys and image. Stored as-is in settings, with each
 * lens's `enabled` flag; the camera receives only the enabled lenses. Schema-1 documents (older settings and
 * backups) are upgraded into schema 2 before they are read, the same way the native parser does it.
 */
internal object LensProfileCodec {
    const val PROFILE_ID = "user"
    const val SCHEMA = 2

    /** [dcpDir]: the camera profile directory, for the native form (each "dcp" gets its absolute "path"). */
    fun encode(lenses: List<LensProfile>, enabledOnly: Boolean = false, dcpDir: java.io.File? = null): String {
        val array = JSONArray()
        lenses.filter { !enabledOnly || it.enabled }.forEach { array.put(encodeLens(it, dcpDir)) }
        return JSONObject().put("id", PROFILE_ID).put("schema", SCHEMA).put("lenses", array).toString()
    }

    /** Null for a missing or unreadable document; unreadable lenses/keys are dropped. */
    fun decode(raw: String?): List<LensProfile>? {
        if (raw.isNullOrBlank()) return null
        val root = runCatching { JSONObject(raw) }.getOrNull() ?: return null
        val lenses = root.optJSONArray("lenses") ?: return null
        val schema = root.optInt("schema", 1)
        if (schema != 1 && schema != SCHEMA) return null
        return (0 until lenses.length()).mapNotNull { i ->
            runCatching { decodeLens(lensV2(lenses.getJSONObject(i), schema)) }.getOrNull()
        }
    }

    /** Backup import must reject damaged nested records rather than dropping them. */
    fun decodeBackup(raw: String): List<LensProfile> = try {
        val root = JSONObject(raw)
        val schema = root.optInt("schema", 1)
        require(schema == 1 || schema == SCHEMA) { "Unsupported lens profile schema $schema" }
        val lenses = root.getJSONArray("lenses")
        (0 until lenses.length()).map { index ->
            val lens = lensV2(lenses.getJSONObject(index), schema)
            validateBackupLens(lens)
            decodeLens(lens)
        }
    } catch (failure: Exception) {
        throw IllegalArgumentException("Backup contains malformed lens profiles", failure)
    }

    private fun lensV2(o: JSONObject, schema: Int): JSONObject = if (schema == SCHEMA) o else upgradeLensV1(o)

    /**
     * A schema-1 lens (flat fields beside the newer blocks) in the schema-2 layout; mirrors the native
     * upgradeLensV1. Fields kept for a capture stream the lens doesn't have are dropped: they had no effect.
     */
    internal fun upgradeLensV1(source: JSONObject): JSONObject {
        val l = JSONObject(source.toString())
        // Early builds kept DCG keys/levels in a separate "dcg" block: its keys join the lens's, and its levels
        // win when the lens's are dynamic.
        l.optJSONObject("dcg")?.let { dcg ->
            val keys = l.optJSONArray("keys") ?: JSONArray()
            dcg.optJSONArray("keys")?.let { extra -> for (i in 0 until extra.length()) keys.put(extra.get(i)) }
            l.put("keys", keys)
            val dcgLevels = dcg.optJSONObject("levels")
            if (dcgLevels?.optBoolean("static") == true && l.optJSONObject("levels")?.optBoolean("static") != true)
                l.put("levels", dcgLevels)
        }
        if (l.has("disableCompanion")) require(l.get("disableCompanion") is Boolean) { "Invalid disableCompanion" }
        require(!l.optBoolean("disableCompanion", false) || !l.has("companion")) { "Cannot disable a required companion" }
        fun JSONObject.copyFrom(from: JSONObject, key: String, to: String = key) = apply { if (from.has(key)) put(to, from.get(key)) }
        fun JSONObject.putIfAny(key: String, value: JSONObject) = apply { if (value.length() > 0) put(key, value) }
        val out = JSONObject().copyFrom(l, "id").copyFrom(l, "enabled")
        out.put("camera", JSONObject().copyFrom(l, "cameraId", "id").copyFrom(l, "physicalCameraId", "physical"))
        val raw = l.optJSONObject("stream")?.let { JSONObject(it.toString()) } ?: JSONObject()
        val reader = JSONObject().copyFrom(l, "readerUsage", "usage")
        l.optJSONObject("reader")?.let { rd -> rd.keys().forEach { reader.put(it, rd.get(it)) } }
        raw.putIfAny("reader", reader).copyFrom(l, "viewfinderLevels", "levels")
        val streams = JSONObject().put("raw", raw)
        l.optJSONObject("captureStream")?.let { c ->
            val capture = JSONObject(c.toString())
                .putIfAny("reader", JSONObject().copyFrom(l, "captureReaderUsage", "usage"))
                .putIfAny("burst", JSONObject().copyFrom(l, "captureBurst", "max").copyFrom(l, "captureBurstSequential", "sequential"))
                .copyFrom(l, "captureLevels", "levels")
            streams.put("capture", capture)
        }
        when {
            l.optBoolean("disableCompanion", false) -> streams.put("companion", JSONObject().put("mode", "off"))
            l.has("companion") -> {
                val c = JSONObject(l.getJSONObject("companion").toString())
                streams.put("companion", c.put("mode", if (c.has("width") || c.has("height")) "fixed" else "auto"))
            }
        }
        streams.copyFrom(l, "auxJpeg")
        out.put("streams", streams)
        val session = l.optJSONObject("session")?.let { JSONObject(it.toString()) } ?: JSONObject()
        for (key in listOf("backend", "template", "fps", "startupDelayMs", "stopRepeatingForStill", "quirks"))
            session.copyFrom(l, key)
        session.putIfAny("gate", JSONObject().apply {
            l.optString("vendorGate").takeIf { it.isNotEmpty() }?.let { put("name", it) }
            copyFrom(l, "gateHold", "hold")
        })
        out.put("session", session).copyFrom(l, "request").copyFrom(l, "keys").copyFrom(l, "streamUseCase")
        val image = JSONObject()
        for (key in listOf("levels", "cfa", "distortion", "mirror", "dcp", "exposure")) image.copyFrom(l, key)
        return out.put("image", image)
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

    private fun flags(o: JSONObject, vararg names: String) {
        for (name in names) if (o.has(name)) require(o.get(name) is Boolean) { "Invalid $name" }
    }

    private fun usage(o: JSONObject) {
        if (o.has("usage")) require(o.get("usage") is String && isReaderUsage(o.getString("usage"))) { "Invalid usage" }
    }

    private fun levels(parent: JSONObject, name: String = "levels") {
        objectField(parent, name)?.let { levels ->
            flags(levels, "static")
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

    private fun streamSize(stream: JSONObject, required: Boolean) {
        choice(stream, "format", RawStreamFormat.entries.map { it.wireName })
        val min = if (required) 1.0 else 0.0
        number(stream, "width", min..Int.MAX_VALUE.toDouble(), true)
        number(stream, "height", min..Int.MAX_VALUE.toDouble(), true)
        if (required) require(stream.has("width") && stream.has("height"))
        number(stream, "useCase", 0.0..Int.MAX_VALUE.toDouble(), true)
    }

    private fun outputSize(o: JSONObject) {
        require(o.has("width") && o.has("height"))
        number(o, "width", 1.0..65536.0, true)
        number(o, "height", 1.0..65536.0, true)
    }

    /** A schema-2 lens from a backup: every field present must have the right type and range. */
    private fun validateBackupLens(o: JSONObject) {
        require(o.get("id") is String && o.getString("id").isNotBlank())
        flags(o, "enabled")
        val camera = o.getJSONObject("camera")
        require(camera.get("id") is String && camera.getString("id").isNotBlank())
        if (camera.has("physical")) require(camera.get("physical") is String)
        require(!o.has("streamUseCase")) { "streamUseCase is not a profile field" }

        objectField(o, "streams")?.let { streams ->
            objectField(streams, "raw")?.let { raw ->
                streamSize(raw, required = false)
                flags(raw, "unlisted")
                objectField(raw, "reader")?.let { reader ->
                    usage(reader)
                    if (reader.has("maxImages")) number(reader, "maxImages", 6.0..16.0, true)
                    flags(reader, "probe", "demote")
                }
                levels(raw)
            }
            objectField(streams, "capture")?.let { capture ->
                streamSize(capture, required = true)
                objectField(capture, "reader")?.let { usage(it) }
                objectField(capture, "burst")?.let { burst ->
                    number(burst, "max", 0.0..30.0, true)
                    flags(burst, "sequential")
                }
                levels(capture)
            }
            objectField(streams, "companion")?.let { c ->
                choice(c, "mode", CompanionMode.entries.map { it.wireName })
                if (c.optString("mode") == "fixed") outputSize(c)
                else require(!c.has("width") && !c.has("height")) { "Companion size needs mode fixed" }
                choice(c, "format", listOf("yuv", "private"))
                choice(c, "fallback", COMPANION_FALLBACKS)
                flags(c, "targeted", "physical", "detach")
                if (c.has("maxImages")) number(c, "maxImages", 2.0..16.0, true)
                number(c, "useCase", 0.0..Int.MAX_VALUE.toDouble(), true)
                usage(c)
            }
            objectField(streams, "auxJpeg")?.let { aux ->
                outputSize(aux)
                number(aux, "useCase", 0.0..Int.MAX_VALUE.toDouble(), true)
                usage(aux)
            }
        }
        objectField(o, "session")?.let { session ->
            choice(session, "backend", BACKEND_CHOICES)
            choice(session, "params", SESSION_PARAMS_POLICIES)
            choice(session, "template", listOf("preview", "still"))
            if (session.has("opMode")) {
                number(session, "opMode", 0.0..Int.MAX_VALUE.toDouble(), true)
                require(session.getInt("opMode") != 1 && session.getInt("opMode") != 2) { "Invalid opMode" }
            }
            number(session, "fps", 0.0..240.0, true)
            number(session, "startupDelayMs", 0.0..5000.0, true)
            flags(session, "stopRepeatingForStill")
            objectField(session, "gate")?.let { gate ->
                if (gate.has("name")) require(gate.get("name") is String)
                choice(gate, "hold", GATE_HOLDS)
            }
            if (session.has("quirks")) {
                val quirks = session.getJSONArray("quirks")
                for (i in 0 until quirks.length())
                    require(quirks.get(i) is String && quirks.getString(i) in CAMERA_QUIRKS) { "Invalid quirks" }
            }
        }
        objectField(o, "request")?.let { choice(it, "defaults", REQUEST_DEFAULTS) }
        if (o.has("keys")) {
            val array = o.getJSONArray("keys")
            for (i in 0 until array.length()) {
                val key = array.getJSONObject(i)
                require(key.get("tag") is String && key.getString("tag").isNotBlank())
                choice(key, "type", VendorKeyType.entries.map { it.wireName })
                choice(key, "scope", VendorKeyScope.entries.map { it.wireName })
                flags(key, "enabled")
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
        objectField(o, "image")?.let { image ->
            levels(image)
            objectField(image, "dcp")?.let { dcp ->
                require(dcp.get("file") is String && DcpFileStore.isFileName(dcp.getString("file")))
                if (dcp.has("name")) require(dcp.get("name") is String)
            }
            objectField(image, "exposure")?.let { exposure ->
                number(exposure, "isoMax", 0.0..EXTENDED_ISO_MAX.toDouble(), true)
                number(exposure, "shutterMaxNs", 0.0..EXTENDED_SHUTTER_MAX_NS.toDouble(), true)
                flags(exposure, "extended")
            }
            objectField(image, "cfa")?.let { cfa ->
                if (cfa.has("block")) {
                    number(cfa, "block", 1.0..Int.MAX_VALUE.toDouble(), true)
                    require(cfa.getInt("block") in CFA_BLOCKS)
                }
                choice(cfa, "order", CFA_ORDERS.toList())
                choice(cfa, "live", CFA_LIVE_QUALITIES.toList())
                choice(cfa, "photo", CFA_PHOTO_QUALITIES.toList())
            }
            objectField(image, "distortion")
            choice(image, "mirror", LensMirror.entries.map { it.wireName })
        }
    }

    private fun encodeStream(choice: RawStreamChoice) = JSONObject()
        .put("format", choice.format.wireName)
        .put("width", choice.width.coerceAtLeast(0))
        .put("height", choice.height.coerceAtLeast(0))

    private fun encodeLens(lens: LensProfile, dcpDir: java.io.File?): JSONObject {
        val streams = lens.streams
        val raw = encodeStream(streams.raw.choice).apply {
            if (streams.raw.choice.unlisted) put("unlisted", true)
            if (streams.raw.useCase != 0) put("useCase", streams.raw.useCase)
            val reader = streams.raw.reader
            if (reader != ReaderSettings()) put("reader", JSONObject().apply {
                reader.usage?.let { put("usage", it) }
                if (reader.maxImages != 0) put("maxImages", reader.maxImages)
                if (reader.probe) put("probe", true)
                if (reader.demote) put("demote", true)
            })
            streams.raw.levels?.let { put("levels", encodeLevels(it)) }
        }
        val streamsJson = JSONObject().put("raw", raw)
        streams.capture?.let { capture ->
            streamsJson.put("capture", encodeStream(capture.choice).apply {
                if (capture.useCase != 0) put("useCase", capture.useCase)
                capture.readerUsage?.let { put("reader", JSONObject().put("usage", it)) }
                if (capture.burstMax > 0 || capture.burstSequential) put("burst", JSONObject().apply {
                    if (capture.burstMax > 0) put("max", capture.burstMax)
                    if (capture.burstSequential) put("sequential", true)
                })
                capture.levels?.let { put("levels", encodeLevels(it)) }
            })
        }
        val companion = streams.companion
        if (companion != CompanionSettings()) {
            streamsJson.put("companion", JSONObject().apply {
                put("mode", companion.mode.wireName)
                if (companion.mode == CompanionMode.Fixed) put("width", companion.size.width).put("height", companion.size.height)
                if (companion.mode != CompanionMode.Off) {
                    put("format", companion.format).put("targeted", companion.targeted)
                    companion.usage?.let { put("usage", it) }
                    if (companion.maxImages != 0) put("maxImages", companion.maxImages)
                    companion.physical?.let { put("physical", it) }
                    companion.detach?.let { put("detach", it) }
                    if (companion.fallback != "base") put("fallback", companion.fallback)
                    if (companion.useCase != 0) put("useCase", companion.useCase)
                }
            })
        }
        streams.auxJpeg?.let { aux ->
            streamsJson.put("auxJpeg", JSONObject().put("width", aux.size.width).put("height", aux.size.height).apply {
                aux.usage?.let { put("usage", it) }
                if (aux.useCase != 0) put("useCase", aux.useCase)
            })
        }
        val s = lens.session
        val session = JSONObject().apply {
            if (s.backend != "auto") put("backend", s.backend)
            if (s.opMode != 0) put("opMode", s.opMode)
            if (s.params != "cadence") put("params", s.params)
            s.stillTemplate?.let { put("template", if (it) "still" else "preview") }
            if (s.fixedFps > 0) put("fps", s.fixedFps)
            if (s.startupDelayMs > 0) put("startupDelayMs", s.startupDelayMs)
            if (s.gate.isNotEmpty() || s.gateHold != null) put("gate", JSONObject().apply {
                if (s.gate.isNotEmpty()) put("name", s.gate)
                s.gateHold?.let { put("hold", it) }
            })
            if (s.stopRepeatingForStill) put("stopRepeatingForStill", true)
            if (s.quirks.isNotEmpty()) put("quirks", JSONArray(s.quirks))
        }
        val i = lens.image
        val image = JSONObject()
            .put("levels", encodeLevels(i.levels))
            .put("exposure", JSONObject().put("extended", i.exposure.extended).put("isoMax", i.exposure.isoMax)
                .put("shutterMaxNs", i.exposure.shutterMaxNs))
            .apply {
                if (i.cfa.block > 1 || i.cfa.order != null) {
                    put("cfa", JSONObject().put("block", i.cfa.block).put("live", i.cfa.live).put("photo", i.cfa.photo)
                        .apply { i.cfa.order?.let { put("order", it) } })
                }
                i.distortion?.let { put("distortion", JSONObject(it)) }
                if (i.mirror != LensMirror.None) put("mirror", i.mirror.wireName)
                i.dcp?.let { dcp ->
                    put("dcp", JSONObject().put("file", dcp.file).put("name", dcp.name)
                        .apply { dcpDir?.let { put("path", java.io.File(it, dcp.file).absolutePath) } })
                }
            }
        return JSONObject()
            .put("id", lens.name.trim())
            .put("enabled", lens.enabled)
            .put("camera", JSONObject().put("id", lens.camera.id).put("physical", lens.camera.physical))
            .put("streams", streamsJson)
            .apply {
                if (session.length() > 0) put("session", session)
                if (lens.request.defaults != "rawr") put("request", JSONObject().put("defaults", lens.request.defaults))
            }
            .put("keys", encodeKeys(lens.vendorKeys))
            .put("image", image)
    }

    private fun decodeStream(o: JSONObject?) = RawStreamChoice(
        format = RawStreamFormat.fromWire(o?.optString("format")),
        width = o?.optInt("width") ?: 0,
        height = o?.optInt("height") ?: 0,
        unlisted = o?.optBoolean("unlisted") ?: false
    )

    private fun useCase(o: JSONObject?) = o?.optInt("useCase")?.takeIf { it >= 0 } ?: 0

    /** A schema-2 lens. */
    private fun decodeLens(o: JSONObject): LensProfile {
        require(!o.has("streamUseCase")) { "streamUseCase is not a profile field; set useCase on a stream" }
        val camera = o.getJSONObject("camera")
        val streams = o.optJSONObject("streams")
        val raw = streams?.optJSONObject("raw")
        val reader = raw?.optJSONObject("reader")
        val c = streams?.optJSONObject("companion")
        val companionMode = c?.let {
            CompanionMode.fromWire(it.optString("mode").ifEmpty { if (it.has("width") || it.has("height")) "fixed" else "auto" })
                ?: throw IllegalArgumentException("Invalid companion mode")
        } ?: CompanionMode.Auto
        val companionFormat = c?.optString("format", "yuv") ?: "yuv"
        require(companionFormat in setOf("yuv", "private")) { "Invalid companion format" }
        val companionSize = decodeSize(c)
        require(companionMode != CompanionMode.Fixed || companionSize != null) { "Companion mode fixed needs a size" }
        val session = o.optJSONObject("session")
        val gate = session?.optJSONObject("gate")
        val image = o.optJSONObject("image")
        val cfa = image?.optJSONObject("cfa")
        return LensProfile(
            name = o.getString("id"),
            camera = LensCamera(camera.getString("id"), camera.optString("physical")),
            enabled = o.optBoolean("enabled", true),
            streams = LensStreams(
                raw = RawStreamSettings(
                    choice = decodeStream(raw),
                    useCase = useCase(raw),
                    reader = ReaderSettings(
                        usage = reader?.optString("usage")?.takeIf { it.isNotEmpty() },
                        maxImages = reader?.optInt("maxImages")?.coerceIn(0, 16) ?: 0,
                        probe = reader?.optBoolean("probe") ?: false,
                        demote = reader?.optBoolean("demote") ?: false
                    ),
                    levels = raw?.optJSONObject("levels")?.let { decodeLevels(it) }
                ),
                capture = streams?.optJSONObject("capture")?.let { cap ->
                    val choice = decodeStream(cap).copy(unlisted = false)
                    if (choice.width <= 0 || choice.height <= 0) return@let null
                    val burst = cap.optJSONObject("burst")
                    CaptureStreamSettings(
                        choice = choice,
                        useCase = useCase(cap),
                        readerUsage = cap.optJSONObject("reader")?.optString("usage")?.takeIf { it.isNotEmpty() },
                        burstMax = burst?.optInt("max")?.coerceIn(0, 30) ?: 0,
                        burstSequential = burst?.optBoolean("sequential") ?: false,
                        levels = cap.optJSONObject("levels")?.let { decodeLevels(it) }
                    )
                },
                companion = CompanionSettings(
                    mode = companionMode,
                    size = companionSize ?: LensOutputSize(320, 240),
                    format = companionFormat,
                    targeted = c?.optBoolean("targeted", true) ?: true,
                    detach = c?.takeIf { it.has("detach") }?.optBoolean("detach"),
                    physical = c?.takeIf { it.has("physical") }?.optBoolean("physical"),
                    fallback = c?.optString("fallback")?.takeIf { it in COMPANION_FALLBACKS } ?: "base",
                    usage = c?.optString("usage")?.takeIf { it.isNotEmpty() },
                    maxImages = c?.optInt("maxImages")?.coerceIn(0, 16) ?: 0,
                    useCase = useCase(c)
                ),
                auxJpeg = streams?.optJSONObject("auxJpeg")?.let { aux ->
                    decodeSize(aux)?.let { AuxJpegSettings(it, aux.optString("usage").ifEmpty { null }, useCase(aux)) }
                }
            ),
            session = LensSession(
                backend = session?.optString("backend")?.takeIf { it in BACKEND_CHOICES } ?: "auto",
                opMode = session?.optInt("opMode")?.takeIf { it >= 0 } ?: 0,
                params = session?.optString("params")?.takeIf { it in SESSION_PARAMS_POLICIES } ?: "cadence",
                stillTemplate = when (session?.optString("template")) { "still" -> true; "preview" -> false; else -> null },
                fixedFps = session?.optInt("fps")?.coerceIn(0, 240) ?: 0,
                startupDelayMs = session?.optInt("startupDelayMs")?.coerceIn(0, 5000) ?: 0,
                gate = gate?.optString("name").orEmpty(),
                gateHold = gate?.optString("hold")?.takeIf { it in GATE_HOLDS },
                stopRepeatingForStill = session?.optBoolean("stopRepeatingForStill") ?: false,
                quirks = session?.optJSONArray("quirks")
                    ?.let { a -> (0 until a.length()).map { a.getString(it) }.filter { it in CAMERA_QUIRKS } } ?: emptyList()
            ),
            request = LensRequest(
                defaults = o.optJSONObject("request")?.optString("defaults")?.takeIf { it in REQUEST_DEFAULTS } ?: "rawr"
            ),
            vendorKeys = decodeKeys(o.optJSONArray("keys")),
            image = LensImage(
                levels = decodeLevels(image?.optJSONObject("levels")),
                cfa = LensCfa(
                    block = cfa?.optInt("block", 1)?.takeIf { it in CFA_BLOCKS } ?: 1,
                    order = cfa?.optString("order")?.takeIf { it in CFA_ORDERS },
                    live = cfa?.optString("live")?.takeIf { it in CFA_LIVE_QUALITIES } ?: "standard",
                    photo = cfa?.optString("photo")?.takeIf { it in CFA_PHOTO_QUALITIES } ?: "high"
                ),
                distortion = image?.optJSONObject("distortion")?.toString(),
                mirror = LensMirror.fromWire(image?.optString("mirror")),
                dcp = image?.optJSONObject("dcp")?.let { d ->
                    d.optString("file").takeIf { DcpFileStore.isFileName(it) }?.let { LensDcp(it, d.optString("name")) }
                },
                exposure = image?.optJSONObject("exposure").let { e ->
                    LensExposure(
                        extended = e?.optBoolean("extended") ?: false,
                        isoMax = (e?.optInt("isoMax") ?: 0).coerceIn(0, EXTENDED_ISO_MAX),
                        shutterMaxNs = (e?.optLong("shutterMaxNs") ?: 0L).coerceIn(0L, EXTENDED_SHUTTER_MAX_NS)
                    )
                }
            )
        )
    }

    private fun decodeSize(o: JSONObject?): LensOutputSize? =
        o?.takeIf { it.has("width") && it.has("height") }
            ?.let { LensOutputSize(it.optInt("width"), it.optInt("height")) }
            ?.takeIf { it.width > 0 && it.height > 0 }

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
