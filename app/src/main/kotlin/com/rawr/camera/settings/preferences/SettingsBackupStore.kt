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
class SettingsBackupStore(context: Context, private val defaults: SettingsValues) {
    private val app = context.applicationContext
    private val repository = SettingsPreferencesRepository(app, defaults)

    fun export(values: SettingsValues, sections: Set<BackupSection>, output: java.io.OutputStream) {
        require(sections.isNotEmpty()) { "Nothing selected to back up" }
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
            val prefs = repository.encodeForBackup(values)
            zip.write(encodePreferences(prefs) {
                it == SCHEMA_KEY || (it !in DEVICE_LOCAL_KEYS && BackupSection.sectionOf(it) in sections)
            }.toString(2).toByteArray())
            zip.closeEntry()
            if (BackupSection.LensProfiles in sections) {
                values.lensProfiles.orEmpty().mapNotNull { it.dcp?.file }.distinct().forEach { name ->
                    val file = File(File(app.filesDir, DcpFileStore.DIR), name)
                    if (!file.isFile) return@forEach
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
                    if (!file.isFile) return@forEach
                    zip.putNextEntry(ZipEntry(LUT_DIR + file.name))
                    file.inputStream().use { it.copyTo(zip) }
                    zip.closeEntry()
                }
        }
    }

    /** Reads and validates a backup; LUT files are staged until [apply] or [discard]. */
    fun read(uri: Uri): BackupContents {
        var settingsJson: JSONObject? = null
        var sections: Set<BackupSection>? = null
        val staging = File(app.cacheDir, "backup_restore").apply { deleteRecursively(); mkdirs() }
        val dcpStaging = File(app.cacheDir, "backup_restore_dcp").apply { deleteRecursively(); mkdirs() }
        try {
            app.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "Unable to open backup" }
                ZipInputStream(input.buffered()).use { zip ->
                    var total = 0L
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        if (entry.isDirectory) continue
                        when {
                            entry.name == MANIFEST -> {
                                val manifest = JSONObject(zip.readBytes().toString(Charsets.UTF_8))
                                require(manifest.optInt("format") in 1..FORMAT_VERSION) {
                                    "Backup was made by a newer version of Rawr"
                                }
                                val ids = manifest.optJSONArray("sections")
                                sections = (0 until (ids?.length() ?: 0)).mapNotNull { BackupSection.fromId(ids!!.getString(it)) }.toSet()
                            }

                            entry.name == SETTINGS -> {
                                settingsJson = JSONObject(zip.readBytes().toString(Charsets.UTF_8))
                            }

                            entry.name.startsWith(LUT_DIR) -> {
                                val name = entry.name.removePrefix(LUT_DIR)
                                if (!name.matches(SAFE_NAME) || !name.endsWith(".cube", true)) continue
                                val out = File(staging, name)
                                out.outputStream().use { zip.copyTo(it) }
                                total += out.length()
                                require(total <= MAX_LUT_BYTES) { "Backup LUTs are too large" }
                            }

                            entry.name.startsWith(DCP_DIR) -> {
                                val name = entry.name.removePrefix(DCP_DIR)
                                if (!DcpFileStore.isFileName(name)) continue
                                val out = File(dcpStaging, name)
                                val bytes = zip.readNBytes(DcpFileStore.MAX_BYTES + 1)
                                require(bytes.size <= DcpFileStore.MAX_BYTES) { "Camera profile is too large" }
                                out.writeBytes(bytes)
                                total += out.length()
                                require(total <= MAX_LUT_BYTES) { "Backup files are too large" }
                            }
                        }
                    }
                }
            }
            val found = requireNotNull(sections) { "Not a Rawr backup" }
            require(found.isNotEmpty()) { "Backup is empty" }
            val prefs = decodePreferences(requireNotNull(settingsJson) { "Backup has no settings" })
            val values =
                requireNotNull(repository.decodeBackup(prefs)) { "Backup was made by a newer version of Rawr" }
            return BackupContents(found, values, staging, dcpStaging)
        } catch (t: Throwable) {
            staging.deleteRecursively()
            dcpStaging.deleteRecursively()
            throw t
        }
    }

    /** Returns [current] with the chosen [sections] taken from the backup. */
    fun apply(contents: BackupContents, sections: Set<BackupSection>, current: SettingsValues): SettingsValues {
        try {
            val backup = contents.values
            val chosen = sections intersect contents.sections
            var next =
                if (BackupSection.Settings in chosen) {
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
                } else {
                    current
                }
            if (BackupSection.LensProfiles in chosen) next = next.copy(lensProfiles = restoreDcpFiles(contents, backup.lensProfiles))
            if (BackupSection.Luts in chosen) {
                val lutFiles = File(app.filesDir, LUT_FILES_PATH).apply { mkdirs() }
                contents.lutFiles.listFiles()?.forEach { it.copyTo(File(lutFiles, it.name), overwrite = true) }
                // Keep only profiles whose LUT files are on this phone now.
                val profiles =
                    backup.userLutProfiles.filter { profile ->
                        profile.stages.all { stage ->
                            stage.relativePath.startsWith("$LUT_FILES_PATH/") &&
                                File(app.filesDir, stage.relativePath).isFile
                        }
                    }
                next =
                    next.copy(
                        userLutProfiles = profiles,
                        selectedUserLutProfileId = backup.selectedUserLutProfileId,
                        videoUserLutProfileId = backup.videoUserLutProfileId
                    )
            }
            // A render profile restored without its LUT (or a LUT selection without the profile) falls back.
            val ids = next.userLutProfiles.map { it.id }.toSet()
            if (next.selectedUserLutProfileId !in ids) next = next.copy(selectedUserLutProfileId = null)
            if (next.videoUserLutProfileId !in ids) next = next.copy(videoUserLutProfileId = null)
            if (next.colorRenderProfile == ColorRenderProfile.UserLut && next.selectedUserLutProfileId == null) {
                next = next.copy(colorRenderProfile = defaults.colorRenderProfile)
            }
            if (next.videoColorRenderProfile == ColorRenderProfile.UserLut && next.videoUserLutProfileId == null) {
                next = next.copy(videoColorRenderProfile = defaults.videoColorRenderProfile)
            }
            return next
        } finally {
            discard(contents)
        }
    }

    fun discard(contents: BackupContents) {
        contents.lutFiles.deleteRecursively()
        contents.dcpFiles.deleteRecursively()
    }

    /** Installs the backup's camera profiles; a lens whose profile didn't come back keeps the camera's colour. */
    private fun restoreDcpFiles(contents: BackupContents, lenses: List<LensProfile>?): List<LensProfile>? {
        return DcpFileStore(app).restore(contents.dcpFiles, lenses)
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
            val entry = json.optJSONObject(name) ?: return@forEach
            when (entry.optString("type")) {
                "boolean" -> p[booleanPreferencesKey(name)] = entry.getBoolean("value")
                "int" -> p[intPreferencesKey(name)] = entry.getInt("value")
                "float" -> p[floatPreferencesKey(name)] = entry.getDouble("value").toFloat()
                "string" -> p[stringPreferencesKey(name)] = entry.getString("value")
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
        const val MAX_LUT_BYTES = 512L * 1024L * 1024L
        val SAFE_NAME = Regex("[A-Za-z0-9._-]+")
    }
}
