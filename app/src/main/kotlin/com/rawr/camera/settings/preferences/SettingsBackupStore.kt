package com.rawr.camera.settings.preferences

import android.content.Context
import android.net.Uri
import android.os.Build
import androidx.datastore.preferences.core.MutablePreferences
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.mutablePreferencesOf
import androidx.datastore.preferences.core.stringPreferencesKey
import com.rawr.camera.BuildConfig
import com.rawr.camera.settings.model.ColorRenderProfile
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.SettingsValues
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.io.ByteArrayOutputStream
import java.util.UUID
import com.rawr.camera.integration.DeviceLensDefaults
import com.rawr.camera.settings.model.effectiveLensProfiles
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream
import org.json.JSONArray
import org.json.JSONObject

/** Parts of a backup the user can include on export and apply on import. */
enum class BackupSection(val id: String, val label: String, val defaultOn: Boolean) {
    Settings("settings", "Settings", true),
    LensProfiles("lens_profiles", "Lens profiles", true),
    /** The .cube files plus each profile's gamut/transfer/tone parameters and the selected LUTs. */
    Luts("luts", "LUT profiles", false);

    companion object {
        private val lensKeys = setOf("lens_profiles")
        private val lutKeys = setOf("user_lut_profiles", "selected_user_lut_profile_id", "video_user_lut_profile_id")

        internal fun sectionOf(key: String): BackupSection =
            when (key) {
                in lensKeys -> LensProfiles
                in lutKeys -> Luts
                else -> Settings
            }

        fun fromId(id: String): BackupSection? = entries.firstOrNull { it.id == id }
    }
}

/** What [SettingsBackupStore.read] found, ready to apply once the user picks sections. */
class BackupContents internal constructor(
    val sections: Set<BackupSection>,
    internal val values: SettingsValues,
    internal val lutFiles: File,
    internal val dcpFiles: File
)

/**
 * Backs up global settings, lens profiles and (opt-in) LUT profiles as a plain zip:
 *
 *   manifest.json   app version, device identity (no serials), creation time, included sections
 *   settings.json   each persisted key of the included sections as {"type": ..., "value": ...}
 *   luts/<name>     every .cube file a LUT profile references (LUT section only)
 *   dcp/<name>      every camera profile (.dcp) a lens references (lens profile section only)
 *
 * Import decodes the keys through the normal settings migrations, so a backup from an older
 * version imports like an upgrade, then applies only the sections the user picks. Device-local
 * state (save folder grant, last lens, GPU driver) is neither exported nor taken from a backup.
 */
