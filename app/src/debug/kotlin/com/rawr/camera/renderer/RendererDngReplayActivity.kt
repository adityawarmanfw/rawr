package com.rawr.camera.renderer

import android.app.Activity
import android.os.Bundle
import android.os.ParcelFileDescriptor
import org.json.JSONObject
import java.io.File

/** Headless DNG replay. The host tool stages files in this private directory. */
class RendererDngReplayActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Thread {
            val dir = File(filesDir, "dng_replay")
            val result = JSONObject().put("version", 1)
            var handle = 0L
            try {
                val inputName = intent.getStringExtra("inputName") ?: "input.dng"
                check(inputName == "input.dng" || Regex("input-[0-9a-f]{16}\\.dng").matches(inputName)) {
                    "Invalid replay input name"
                }
                val input = File(dir, inputName)
                check(input.isFile && input.length() > 0L) { "Missing staged $inputName" }
                handle = RendererNative.open(input.absolutePath, filesDir.absolutePath, applicationInfo.nativeLibraryDir, assets)
                val inspected = RendererNative.inspect(handle).split('\n', limit = 5)
                check(inspected.size == 5) { "DNG inspection did not return provenance" }
                val width = inspected[0].toInt()
                val height = inspected[1].toInt()
                val override = File(dir, "recipe.json")
                val recipe = if (override.isFile) JSONObject(override.readText()) else {
                    val extensions = JSONObject(inspected[4]).getJSONObject("extensions")
                    val original = JSONObject(extensions.getString("com.rawrcam.processing.recipe.v1"))
                    check(original.isNull("importedProfileId")) { "Replay needs the imported LUT asset" }
                    val resolved = JSONObject(extensions.getString("com.rawrcam.processing.resolved.v1"))
                    val frame = JSONObject(extensions.getString("com.rawrcam.processing.frame.v1"))
                    resolved.optJSONObject("tonemap")?.optJSONObject("adaptiveTone")?.let {
                        original.put("adaptiveTone", it)
                    }
                    original.put("aePostGain", resolved.getDouble("aePostGain"))
                        .put("timestampNs", frame.getLong("timestampNs"))
                        .put("resolvedFrame", frame)
                }
                check(recipe.getInt("version") == 1) { "Unsupported replay recipe version" }
                intent.getStringExtra("cfaName")?.let { name ->
                    check(Regex("[0-9a-f]{16}\\.f32").matches(name)) { "Invalid replay CFA name" }
                    check(File(dir, name).isFile) { "Missing replay CFA" }
                    recipe.put("replayCfaOverride", name)
                }
                val diagnostics = intent.getBooleanExtra("diagnostics", true)
                recipe.put("replayDiagnostics", diagnostics)
                if (diagnostics) {
                    for (name in listOf("rawrcam_diag_manifest_latest.txt", "rawrcam_diag_pre_wb_latest.rgba16f",
                        "rawrcam_diag_pre_fcc_latest.rgba16f", "rawrcam_diag_pre_tonemap_latest.rgba16f",
                        "rawrcam_prejpeg_latest.ppm")) File(filesDir, name).delete()
                }
                val out = File(dir, "render.jpg")
                ParcelFileDescriptor.open(out, ParcelFileDescriptor.MODE_CREATE or ParcelFileDescriptor.MODE_TRUNCATE or ParcelFileDescriptor.MODE_READ_WRITE).use { fd ->
                    RendererNative.render(handle, recipe.toString(), width, height, fd.fd)
                }
                check(out.isFile && out.length() > 0L) { "Renderer produced an empty JPEG" }
                val dumps = mutableListOf<String>()
                if (diagnostics) {
                    for (name in listOf("rawrcam_diag_manifest_latest.txt", "rawrcam_diag_pre_wb_latest.rgba16f",
                        "rawrcam_diag_pre_fcc_latest.rgba16f", "rawrcam_diag_pre_tonemap_latest.rgba16f",
                        "rawrcam_prejpeg_latest.ppm")) {
                        val source = File(filesDir, name)
                        if (source.isFile) {
                            source.copyTo(File(dir, name), overwrite = true)
                            dumps += name
                        }
                    }
                }
                result.put("success", true).put("width", width).put("height", height)
                    .put("bytes", out.length()).put("ultraHdrRequested", recipe.optBoolean("ultraHdrEnabled"))
                    .put("diagnostics", diagnostics).put("dumps", org.json.JSONArray(dumps))
            } catch (error: Throwable) {
                result.put("success", false).put("error", error.stackTraceToString())
                android.util.Log.e("RendererDngReplay", "Replay failed", error)
            } finally {
                if (handle != 0L) RendererNative.close(handle)
                dir.mkdirs()
                File(dir, "result.json.tmp").writeText(result.toString())
                File(dir, "result.json.tmp").renameTo(File(dir, "result.json"))
                runOnUiThread { finish() }
            }
        }.start()
    }
}
