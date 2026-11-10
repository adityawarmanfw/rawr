package com.rawr.camera.settings.model

/**
 * User lens configuration (Settings → Capture → Lens). The list order is the
 * capture-screen order; disabled lenses are kept but not sent to the camera.
 * The name doubles as the app's lens id and the capture-screen label.
 */
const val LENS_NAME_MAX_LENGTH = 4

enum class VendorKeyScope(val wireName: String, val label: String) {
    /** Session parameter, set when each capture session (photo or video) is created. */
    Session("session", "Session"),

    /** Set on every capture request (preview, stills, multiframe, video). */
    Request("request", "Request"),

    /** Only on full-size photo requests of a high-res photo lens (vivo 50/200 MP). */
    Still("still", "Still"),

    /**
     * Only on a one-frame viewfinder request (lowest ISO, companion YUV only) sent before the
     * full-size photos of a high-res photo lens: the vivo 200 MP HAL re-decides full size vs
     * binned only on such a frame.
     */
    Decision("decision", "Decision"),

    /** The same value in the session parameters and on every request (HALs that read it at configure and per frame). */
    SessionAndRequest("sessionAndRequest", "Session + request");

    companion object {
        fun fromWire(value: String?): VendorKeyScope = entries.firstOrNull { it.wireName == value } ?: Session
    }
}

enum class VendorKeyType(val wireName: String, val label: String, val integral: Boolean) {
    Byte("byte", "Byte", true),
    Int32("int32", "Int32", true),
    Int64("int64", "Int64", true),
    Float("float", "Float", false),
    Double("double", "Double", false);

    companion object {
        fun fromWire(value: String?): VendorKeyType? = entries.firstOrNull { it.wireName == value }
    }
}

enum class RawStreamFormat(val wireName: String, val label: String) {
    Raw16("RAW16", "RAW_SENSOR"),
    Raw10("RAW10", "RAW10");

    companion object {
        fun fromWire(value: String?): RawStreamFormat = entries.firstOrNull { it.wireName == value } ?: Raw16
    }
}

data class VendorKey(
    /** Metadata name ("vivo.control.forceSensorMode") or numeric tag ("0x80020000"). */
    val tag: String,
    val scope: VendorKeyScope = VendorKeyScope.Session,
    val type: VendorKeyType = VendorKeyType.Int32,
    val values: List<Double> = emptyList(),
    val enabled: Boolean = true
)

data class LensLevels(
    /** False: the camera's own (dynamic) black/white levels. */
    val isStatic: Boolean = false,
    val blackRggb: List<Float> = listOf(0f, 0f, 0f, 0f),
    val white: Float = 0f
)

/**
 * Opt-in requests past the camera's advertised ISO / shutter ranges (some
 * HALs accept them). Zero keeps the advertised bound. Shutters past the 1/5 s
 * viewfinder run only when the shutter is pressed.
 */
data class LensExposure(
    val extended: Boolean = false,
    val isoMax: Int = 0,
    val shutterMaxNs: Long = 0L
)

const val EXTENDED_SHUTTER_MAX_NS = 25_000_000_000L
const val EXTENDED_ISO_MAX = 409_600

/**
 * Preferred RAW stream; a zero size means the largest size of the format. [highResLive]: a vendor full-size mode
 * the camera doesn't advertise, configured at exactly this size.
 */
data class RawStreamChoice(
    val format: RawStreamFormat = RawStreamFormat.Raw16,
    val width: Int = 0,
    val height: Int = 0,
    val highResLive: Boolean = false
) {
    val isLargest: Boolean get() = width <= 0 || height <= 0
    val label: String get() = if (isLargest) "${format.label} (largest)" else "${format.label} ${width}×$height"
}

/**
 * A DNG camera profile (.dcp) for the lens, replacing the camera's colour calibration in the DNG and
 * the app's render. [file] names the copy in app storage (content-addressed); [name] is its ProfileName.
 */
data class LensDcp(val file: String, val name: String)

/** What the session parameters carry: the fps range and session keys, nothing at all, or the whole repeating request. */
val SESSION_PARAMS_POLICIES = listOf("cadence", "none", "full")

/** Request defaults the app writes: its own, or a minimal set that leaves processing overrides to the template. */
val REQUEST_DEFAULTS = listOf("rawr", "minimal")