class SettingsBackupStore(
    context: Context,
    private val defaults: SettingsValues,
    private val deviceLenses: () -> List<LensProfile> = { DeviceLensDefaults.lenses }
) {
    private val app = context.applicationContext
    private val repository = SettingsPreferencesRepository(app, defaults)

    fun export(values: SettingsValues, sections: Set<BackupSection>, output: java.io.OutputStream) {
        require(sections.isNotEmpty()) { "Nothing selected to back up" }
        val exported = if (BackupSection.LensProfiles in sections) {
            values.copy(lensProfiles = values.effectiveLensProfiles(deviceLensesIfNeeded(values)))
        } else values
        ZipOutputStream(output.buffered()).use { zip ->
            zip.putNextEntry(ZipEntry(MANIFEST))
            zip.write(
                JSONObject()
                    .put("format", FORMAT_VERSION)
                    .put("appVersion", BuildConfig.VERSION_NAME)
                    .put("appVersionCode", BuildConfig.VERSION_CODE)
                    .put("appBuildType", BuildConfig.BUILD_TYPE)
                    .put("appId", BuildConfig.APPLICATION_ID)
                    .put("manufacturer", Build.MANUFACTURER)
                    .put("brand", Build.BRAND)
                    .put("model", Build.MODEL)
                    .put("device", Build.DEVICE)
                    .put("product", Build.PRODUCT)
                    .put("androidVersion", Build.VERSION.RELEASE)
                    .put("sdk", Build.VERSION.SDK_INT)
                    .put("buildId", Build.ID)
                    .put("fingerprint", Build.FINGERPRINT)
                    .apply { if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) put("socModel", Build.SOC_MODEL) }
                    .put("createdAtMs", System.currentTimeMillis())
                    .put("sections", JSONArray(sections.map { it.id }))
                    .toString(2)
                    .toByteArray()
            )
            zip.closeEntry()
            zip.putNextEntry(ZipEntry(SETTINGS))
            val prefs = repository.encodeForBackup(exported)
            zip.write(encodePreferences(prefs) {
                it == SCHEMA_KEY || (it !in DEVICE_LOCAL_KEYS && BackupSection.sectionOf(it) in sections)
            }.toString(2).toByteArray())
            zip.closeEntry()
            if (BackupSection.LensProfiles in sections) {
                exported.lensProfiles.orEmpty().mapNotNull { it.dcp?.file }.distinct().forEach { name ->
                    val file = File(File(app.filesDir, DcpFileStore.DIR), name)
                    require(file.isFile) { "Backup is incomplete: missing ${file.name}" }
                    zip.putNextEntry(ZipEntry(DCP_DIR + name))
                    file.inputStream().use { it.copyTo(zip) }
                    zip.closeEntry()
                }
            }
            if (BackupSection.Luts !in sections) return@use
            values.userLutProfiles
                .flatMap { it.stages }
                .map { it.relativePath }
                .distinct()
                .forEach { path ->
                    val file = File(app.filesDir, path)
                    require(file.isFile) { "Backup is incomplete: missing ${file.name}" }
                    zip.putNextEntry(ZipEntry(LUT_DIR + file.name))
                    file.inputStream().use { it.copyTo(zip) }
                    zip.closeEntry()
                }
        }
    }

    /** Reads and validates a backup; LUT files are staged until [prepare] or [discard]. */
    fun read(uri: Uri): BackupContents {
        var settingsJson: JSONObject? = null
        var sections: Set<BackupSection>? = null
        val staging = File(app.cacheDir, "backup_restore_${UUID.randomUUID()}").apply { check(mkdirs()) }
        val dcpStaging = File(staging, "dcp").apply { check(mkdirs()) }
        try {
            app.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "Unable to open backup" }
                ZipInputStream(input.buffered()).use { zip ->
                    var total = 0L
                    val names = mutableSetOf<String>()
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        require(names.add(entry.name)) { "Duplicate backup entry: ${entry.name}" }
                        require(names.size <= 4096) { "Too many backup entries" }
                        fun copyEntry(out: OutputStream, limit: Long) {
                            total += copyBackupEntry(zip, out, minOf(limit, MAX_LUT_BYTES - total))
                        }
                        fun jsonEntry(): JSONObject {
                            val bytes = ByteArrayOutputStream()
                            copyEntry(bytes, MAX_JSON_BYTES)
                            return JSONObject(bytes.toString(Charsets.UTF_8.name()))
                        }
                        when {
                            entry.isDirectory -> copyEntry(OutputStream.nullOutputStream(), MAX_LUT_BYTES)
                            entry.name == MANIFEST -> {
                                val manifest = jsonEntry()
                                require(manifest.optInt("format") in 1..FORMAT_VERSION) {
                                    "Backup was made by a newer version of Rawr"
                                }
                                val ids = manifest.optJSONArray("sections")
                                sections = (0 until (ids?.length() ?: 0)).mapNotNull { BackupSection.fromId(ids!!.getString(it)) }.toSet()
                            }

                            entry.name == SETTINGS -> {
                                settingsJson = jsonEntry()
                            }

                            entry.name.startsWith(LUT_DIR) -> {
                                val name = entry.name.removePrefix(LUT_DIR)
                                require(name.matches(SAFE_NAME) && name.endsWith(".cube", true)) { "Invalid LUT filename" }
                                val out = File(staging, name)
                                out.outputStream().use { copyEntry(it, MAX_CUBE_BYTES) }
                            }

                            entry.name.startsWith(DCP_DIR) -> {
                                val name = entry.name.removePrefix(DCP_DIR)
                                require(DcpFileStore.isFileName(name)) { "Invalid camera profile filename" }
                                val out = File(dcpStaging, name)
                                out.outputStream().use { copyEntry(it, DcpFileStore.MAX_BYTES.toLong()) }
                            }
                            else -> copyEntry(OutputStream.nullOutputStream(), MAX_LUT_BYTES)
                        }
                    }
                }
            }
            val found = requireNotNull(sections) { "Not a Rawr backup" }
            require(found.isNotEmpty()) { "Backup is empty" }
            val json = requireNotNull(settingsJson) { "Backup has no settings" }
            require(json.has(SCHEMA_KEY)) { "Backup has no settings schema" }
            require(json.getJSONObject(SCHEMA_KEY).getInt("value") >= 0) { "Invalid settings schema" }
            if (BackupSection.Settings in found) {
                val known = repository.encodeForBackup(defaults).asMap().keys.map { it.name }.toSet()
                require(json.keys().asSequence().any {
                    it != SCHEMA_KEY && it !in DEVICE_LOCAL_KEYS &&
                        it in known && BackupSection.sectionOf(it) == BackupSection.Settings
                }) { "Backup has no settings values" }
            }
            if (BackupSection.Luts in found) require(json.has("user_lut_profiles")) { "Backup has no LUT profiles" }
            if (BackupSection.Luts in found) {
                LutProfileCodec.decodeBackup(json.getJSONObject("user_lut_profiles").getString("value"))
            }
            if (BackupSection.LensProfiles in found && json.has("lens_profiles")) {
                val raw = json.getJSONObject("lens_profiles").getString("value")
                LensProfileCodec.decodeBackup(raw)
            }
            val prefs = decodePreferences(json)
            val values =
                requireNotNull(repository.decodeBackup(prefs)) { "Backup was made by a newer version of Rawr" }
            return BackupContents(found, values, staging, dcpStaging)
        } catch (t: Throwable) {
            staging.deleteRecursively()
            dcpStaging.deleteRecursively()
            throw t
        }
    }

    private fun deviceLensesIfNeeded(values: SettingsValues): List<LensProfile> =
        if (values.lensProfiles == null) deviceLenses().also {
            require(it.isNotEmpty()) { "Device lens profiles are unavailable" }
        } else emptyList()

    /**
     * Installs LUTs under fresh names, so failed preparation cannot overwrite live assets.
     * The returned edit is applied to the latest settings by SettingsEditor.update.
     */
    fun prepare(contents: BackupContents, sections: Set<BackupSection>): PreparedBackup {
        val chosen = sections intersect contents.sections
        require(chosen.isNotEmpty()) { "Nothing selected to restore" }
        val installed = mutableListOf<File>()
        try {
            var backup = contents.values
            if (BackupSection.Luts in chosen) {
                val paths = mutableMapOf<String, String>()
                val profiles = backup.userLutProfiles.map { profile ->
                    require(profile.id.matches(SAFE_NAME)) { "Invalid LUT profile ID" }
                    profile.copy(stages = profile.stages.map { stage ->
                        val path = paths.getOrPut(stage.relativePath) {
                            val name = stage.relativePath.removePrefix("$LUT_FILES_PATH/")
                            require(stage.relativePath == "$LUT_FILES_PATH/$name" &&
                                name.matches(SAFE_NAME) && name.endsWith(".cube", true)) { "Invalid LUT path" }
                            val source = File(contents.lutFiles, name)
                            require(source.isFile) { "Backup is incomplete: missing $name" }
                            val normalized = source.bufferedReader().useLines { CubeNormalizer.normalize(it) }
                            val dir = File(app.filesDir, LUT_FILES_PATH).apply { check(mkdirs() || isDirectory) }
                            val target = File(dir, "${UUID.randomUUID()}.cube")
                            installed += target
                            target.outputStream().use { output ->
                                output.bufferedWriter().use { out ->
                                    normalized.forEach { out.write(it); out.newLine() }
                                    out.flush()
                                    output.fd.sync()
                                }
                            }
                            "$LUT_FILES_PATH/${target.name}"
                        }
                        stage.copy(relativePath = path)
                    })
                }
                backup = backup.copy(userLutProfiles = profiles)
            }
            if (BackupSection.LensProfiles in chosen) {
                // DCP names are content hashes; verify the archive before installing anything.
                backup.lensProfiles.orEmpty().mapNotNull { it.dcp }.forEach { dcp ->
                    val file = File(contents.dcpFiles, dcp.file)
                    require(DcpFileStore.isFileName(dcp.file) && file.isFile &&
                        file.length() <= DcpFileStore.MAX_BYTES &&
                        DcpFileStore.fileName(file.readBytes()) == dcp.file) {
                        "Backup has a missing or damaged camera profile: ${dcp.name}"
                    }
                }
                val lenses = DcpFileStore(app).restore(contents.dcpFiles, backup.lensProfiles)
                require(lenses == backup.lensProfiles) { "Backup contains an invalid camera profile" }
            }
            return PreparedBackup(backup, chosen, defaults, installed)
        } catch (t: Throwable) {
            installed.forEach { it.delete() }
            throw t
        } finally {
            discard(contents)
        }
    }

    fun discard(contents: BackupContents) {
        contents.lutFiles.deleteRecursively()
        contents.dcpFiles.deleteRecursively()
    }

    private fun encodePreferences(p: Preferences, include: (String) -> Boolean): JSONObject {
        val json = JSONObject()
        p.asMap().toSortedMap(compareBy { it.name }).forEach { (key, value) ->
            if (!include(key.name)) return@forEach
            val type =
                when (value) {
                    is Boolean -> "boolean"
                    is Int -> "int"
                    is Float -> "float"
                    is String -> "string"
                    else -> return@forEach
                }
            json.put(key.name, JSONObject().put("type", type).put("value", value))
        }
        return json
    }

    private fun decodePreferences(json: JSONObject): Preferences {
        val p: MutablePreferences = mutablePreferencesOf()
        json.keys().forEach { name ->
            val entry = requireNotNull(json.optJSONObject(name)) { "Invalid backup preference: $name" }
            when (entry.optString("type")) {
                "boolean" -> p[booleanPreferencesKey(name)] = entry.getBoolean("value")
                "int" -> p[intPreferencesKey(name)] = entry.getInt("value")
                "float" -> p[floatPreferencesKey(name)] = entry.getDouble("value").toFloat()
                "string" -> p[stringPreferencesKey(name)] = entry.getString("value")
                else -> error("Unsupported backup preference type")
            }
        }
        return p
    }

    private companion object {
        const val FORMAT_VERSION = 1
        const val MANIFEST = "manifest.json"
        const val SETTINGS = "settings.json"
        const val SCHEMA_KEY = "settings_schema_version"
        /** Only meaningful on this phone (folder grant, lens, installed driver); never exported. */
        val DEVICE_LOCAL_KEYS =
            setOf(
                "save_location_id",
                "last_lens_id",
                "debug_custom_gpu_driver_enabled",
                "debug_custom_gpu_driver_name"
            )
        const val LUT_DIR = "luts/"
        const val DCP_DIR = "dcp/"
        const val LUT_FILES_PATH = "lut_profiles/files"
        const val MAX_JSON_BYTES = 8L * 1024L * 1024L
        const val MAX_CUBE_BYTES = 32L * 1024L * 1024L
        const val MAX_LUT_BYTES = 512L * 1024L * 1024L
        val SAFE_NAME = Regex("[A-Za-z0-9._-]+")
    }
}


