package com.rawr.camera.settings.preferences

import android.content.Context
import android.net.Uri
import com.rawr.camera.integration.NativePreviewEngine
import com.rawr.camera.settings.model.LensDcp
import com.rawr.camera.settings.model.LensProfile
import java.io.File
import java.security.MessageDigest

/**
 * Lens camera profiles (.dcp) in app storage, named by content hash so equal files share one copy and a
 * backup restores them under the same name. Capture jobs keep their own copy of the profile, so files no
 * lens references are deleted on [sync].
 */
class DcpFileStore internal constructor(
    val dir: File,
    private val inspect: (String) -> Array<String>,
    private val open: (Uri) -> java.io.InputStream?
) {
    constructor(context: Context) : this(
        File(context.filesDir, DIR),
        { NativePreviewEngine().inspectCameraProfile(it) },
        { context.contentResolver.openInputStream(it) }
    )

    /** Copies and validates the picked file; throws with a user-facing message when the app can't use it. */
    fun import(uri: Uri): LensDcp {
        val bytes = requireNotNull(open(uri)) { "Unable to open the profile" }.use { input ->
            input.readNBytes(MAX_BYTES + 1)
        }
        require(bytes.size <= MAX_BYTES) { "Not a camera profile (file too large)" }
        return add(bytes)
    }

    /** Stores [bytes] under its content name and reads its profile name natively. */
    fun add(bytes: ByteArray): LensDcp {
        require(bytes.size <= MAX_BYTES) { "Not a camera profile (file too large)" }
        dir.mkdirs()
        val name = fileName(bytes)
        val target = File(dir, name)
        val staged = File.createTempFile("import-", ".tmp", dir)
        try {
            staged.writeBytes(bytes)
            val (error, profileName) = inspect(staged.absolutePath)
            require(error.isEmpty()) { "Can't use this profile: $error" }
            check(staged.renameTo(target)) { "Unable to store the profile" }
            // A fresh import isn't referenced until the lens is saved: sync() keeps it a while.
            target.setLastModified(System.currentTimeMillis())
            return LensDcp(name, profileName.ifBlank { name.removeSuffix(".dcp") })
        } finally {
            staged.delete()
        }
    }

    /** Deletes stored profiles no lens references, except recent imports a lens editor may still save. */
    fun sync(lenses: List<LensProfile>?) {
        val used = lenses.orEmpty().mapNotNull { it.dcp?.file }.toSet()
        val cutoff = System.currentTimeMillis() - UNSAVED_GRACE_MS
        dir.listFiles()?.forEach { file -> if (file.name !in used && file.lastModified() < cutoff) file.delete() }
    }

    /** Restore only referenced, intact, usable profiles; missing profiles fall back to camera colour. */
    internal fun restore(staging: File, lenses: List<LensProfile>?): List<LensProfile>? {
        lenses.orEmpty().mapNotNull { it.dcp?.file }.distinct().forEach { name ->
            if (!isFileName(name)) return@forEach
            val file = File(staging, name)
            if (!file.isFile || file.length() > MAX_BYTES) return@forEach
            val bytes = file.readBytes()
            if (fileName(bytes) != name) return@forEach
            try {
                add(bytes)
            } catch (_: IllegalArgumentException) {
                // An invalid profile must not prevent restoring the lens itself.
            }
        }
        return lenses?.map { lens ->
            val dcp = lens.dcp
            if (dcp != null && (!isFileName(dcp.file) || !File(dir, dcp.file).isFile)) lens.copy(dcp = null) else lens
        }
    }

    companion object {
        const val DIR = "dcp_profiles"
        internal const val MAX_BYTES = 16 * 1024 * 1024
        private const val UNSAVED_GRACE_MS = 24L * 60 * 60 * 1000
        private val NAME = Regex("[0-9a-f]{32}\\.dcp")

        fun isFileName(name: String): Boolean = NAME.matches(name)

        fun fileName(bytes: ByteArray): String =
            MessageDigest.getInstance("SHA-256").digest(bytes).take(16).joinToString("") { "%02x".format(it) } + ".dcp"
    }
}