/** How long a vendor gate is held: until the session is configured, or until the device closes. */
val GATE_HOLDS = listOf("configured", "closed")

/** Highest fixed frame rate a profile can ask for (native clamps to the same). */
const val MAX_FRAME_RATE = 240

/** Which camera API runs a lens: "auto" picks the Java one when an operating mode or stream use case is set. */
val CAMERA_API_CHOICES = listOf("auto", "native", "java")

/** Operating mode and stream use case exist only on the Java camera API. */
fun LensProfile.needsJavaCameraApi(): Boolean =
    session.opMode != 0 || streams.raw.useCase != 0 || (streams.photo?.useCase ?: 0) != 0 ||
        streams.companion.useCase != 0 || (streams.auxJpeg?.useCase ?: 0) != 0

/** The camera API this lens runs on, after the derived rule ("auto" picks Java for Java-only options). */
fun LensProfile.resolvedCameraApi(): String =
    if (session.cameraApi != "auto") session.cameraApi else if (needsJavaCameraApi()) "java" else "native"

/** How a lens takes a full-size photo; derived, not set (native stillStrategyFor). */
enum class StillSequence { Direct, DecisionFrame, PauseViewfinder }

fun LensProfile.stillSequence(): StillSequence {
    val decisionKey = vendorKeys.any { it.enabled && it.scope == VendorKeyScope.Decision }
    val companionOn = streams.companion.mode != CompanionMode.Off && streams.companion.targeted
    return when {
        streams.photo != null && companionOn && decisionKey -> StillSequence.DecisionFrame
        session.stopViewfinderForPhotos -> StillSequence.PauseViewfinder
        else -> StillSequence.Direct
    }
}

/** What an automatic companion size does when the camera has no output with the RAW's shape. */
val COMPANION_FALLBACKS = listOf("base", "nearest", "none")

/** Buffer usage as the profile writes it: a name ("gpuDataBuffer", "none") or hex ("0x100003"). */
fun isBufferUsage(text: String): Boolean =
    text == "gpuDataBuffer" || text == "none" ||
        (text.length > 2 && (text.startsWith("0x") || text.startsWith("0X")) && text.drop(2).toULongOrNull(16) != null)

/** Provider behaviours a lens can say it avoids (see the native CameraWorkarounds). */
val CAMERA_WORKAROUNDS = listOf("holdTapFocus", "appPriorityAe", "noTapFocus", "alwaysSendFrameRate", "noAutoCompanion", "keepTemplateRegions")

/**
 * One lens, grouped the way profile schema 2 groups it: which camera, its streams, how the session runs, what
 * requests carry, and how its image is read. [name] is the app's lens id and capture-screen label.
 */
data class LensProfile(
    val name: String,
    val camera: LensCamera,
    val enabled: Boolean = true,
    val streams: LensStreams = LensStreams(),
    val session: LensSession = LensSession(),
    val request: LensRequest = LensRequest(),
    val vendorKeys: List<VendorKey> = emptyList(),
    val image: LensImage = LensImage()
) {
    /** A lens on [cameraId] (and [physicalCameraId] inside it) with every other setting at its default. */
    constructor(name: String, cameraId: String, physicalCameraId: String = "") :
        this(name, LensCamera(cameraId, physicalCameraId))

    fun streams(change: LensStreams.() -> LensStreams) = copy(streams = streams.change())
    fun session(change: LensSession.() -> LensSession) = copy(session = session.change())
    fun image(change: LensImage.() -> LensImage) = copy(image = image.change())
    fun raw(change: RawStreamSettings.() -> RawStreamSettings) = streams { copy(raw = raw.change()) }
    fun companion(change: CompanionSettings.() -> CompanionSettings) = streams { copy(companion = companion.change()) }
}

/** The Camera2 camera to open, and the physical sub-camera inside it to stream from ("" for none). */
data class LensCamera(val id: String, val physical: String = "")

data class LensStreams(
    val raw: RawStreamSettings = RawStreamSettings(),
    /** Full-size RAW that only photos fill (vivo seamless 50/200 MP); null for none. */
    val photo: PhotoStreamSettings? = null,
    val companion: CompanionSettings = CompanionSettings(),
    /** Extra JPEG output configured with the session but never targeted; null for none. */
    val auxJpeg: AuxJpegSettings? = null
)

