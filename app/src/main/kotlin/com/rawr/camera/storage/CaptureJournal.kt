package com.rawr.camera.storage

import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.Properties

/** Output identities survive process death; native commits matching <name>.job input files. */
internal class CaptureJournal(private val directory: File, private val syncDirectory: (File) -> Unit = {}) {
    data class Target(
        val uri: String, val name: String, val published: Boolean = false,
        val role: String = "", val conditional: Boolean = false, val skipped: Boolean = false
    )
    data class Entry(
        val name: String, val targets: List<Target>, val multiframe: Boolean = false,
        val capturedAt: Long = 0, val recipe: String = "", val fallback: Boolean = false,
        val lastError: String = ""
    ) {
        val complete get() = targets.all { it.published || it.skipped }
        /** Resolve durable decisions before any provider FD is reopened. */
        fun resolveForRecovery(memoryFallback: Boolean): Entry {
            if (targets.any { it.role == "jpeg" && it.published }) {
                return copy(targets = targets.map { if (it.conditional) it.copy(skipped = true) else it })
            }
            if (!memoryFallback) return this
            return copy(fallback = true, targets = targets.map {
                if (it.role == "jpeg") it.copy(skipped = true) else it.copy(conditional = false)
            })
        }
        val status get() = when {
            complete -> "complete"
            lastError.isNotEmpty() -> "failed"
            fallback -> "dng_fallback"
            targets.any { it.published } -> "partially_published"
            else -> "pending"
        }
    }

    fun write(entry: Entry) {
        require(entry.name.isNotBlank() && '/' !in entry.name && ".." !in entry.name)
        directory.mkdirs()
        val properties = Properties().apply {
            setProperty("version", "2")
            setProperty("multiframe", entry.multiframe.toString())
            setProperty("capturedAt", entry.capturedAt.toString())
            setProperty("recipe", entry.recipe)
            setProperty("fallback", entry.fallback.toString())
            setProperty("lastError", entry.lastError)
            setProperty("name", entry.name)
            setProperty("count", entry.targets.size.toString())
            entry.targets.forEachIndexed { i, target ->
                setProperty("$i.uri", target.uri)
                setProperty("$i.name", target.name)
                setProperty("$i.published", target.published.toString())
                setProperty("$i.role", target.role)
                setProperty("$i.conditional", target.conditional.toString())
                setProperty("$i.skipped", target.skipped.toString())
            }
        }
        val target = File(directory, entry.name + ".properties")
        val temporary = File(directory, entry.name + ".properties.tmp")
        FileOutputStream(temporary).use { properties.store(it, null); it.fd.sync() }
        Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
        syncDirectory(directory)
    }

    fun readAll(): List<Entry> = directory.listFiles().orEmpty()
        .filter { it.name.endsWith(".properties") }.sortedBy { it.name }
        .mapNotNull { file ->
            runCatching {
                val p = Properties().apply { file.inputStream().use { load(it) } }
                check(p.getProperty("version") in setOf("1", "2"))
                val name = p.getProperty("name") ?: error("Missing capture name")
                check(file.name == "$name.properties" && '/' !in name && ".." !in name)
                val count = p.getProperty("count").toInt().also { check(it in 1..3) }
                val names = List(count) { checkNotNull(p.getProperty("$it.name")) }
                val multiframe = p.getProperty("multiframe")?.toBooleanStrictOrNull()
                    ?: (names.count { it.endsWith(".dng") } == 2)
                Entry(name, List(count) { i ->
                    val role = p.getProperty("$i.role") ?: when {
                        names[i].endsWith(".jpg") -> "jpeg"
                        multiframe && i == 0 -> "base"
                        multiframe -> "merged"
                        else -> "single"
                    }
                    Target(checkNotNull(p.getProperty("$i.uri")), names[i], p.getProperty("$i.published") == "true",
                        role, p.getProperty("$i.conditional") == "true", p.getProperty("$i.skipped") == "true")
                }, multiframe, p.getProperty("capturedAt")?.toLongOrNull() ?: 0,
                    p.getProperty("recipe").orEmpty(), p.getProperty("fallback") == "true", p.getProperty("lastError").orEmpty())
            }.getOrNull() // Preserve unsupported/corrupt journals for inspection.
        }

    fun hasFallback(entry: Entry) = entry.fallback || File(directory, entry.name + ".job.fallback").isFile

    fun hasInput(entry: Entry) = File(directory, entry.name + ".job").isFile

    /**
     * Best-effort quarantine marker for entries whose outputs are permanently
     * unavailable (e.g. the gallery file was deleted while the payload
     * survived). The journal + payload are preserved so a future repair pass
     * can still use them; recovery just stops attempting (and logging) them.
     * Invisible to readAll(); cleared by remove().
     */
    fun isSkipped(entry: Entry): Boolean = File(directory, entry.name + SKIPPED_SUFFIX).isFile

    fun markSkipped(entry: Entry) {
        require(entry.name.isNotBlank() && '/' !in entry.name && ".." !in entry.name)
        directory.mkdirs()
        File(directory, entry.name + SKIPPED_SUFFIX).writeText("unrecoverable\n")
        syncDirectory(directory)
    }

    fun remove(entry: Entry) {
        // Remove payload first: an interrupted cleanup leaves a completed journal,
        // which recovery can safely finish without rendering or duplicating outputs.
        File(directory, entry.name + ".job").delete()
        File(directory, entry.name + ".job.tmp").delete()
        File(directory, entry.name + ".job.fallback").delete()
        File(directory, entry.name + ".job.fallback.tmp").delete()
        File(directory, entry.name + ".properties").delete()
        File(directory, entry.name + SKIPPED_SUFFIX).delete()
        File(directory, entry.name + StillOutputStore.RENDERER_EDITED).delete()
        val assetId = Regex("\"importedProfileId\":\"(capture_[A-Za-z0-9-]+)\"").find(entry.recipe)?.groupValues?.get(1)
        if (assetId != null) releaseCaptureAssets(requireNotNull(directory.parentFile), assetId)
        syncDirectory(directory)
    }

    private companion object {
        const val SKIPPED_SUFFIX = ".recovery-skipped"
    }
}

/** Only per-job copies are removed; imported user assets are never touched. */
internal fun releaseCaptureAssets(filesDir: File, id: String) {
    if (!id.matches(Regex("capture_[A-Za-z0-9-]+"))) return
    val root = File(filesDir, "lut_profiles")
    File(root, "$id.rawrprofile").delete()
    File(root, "files").listFiles()?.filter { it.name.startsWith("${id}_") && it.extension == "cube" }?.forEach { it.delete() }
}
