package com.rawr.camera.settings.model

import java.io.File
import kotlin.test.*

/**
 * [AgxPreset] defaults mirror kAgxPresetControls in native Agx.h: native
 * rebuilds each preset from its id and overrides only these five controls,
 * so a drift would make the app's "default" differ from what renders.
 */
class AgxPresetSyncTest {
    private fun nativeTable(): List<List<Float>> {
        val start = File(System.getProperty("user.dir").orEmpty()).absoluteFile
        val header = generateSequence(start) { it.parentFile }
            .map { File(it, "native/tonemap_engine/include/tonemap/Agx.h") }
            .firstOrNull { it.isFile } ?: fail("Agx.h not found above $start")
        val text = header.readText()
        val table = text.substringAfter("AGX_PRESET_CONTROLS_BEGIN").substringBefore("AGX_PRESET_CONTROLS_END")
        val body = table.substringAfter("= {")
        return Regex("""\{([^{}]*)\}""").findAll(body).map { row ->
            row.groupValues[1].split(',').map { it.trim().removeSuffix("f").toFloat() }
        }.toList()
    }

    @Test fun presetDefaultsMatchNative() {
        val rows = nativeTable()
        assertEquals(5, rows.size)  // native keeps all five darktable presets
        assertEquals(AgxPreset.entries.size, AgxPreset.entries.map { it.nativeId }.distinct().size)
        AgxPreset.entries.forEach { preset ->
            assertEquals(rows[preset.nativeId], preset.defaults.toNativeArray().toList(), "preset ${preset.name}")
        }
    }

    @Test fun presetDefaultsAreWithinSliderRanges() {
        AgxPreset.entries.forEach { preset ->
            AgxCatalog.all.forEach { spec ->
                val v = preset.defaults.valueOf(spec.parameter)
                assertTrue(v in spec.minimum..spec.maximum, "${preset.name} ${spec.jsonKey}=$v")
            }
        }
    }
}