/** The viewfinder RAW stream: what to ask for, its use case, its buffers, and levels that differ from the lens's. */
data class RawStreamSettings(
    val choice: RawStreamChoice = RawStreamChoice(),
    /** Stream use case; 0 = not set. Needs Java Camera2. */
    val useCase: Int = 0,
    val buffers: BufferSettings = BufferSettings(),
    /** Null inherits the lens levels; explicit dynamic uses the stream's sensor result. */
    val levels: LensLevels? = null
)

data class BufferSettings(
    /** AHB usage the mode needs ([isBufferUsage]); null for the app's own choice. */
    val usage: String? = null,
    /** Buffers the reader holds; 0 keeps the app's 6, otherwise 2 to 16 (below 6 the preview can drop frames). */
    val count: Int = 0,
    /** Check a fixed [usage] with the allocator first and fall back to CPU-only when refused. */
    val testFirst: Boolean = false,
    /** Let a silent fixed reader fall back to CPU-only like the app's own choice does. */
    val fallBack: Boolean = false
)

data class PhotoStreamSettings(
    val choice: RawStreamChoice,
    val useCase: Int = 0,
    /** Buffer usage ([isBufferUsage]); null keeps GPU_DATA_BUFFER. */
    val bufferUsage: String? = null,
    /** Most full-size frames one merged photo may request (1 = single photos only); 0 = the app's own cap. */
    val mergeFrameLimit: Int = 0,
    /** Merged frames go in one by one, each after the previous frame (HAL fails queued ones). */
    val mergeFramesOneByOne: Boolean = false,
    val levels: LensLevels? = null,
    /** Include the viewfinder RAW target in each photo request (legacy behaviour). */
    val includePreview: Boolean = true,
    /** Photo reader maxImages, 2–16; 0 keeps the app default. */
    val bufferCount: Int = 0
)

/** The companion: a small output next to RAW that some HALs need before they deliver RAW. */
enum class CompanionMode(val wireName: String, val label: String) {
    /** Added by the app on unverified devices and high-res photo lenses. */
    Auto("auto", "Automatic"),

    /** Always configured at [CompanionSettings.size]: part of the lens's mode. */
    Fixed("fixed", "Fixed size"),

    /** Never configured, including the automatic high-res photo companion. */
    Off("off", "Off");

    companion object {
        fun fromWire(value: String?): CompanionMode? = entries.firstOrNull { it.wireName == value }
    }
}

data class CompanionSettings(
    val mode: CompanionMode = CompanionMode.Auto,
    /** Used when [mode] is [CompanionMode.Fixed]; kept while another mode is chosen. */
    val size: LensOutputSize = LensOutputSize(320, 240),
    /** "yuv" or "private". */
    val format: String = "yuv",
    val targeted: Boolean = true,
    /** Drop it after the first paired frame; null drops only an automatic one. */
    val detach: Boolean? = null,
    /** Tied to the route's physical camera; null follows the route. */
    val physical: Boolean? = null,
    /** One of [COMPANION_FALLBACKS]; only for an automatic size. */
    val fallback: String = "base",
    /** Buffer usage ([isBufferUsage]); null keeps the default for its format. */
    val bufferUsage: String? = null,
    /** Buffers its reader holds; 0 keeps 2, otherwise 2 to 16. */
    val bufferCount: Int = 0,
    val useCase: Int = 0
)

data class AuxJpegSettings(val size: LensOutputSize, val bufferUsage: String? = null, val useCase: Int = 0)

data class LensSession(
    /** One of [CAMERA_API_CHOICES]. */
    val cameraApi: String = "auto",
    /** SessionConfiguration type; 0 = normal. Needs Java Camera2; 1 and 2 are refused. */
    val opMode: Int = 0,
    /** One of [SESSION_PARAMS_POLICIES]; "none" cannot carry session-scope keys. */
    val params: String = "cadence",
    /** Still template for the session, viewfinder and stills (true), preview for all (false), or the app's mix (null). */
    val stillTemplate: Boolean? = null,
    /** Fixed AE target frame rate on every request; 0 = the app's cadence. */
    val frameRate: Int = 0,
    /** Delay after configure before the first request. */
    val startupDelayMs: Int = 0,
    /** Vendor state held while the camera is open ("vivoFullSize"); empty for none. */
    val gate: String = "",
    /** "configured" or "closed"; null keeps the default (closed with high-res photos). Needs a [gate]. */
    val gateHold: String? = null,
    val stopViewfinderForPhotos: Boolean = false,
    /** Provider behaviours this lens avoids, from [CAMERA_WORKAROUNDS]. */
    val workarounds: List<String> = emptyList()
)

