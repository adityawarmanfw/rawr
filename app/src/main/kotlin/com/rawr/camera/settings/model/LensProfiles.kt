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

    /** Only on full-size still requests of a capture-stream lens (vivo 50/200 MP). */
    Still("still", "Still");

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
 * Preferred RAW stream; a zero size means the largest size of the format. [unlisted]: a vendor full-size mode
 * the camera doesn't advertise, configured at exactly this size.
 */
data class RawStreamChoice(
    val format: RawStreamFormat = RawStreamFormat.Raw16,
    val width: Int = 0,
    val height: Int = 0,
    val unlisted: Boolean = false
) {
    val isLargest: Boolean get() = width <= 0 || height <= 0
    val label: String get() = if (isLargest) "${format.label} (largest)" else "${format.label} ${width}×$height"
}

data class LensProfile(
    val name: String,
    val cameraId: String,
    val enabled: Boolean = true,
    /** Physical sub-camera of [cameraId] to stream from; empty for none. */
    val physicalCameraId: String = "",
    val stream: RawStreamChoice = RawStreamChoice(),
    val levels: LensLevels = LensLevels(),
    val exposure: LensExposure = LensExposure(),
    val vendorKeys: List<VendorKey> = emptyList(),
    /** RAW reader usage the mode needs ("gpuDataBuffer", "none"); null for the app's own choice. */
    val readerUsage: String? = null,
    /** Vendor state held while the camera is open ("vivoFullSize"); empty for none. */
    val vendorGate: String = "",
    /** Full-size RAW stream only still requests fill (vivo seamless 50/200 MP); null for none. */
    val captureStream: RawStreamChoice? = null,
    /** Most full-size stills one merge may request (1 = none); 0 = the app's own cap. */
    val captureBurst: Int = 0,
    /** Burst stills go in one at a time, each after the previous frame (HAL fails queued ones). */
    val captureBurstSequential: Boolean = false,
    /**
     * Same-colour block side the RAW really holds (1 Bayer, 2 quad, 4 for 4x4): blocks are remosaiced to Bayer
     * for the viewfinder, stills and video. Some sensor modes (in-sensor crops) report Bayer but aren't.
     */
    val cfaBlock: Int = 1,
    /** Block-level colour order ("RGGB", "GRBG", "GBRG", "BGGR") replacing the camera's; null keeps it. */
    val cfaOrder: String? = null
)

val CFA_BLOCKS = listOf(1, 2, 4)
val CFA_ORDERS = listOf("RGGB", "GRBG", "GBRG", "BGGR")

/** Problems with one lens, in display order; empty when the lens is valid. */
fun validateLensProfile(lens: LensProfile, others: List<LensProfile>): List<String> = buildList {
    val name = lens.name.trim()
    when {
        name.isEmpty() -> add("Name is required")
        name.length > LENS_NAME_MAX_LENGTH -> add("Name must be at most $LENS_NAME_MAX_LENGTH characters")
        others.any { it.name.trim().equals(name, ignoreCase = true) } -> add("Another lens is named $name")
    }
    if (lens.cameraId.isBlank()) add("Choose a camera")
    validateLevels(lens.levels)?.let { add(it) }
    lens.vendorKeys.forEach { key -> validateVendorKey(key)?.let { add(it) } }
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