/** A bounded copy checks before writing, including entries with unknown ZIP sizes. */
internal fun copyBackupEntry(input: InputStream, output: OutputStream, limit: Long): Long {
    var count = 0L
    val buffer = ByteArray(8192)
    while (true) {
        val size = input.read(buffer, 0, minOf(buffer.size.toLong(), limit - count + 1).toInt())
        if (size < 0) return count
        count += size
        require(count <= limit) { "Backup entry is too large" }
        output.write(buffer, 0, size)
    }
}

class PreparedBackup internal constructor(
    private val backup: SettingsValues,
    private val chosen: Set<BackupSection>,
    private val defaults: SettingsValues,
    private val installed: List<File>
) {
    /** Remove newly installed LUTs if publishing the settings fails. */
    fun discard() { installed.forEach { it.delete() } }

    fun merge(current: SettingsValues): SettingsValues {
        var next = if (BackupSection.Settings in chosen) {
            backup.copy(
                lensProfiles = current.lensProfiles,
                userLutProfiles = current.userLutProfiles,
                selectedUserLutProfileId = current.selectedUserLutProfileId,
                videoUserLutProfileId = current.videoUserLutProfileId,
                saveLocationId = current.saveLocationId,
                lastLensId = current.lastLensId,
                customGpuDriverEnabled = current.customGpuDriverEnabled,
                customGpuDriverName = current.customGpuDriverName
            )
        } else current
        if (BackupSection.LensProfiles in chosen) next = next.copy(lensProfiles = backup.lensProfiles)
        if (BackupSection.Luts in chosen) next = next.copy(
            userLutProfiles = backup.userLutProfiles,
            selectedUserLutProfileId = backup.selectedUserLutProfileId,
            videoUserLutProfileId = backup.videoUserLutProfileId
        )
        if (BackupSection.Settings in chosen || BackupSection.Luts in chosen) {
            val ids = next.userLutProfiles.map { it.id }.toSet()
            if (next.selectedUserLutProfileId !in ids) next = next.copy(selectedUserLutProfileId = null)
            if (next.videoUserLutProfileId !in ids) next = next.copy(videoUserLutProfileId = null)
            if (next.colorRenderProfile == ColorRenderProfile.UserLut && next.selectedUserLutProfileId == null) {
                next = next.copy(colorRenderProfile = defaults.colorRenderProfile)
            }
            if (next.videoColorRenderProfile == ColorRenderProfile.UserLut && next.videoUserLutProfileId == null) {
                next = next.copy(videoColorRenderProfile = defaults.videoColorRenderProfile)
            }
        }
        return next
    }
}