data class LensRequest(
    /** One of [REQUEST_DEFAULTS]. */
    val defaults: String = "rawr"
)

data class LensImage(
    /** Black/white levels of the lens; a stream can override them. */
    val levels: LensLevels = LensLevels(),
    val sensorLayout: LensSensorLayout = LensSensorLayout(),
    /** Lens distortion calibration as its profile JSON object (validated natively); null for none. */
    val distortion: String? = null,
    /** Mirror of the image (photo, video, viewfinder; DNG via its orientation). */
    val mirror: LensMirror = LensMirror.None,
    /** Camera profile (.dcp) replacing the camera's colour; null for the camera's own. */
    val dcp: LensDcp? = null,
    val exposure: LensExposure = LensExposure(),
    /** Imported choices retained even while camera colour is selected. */
    val dcpProfiles: List<LensDcp> = emptyList()
) {
    /** Includes the selection from older settings that stored only one profile. */
    val availableDcps: List<LensDcp> get() = (dcpProfiles + listOfNotNull(dcp)).distinctBy { it.file }

    fun importDcp(profile: LensDcp): LensImage =
        copy(dcp = profile, dcpProfiles = (availableDcps + profile).distinctBy { it.file })

    fun selectDcp(file: String?): LensImage = when {
        file == null -> copy(dcpProfiles = availableDcps, dcp = null)
        else -> availableDcps.firstOrNull { it.file == file }?.let { copy(dcp = it) } ?: this
    }

    fun moveDcp(from: Int, to: Int): LensImage {
        val profiles = availableDcps
        if (from !in profiles.indices || to !in profiles.indices || from == to) return this
        return copy(dcpProfiles = profiles.toMutableList().apply { add(to, removeAt(from)) })
    }

    fun sortDcps(descending: Boolean = false): LensImage {
        val order = compareBy<LensDcp, String>(String.CASE_INSENSITIVE_ORDER) { it.name }
        return copy(dcpProfiles = availableDcps.sortedWith(if (descending) order.reversed() else order))
    }

    fun removeDcp(file: String): LensImage = copy(
        dcpProfiles = availableDcps.filterNot { it.file == file },
        dcp = dcp?.takeUnless { it.file == file }
    )
}

data class LensSensorLayout(
    /**
     * Same-colour block side the RAW really holds (1 Bayer, 2 quad, 4 for 4x4): blocks are remosaiced to Bayer
     * for the viewfinder, stills and video. Some sensor modes (in-sensor crops) report Bayer but aren't.
     */
    val block: Int = 1,
    /** Block-level colour order ("RGGB", "GRBG", "GBRG", "BGGR") replacing the camera's; null keeps it. */
    val colourOrder: String? = null,
    /** Remosaic for the viewfinder, video and merge frames: "fast" or "standard" (GPU). */
    val liveRemosaic: String = "standard",
    /** Remosaic for single photos: "standard" or "high" (CPU). */
    val photoRemosaic: String = "high",
    /** Same-colour block side for the separate photo stream; Bayer by default. */
    val photoBlock: Int = 1
)

/** Width x height of an extra camera output of a lens profile. */
data class LensOutputSize(val width: Int, val height: Int)

/** Lens image mirror in the sensor frame; [wireName] is the profile JSON value. */
enum class LensMirror(val wireName: String, val label: String) {
    None("none", "None"),
    Horizontal("horizontal", "Horizontal"),
    Vertical("vertical", "Vertical"),
    Both("both", "Both (180°)");

    companion object {
        fun fromWire(value: String?): LensMirror = entries.firstOrNull { it.wireName == value } ?: None
    }
}

val SENSOR_LAYOUT_BLOCKS = listOf(1, 2, 4)
val COLOUR_ORDERS = listOf("RGGB", "GRBG", "GBRG", "BGGR")
val LIVE_REMOSAIC_QUALITIES = listOf("fast", "standard")
val PHOTO_REMOSAIC_QUALITIES = listOf("standard", "high")

