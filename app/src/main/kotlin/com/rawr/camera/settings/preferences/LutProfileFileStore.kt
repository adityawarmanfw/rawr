package com.rawr.camera.settings.preferences

import android.content.Context
import android.net.Uri
import com.rawr.camera.settings.model.*
import java.io.File
import java.util.UUID

class LutProfileFileStore(private val context: Context) {
    private val root = File(context.filesDir, "lut_profiles")
    private val cubeDir = File(root, "files")

    fun importCube(uri: Uri): ImportedLutStage {
        root.mkdirs()
        cubeDir.mkdirs()
        val displayName = queryName(uri)?.takeIf { it.endsWith(".cube", true) } ?: "Imported.cube"
        val id = UUID.randomUUID().toString()
        val dst = File(cubeDir, "$id.cube")
        try {
            context.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "Unable to open LUT" }
                dst.outputStream().use { input.copyTo(it) }
            }
            validateCube(dst)
            return ImportedLutStage(id, displayName, "lut_profiles/files/${dst.name}")
        } catch (t: Throwable) {
            dst.delete()
            throw t
        }
    }

    fun sync(
        profiles: List<ImportedLutProfile>,
        previousProfiles: List<ImportedLutProfile> = emptyList()
    ) = synchronized(CaptureLutPins.lock) {
        root.mkdirs()
        cubeDir.mkdirs()
        val removed = removedLutAssets(previousProfiles, profiles)
        val cubeRoot = cubeDir.canonicalFile
        removed.stagePaths.forEach { path ->
            val file = File(context.filesDir, path).canonicalFile
            if (file.parentFile == cubeRoot && file.extension.equals("cube", true) &&
                !file.name.startsWith("capture_") && !CaptureLutPins.contains(path)
            ) file.delete()
        }
        removed.profileIds.filter { it.matches(Regex("[A-Za-z0-9._-]+")) && !it.startsWith("capture_") }.forEach { id ->
            File(root, "$id.rawrprofile").delete()
        }
        val previousById = previousProfiles.associateBy { it.id }
        profiles.filter { previousById[it.id] != it }.forEach(::writeMetadata)
    }

    /** A per-job immutable copy, excluded from settings-library garbage collection. */
    fun freezeForCapture(profile: ImportedLutProfile): ImportedLutProfile {
        val id = "capture_" + UUID.randomUUID().toString()
        root.mkdirs(); cubeDir.mkdirs()
        try {
            val stages = profile.stages.mapIndexed { index, stage ->
                val name = "${id}_$index.cube"
                val source = File(context.filesDir, stage.relativePath)
                val target = File(cubeDir, name)
                source.inputStream().use { input ->
                    java.io.FileOutputStream(target).use { output -> input.copyTo(output); output.fd.sync() }
                }
                stage.copy(id = "${id}_$index", relativePath = "lut_profiles/files/$name")
            }
            return profile.copy(id = id, stages = stages).also {
                writeMetadata(it)
                java.io.FileOutputStream(File(root, "$id.rawrprofile"), true).use { out -> out.fd.sync() }
            }
        } catch (error: Exception) {
            cubeDir.listFiles()?.filter { it.name.startsWith(id + "_") }?.forEach { it.delete() }
            File(root, "$id.rawrprofile").delete()
            throw error
        }
    }

    private fun writeMetadata(profile: ImportedLutProfile) {
        require(profile.id.matches(Regex("[A-Za-z0-9._-]+")))
        File(root, "${profile.id}.rawrprofile").writeText(
            buildString {
                appendLine("version=1")
                appendLine("input_gamut=${profile.inputGamut.nativeId}")
                appendLine("input_transfer=${profile.inputTransfer.nativeId}")
                appendLine("output_gamut=${profile.outputGamut.nativeId}")
                appendLine("output_transfer=${profile.outputTransfer.nativeId}")
                appendLine("after_lut=${if (profile.afterLut == AfterLutAction.UseDirectly) 0 else 1}")
                appendLine("tone_render_exposure=${profile.tone.renderExposure}")
                appendLine("tone_blacks=${profile.tone.blacks}")
                appendLine("tone_shadows=${profile.tone.shadows}")
                appendLine("tone_contrast=${profile.tone.contrast}")
                appendLine("tone_midtones=${profile.tone.midtones}")
                appendLine("tone_highlights=${profile.tone.highlights}")
                appendLine("tone_whites=${profile.tone.whites}")
                appendLine("tone_saturation=${profile.tone.saturation}")
                appendLine("tone_vibrance=${profile.tone.vibrance}")
                profile.stages.forEach { appendLine("stage=${it.relativePath}") }
            }
        )
    }

    /** Rewrites the imported file in the normalized form the renderer can always load, or throws why it cannot. */
    private fun validateCube(file: File) {
        val cleaned = file.bufferedReader().useLines { CubeNormalizer.normalize(it) }
        file.bufferedWriter().use { out ->
            cleaned.forEach {
                out.write(it)
                out.newLine()
            }
        }
    }

    private fun queryName(uri: Uri): String? = context.contentResolver
        .query(
            uri,
            arrayOf(android.provider.OpenableColumns.DISPLAY_NAME),
            null,
            null,
            null
        )?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
}

/** Admission pins cover the interval before the worker creates its durable per-job copies. */
internal object CaptureLutPins {
    val lock = Any()
    private val counts = mutableMapOf<String, Int>()
    fun contains(path: String): Boolean = synchronized(lock) { path in counts }
    fun acquire(profile: ImportedLutProfile?): () -> Unit {
        val paths = profile?.stages.orEmpty().map { it.relativePath }.distinct()
        synchronized(lock) { paths.forEach { counts[it] = (counts[it] ?: 0) + 1 } }
        var released = false
        return {
            synchronized(lock) {
                if (!released) {
                    released = true
                    paths.forEach { path ->
                        val remaining = (counts[path] ?: 1) - 1
                        if (remaining == 0) counts.remove(path) else counts[path] = remaining
                    }
                }
            }
        }
    }
}
