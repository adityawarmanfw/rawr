package com.rawr.camera.settings.model

import java.io.File
import kotlin.test.*

/**
 * [AgxPreset] defaults and [AgxCatalog] keys mirror kAgxPresetControls and
 * kAgxControlKeys in native Agx.h. Native renders whatever controls the app
 * sends, so a drift would make a preset's "default" differ from darktable's
 * and break recipe keys.
 */
class AgxPresetSyncTest {
    private fun header(): String {
        val start = File(System.getProperty("user.dir").orEmpty()).absoluteFile
        val header = generateSequence(start) { it.parentFile }
            .map { File(it, "native/tonemap_engine/include/tonemap/Agx.h") }
            .firstOrNull { it.isFile } ?: fail("Agx.h not found above $start")
        return header.readText()
    }

    private fun nativeTable(): List<List<Float>> {
        val table = header().substringAfter("AGX_PRESET_CONTROLS_BEGIN").substringBefore("AGX_PRESET_CONTROLS_END")
            .lines().joinToString("\n") { it.substringBefore("//") }
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

    @Test fun controlKeysAndOrderMatchNative() {
        val block = header().substringAfter("AGX_CONTROL_KEYS_BEGIN").substringBefore("AGX_CONTROL_KEYS_END")
        val keys = Regex("\"([A-Za-z]+)\"").findAll(block).map { it.groupValues[1] }.toList()
        assertEquals(keys, AgxCatalog.all.map { it.jsonKey })
        assertEquals(AgxParameter.entries.toList(), AgxCatalog.all.map { it.parameter })
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