/** Problems with one lens, in display order; empty when the lens is valid. */
fun validateLensProfile(lens: LensProfile, others: List<LensProfile>): List<String> = buildList {
    val name = lens.name.trim()
    when {
        name.isEmpty() -> add("Name is required")
        name.length > LENS_NAME_MAX_LENGTH -> add("Name must be at most $LENS_NAME_MAX_LENGTH characters")
        others.any { it.name.trim().equals(name, ignoreCase = true) } -> add("Another lens is named $name")
    }
    val session = lens.session
    val streams = lens.streams
    val companion = streams.companion
    if (lens.camera.id.isBlank()) add("Choose a camera")
    streams.photo?.let {
        if (it.choice.width <= 0 || it.choice.height <= 0) add("Photo stream width and height must be positive whole numbers")
        if (it.mergeFrameLimit !in 0..30) add("Merged photo frame limit must be between 0 and 30")
        if (it.bufferCount != 0 && it.bufferCount !in 2..16) add("Photo stream buffer count must be 2 to 16")
    }
    validateLevels(lens.image.levels)?.let { add(it) }
    lens.vendorKeys.forEach { key -> validateVendorKey(key)?.let { add(it) } }
    if (session.cameraApi !in CAMERA_API_CHOICES) add("Unknown camera API ${session.cameraApi}")
    if (session.cameraApi == "native" && lens.needsJavaCameraApi()) add("An operating mode or stream use case needs Java Camera2")
    if (lens.resolvedCameraApi() == "java") {
        if (session.stopViewfinderForPhotos) add("Java Camera2 can't stop the viewfinder stream for photos")
    }
    if (session.opMode < 0 || session.opMode == 1 || session.opMode == 2) add("Operating mode must be 0 or 3 and above")
    if (session.frameRate !in 0..MAX_FRAME_RATE) add("Fixed frame rate must be 0 to $MAX_FRAME_RATE")
    if (listOf(streams.raw.useCase, streams.photo?.useCase ?: 0, companion.useCase, streams.auxJpeg?.useCase ?: 0)
            .any { it < 0 }) add("Stream use cases cannot be negative")
    if (companion.fallback !in COMPANION_FALLBACKS) add("Unknown companion fallback ${companion.fallback}")
    if (companion.fallback != "base" && companion.mode == CompanionMode.Fixed)
        add("A companion fallback needs an automatic companion size")
    if (companion.mode == CompanionMode.Fixed && (companion.size.width <= 0 || companion.size.height <= 0))
        add("Companion width and height must be positive whole numbers")
    if (companion.bufferCount != 0 && companion.bufferCount !in 2..16) add("Companion buffer count must be 2 to 16")
    if (streams.raw.buffers.count != 0 && streams.raw.buffers.count !in 2..16) add("RAW buffer count must be 2 to 16")
    streams.auxJpeg?.let {
        if (it.size.width <= 0 || it.size.height <= 0) add("Aux JPEG width and height must be positive whole numbers")
    }
    for ((label, usage) in listOf("Companion" to companion.bufferUsage, "Aux JPEG" to streams.auxJpeg?.bufferUsage,
        "RAW" to streams.raw.buffers.usage, "Photo stream" to streams.photo?.bufferUsage)) {
        if (usage != null && !isBufferUsage(usage)) add("$label buffer usage must be a name or a 0x hex value")
    }
    if (session.params !in SESSION_PARAMS_POLICIES) add("Unknown session parameters ${session.params}")
    if (lens.request.defaults !in REQUEST_DEFAULTS) add("Unknown request defaults ${lens.request.defaults}")
    if (session.gateHold != null && session.gateHold !in GATE_HOLDS) add("Unknown gate hold ${session.gateHold}")
    if (session.gateHold != null && session.gate.isEmpty()) add("A gate hold needs a vendor gate")
    session.workarounds.filter { it !in CAMERA_WORKAROUNDS }.forEach { add("Unknown workaround $it") }
    if (session.params == "none") {
        lens.vendorKeys
            .firstOrNull { it.enabled && (it.scope == VendorKeyScope.Session || it.scope == VendorKeyScope.SessionAndRequest) }
            ?.let { add("Session parameters \"none\" cannot carry the session key ${it.tag}") }
    }
}

/** Problems with the whole list (per-lens problems included). */
fun validateLensProfiles(lenses: List<LensProfile>): List<String> = buildList {
    if (lenses.none { it.enabled }) add("At least one lens must be enabled")
    lenses.forEachIndexed { index, lens ->
        val others = lenses.filterIndexed { i, _ -> i != index }
        validateLensProfile(lens, others).forEach { add("${lens.name.ifBlank { "Lens ${index + 1}" }}: $it") }
    }
}

fun validateLevels(levels: LensLevels): String? {
    if (!levels.isStatic) return null
    if (levels.blackRggb.size != 4 || levels.blackRggb.any { it < 0f }) return "Black levels must be 0 or more"
    if (levels.white <= levels.blackRggb.max()) return "White level must be above the black levels"
    return null
}

fun validateVendorKey(key: VendorKey): String? {
    if (key.tag.isBlank()) return "Key name is required"
    if (key.tag.trim().any { it.isWhitespace() })
        return "Key name cannot contain whitespace; enter type and value in their separate fields"
    if (key.values.isEmpty()) return "${key.tag}: enter at least one value"
    val range: ClosedFloatingPointRange<Double>? =
        when (key.type) {
            VendorKeyType.Byte -> 0.0..255.0
            VendorKeyType.Int32 -> Int.MIN_VALUE.toDouble()..Int.MAX_VALUE.toDouble()
            VendorKeyType.Int64 -> Long.MIN_VALUE.toDouble()..Long.MAX_VALUE.toDouble()
            else -> null
        }
    if (key.type.integral && key.values.any { it != Math.rint(it) }) return "${key.tag}: ${key.type.label} needs whole numbers"
    if (range != null && key.values.any { it !in range }) return "${key.tag}: value out of ${key.type.label} range"
    return null
}

/** Parses comma/space separated values ("31", "1, 2, 3"); null when any entry isn't a number. */
fun parseVendorKeyValues(text: String): List<Double>? {
    val parts = text.split(',', ' ', ';').map { it.trim() }.filter { it.isNotEmpty() }
    return parts.map { it.toDoubleOrNull()?.takeIf { v -> v.isFinite() } ?: return null }
}

fun formatVendorKeyValues(values: List<Double>, type: VendorKeyType): String =
    values.joinToString(", ") { if (type.integral) it.toLong().toString() else it.toString() }

/** The lens list the camera uses: the user's list, or the device's built-in lenses. */
fun SettingsValues.effectiveLensProfiles(deviceDefaults: List<LensProfile>): List<LensProfile> =
    lensProfiles ?: deviceDefaults

fun List<LensProfile>.moveLens(from: Int, to: Int): List<LensProfile> {
    if (from !in indices || to !in indices || from == to) return this
    return toMutableList().apply { add(to, removeAt(from)) }
}

/** Disabling the last enabled lens is refused (the camera needs one). */
fun List<LensProfile>.setLensEnabled(index: Int, enabled: Boolean): List<LensProfile> {
    if (index !in indices) return this
    if (!enabled && count { it.enabled } <= 1 && this[index].enabled) return this
    return mapIndexed { i, lens -> if (i == index) lens.copy(enabled = enabled) else lens }
}

/** Replaces the lens named [originalName] (case-insensitive), or appends when it is null or missing. */
fun List<LensProfile>.upsertLens(originalName: String?, lens: LensProfile): List<LensProfile> {
    val trimmed = lens.copy(name = lens.name.trim())
    val index = originalName?.let { name -> indexOfFirst { it.name.equals(name, ignoreCase = true) } } ?: -1
    return if (index >= 0) toMutableList().apply { this[index] = trimmed } else this + trimmed
}

/** Removing the only enabled lens is refused. */
fun List<LensProfile>.removeLens(name: String): List<LensProfile> {
    val next = filterNot { it.name.equals(name, ignoreCase = true) }
    return if (next.none { it.enabled }) this else next
}

/** A lens's name not yet used in the list ("L1", "L2", ...). */
fun List<LensProfile>.newLensName(): String =
    generateSequence(1) { it + 1 }.map { "L$it" }.first { name -> none { it.name.equals(name, ignoreCase = true) } }
