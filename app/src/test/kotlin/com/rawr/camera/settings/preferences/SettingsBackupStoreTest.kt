package com.rawr.camera.settings.preferences

import android.content.Context
import android.net.Uri
import android.os.Bundle
import androidx.test.core.app.ApplicationProvider
import com.rawr.camera.settings.BackupExportSelection
import com.rawr.camera.settings.architecture.SettingsEditor
import com.rawr.camera.settings.model.*
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlin.test.*
import kotlinx.coroutines.async
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.json.JSONObject
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
@OptIn(ExperimentalCoroutinesApi::class)
class SettingsBackupStoreTest {
    private val app = ApplicationProvider.getApplicationContext<Context>()
    private val defaults = SettingsCatalog.initialState().values
    private val builtIn = listOf(LensProfile(name = "W", cameraId = "0"))
    private val store = SettingsBackupStore(app, defaults) { builtIn }
    private val schema = """"settings_schema_version":{"type":"int","value":38}"""
    private val cube = (listOf("LUT_3D_SIZE 2") + List(8) { "0 0 0" }).joinToString(10.toChar().toString())

    private fun temp(): File = File(app.cacheDir, UUID.randomUUID().toString())
    private fun export(values: SettingsValues, sections: Set<BackupSection>): Uri {
        val file = temp()
        file.outputStream().use { store.export(values, sections, it) }
        return Uri.fromFile(file)
    }
    private fun archive(sections: String, settings: String, assets: Map<String, String> = emptyMap()): Uri {
        val file = temp()
        ZipOutputStream(file.outputStream()).use { zip ->
            val entries = mapOf("manifest.json" to """{"format":1,"sections":[$sections]}""",
                "settings.json" to settings) + assets
            entries.forEach { (name, value) ->
                zip.putNextEntry(ZipEntry(name))
                zip.write(value.toByteArray())
                zip.closeEntry()
            }
        }
        return Uri.fromFile(file)
    }
    private fun profile(name: String = "one"): ImportedLutProfile =
        ImportedLutProfile(name, name, listOf(ImportedLutStage(name, "$name.cube", "lut_profiles/files/$name.cube")))
    private fun staged(values: SettingsValues, files: Map<String, String>): BackupContents {
        val dir = temp().apply { mkdirs() }
        files.forEach { (name, text) -> File(dir, name).writeText(text) }
        return BackupContents(BackupSection.entries.toSet(), values, dir, File(dir, "dcp").apply { mkdirs() })
    }

    @Test fun untouchedDefaultsExportConcreteLensDefinitions() {
        val contents = store.read(export(defaults, setOf(BackupSection.LensProfiles)))
        assertEquals(builtIn, contents.values.lensProfiles)
        val current = defaults.copy(lensProfiles = listOf(LensProfile("T", "3")), locationTagging = true)
        val result = store.prepare(contents, setOf(BackupSection.LensProfiles)).merge(current)
        assertEquals(current.copy(lensProfiles = builtIn), result)
        assertNull(defaults.lensProfiles)
    }

    @Test fun customLensesDoNotQueryDeviceDefaults() {
        val custom = defaults.copy(lensProfiles = builtIn)
        SettingsBackupStore(app, defaults) { error("Must not query native defaults") }
            .export(custom, setOf(BackupSection.LensProfiles), ByteArrayOutputStream())
    }

    @Test fun allSectionCombinationsRoundTripAndPreserveUnselectedValues() {
        val source = defaults.copy(locationTagging = true, lensProfiles = builtIn)
        val current = defaults.copy(lensProfiles = listOf(LensProfile("T", "3")), lastLensId = "T",
            saveLocationId = "storage.custom", customGpuDriverEnabled = true, customGpuDriverName = "driver")
        for (mask in 1..7) {
            val sections = BackupSection.entries.filterIndexed { i, _ -> mask and (1 shl i) != 0 }.toSet()
            val result = store.prepare(store.read(export(source, sections)), sections).merge(current)
            val expected = current.copy(
                locationTagging = if (BackupSection.Settings in sections) true else current.locationTagging,
                lensProfiles = if (BackupSection.LensProfiles in sections) builtIn else current.lensProfiles)
            assertEquals(expected, result, "Sections: $sections")
        }
    }

    @Test fun lensOnlyRestoreLeavesEvenInvalidLutSelectionUntouched() {
        val current = defaults.copy(selectedUserLutProfileId = "missing",
            videoUserLutProfileId = "missing", colorRenderProfile = ColorRenderProfile.UserLut,
            videoColorRenderProfile = ColorRenderProfile.UserLut)
        val result = store.prepare(store.read(export(defaults, setOf(BackupSection.LensProfiles))),
            setOf(BackupSection.LensProfiles)).merge(current)
        assertEquals(current.copy(lensProfiles = builtIn), result)
    }

    @Test fun exportFailsForMissingLutAndDcp() {
        assertFailsWith<IllegalArgumentException> {
            export(defaults.copy(userLutProfiles = listOf(profile("absent"))), setOf(BackupSection.Luts))
        }
        assertFailsWith<IllegalArgumentException> {
            export(defaults.copy(lensProfiles = listOf(builtIn.first().copy(
                dcp = LensDcp("0".repeat(32) + ".dcp", "Missing")))), setOf(BackupSection.LensProfiles))
        }
    }

    @Test fun emptyOrSchemaOnlySettingsAreRejected() {
        for (json in listOf("{}", "{$schema}", """{$schema,"unknown":{"type":"int","value":1}}""")) {
            assertFailsWith<IllegalArgumentException> { store.read(archive(""""settings"""", json)) }
        }
    }

    @Test fun legacyDefaultLensBackupStillRestoresDefaults() {
        val contents = store.read(archive(""""lens_profiles"""", "{$schema}"))
        val result = store.prepare(contents, setOf(BackupSection.LensProfiles))
            .merge(defaults.copy(lensProfiles = builtIn))
        assertNull(result.lensProfiles)
    }

    @Test fun readsAndCancellationHaveIndependentStagingDirectories() {
        val uri = export(defaults, setOf(BackupSection.Settings))
        val first = store.read(uri)
        val marker = File(first.lutFiles, "first.cube").apply { writeText(cube) }
        val second = store.read(uri)
        assertNotEquals(first.lutFiles, second.lutFiles)
        store.discard(second)
        assertTrue(marker.isFile)
        store.discard(first)
        assertFalse(marker.exists())
    }

    @Test fun boundedCopyRejectsBeforeWritingBeyondLimit() {
        val out = ByteArrayOutputStream()
        assertFailsWith<IllegalArgumentException> {
            copyBackupEntry(ByteArrayInputStream(ByteArray(20_000)), out, 10_000)
        }
        assertTrue(out.size() <= 10_000)
        val exact = ByteArrayOutputStream()
        assertEquals(10L, copyBackupEntry(ByteArrayInputStream(ByteArray(10)), exact, 10))
        assertEquals(10, exact.size())
    }

    @Test fun oversizedJsonIsRejectedAndStagingIsRemoved() {
        val before = app.cacheDir.listFiles()!!.filter { it.name.startsWith("backup_restore_") }.toSet()
        assertFailsWith<IllegalArgumentException> {
            store.read(archive(""""settings"""", " ".repeat(8 * 1024 * 1024 + 1)))
        }
        assertEquals(before, app.cacheDir.listFiles()!!.filter { it.name.startsWith("backup_restore_") }.toSet())
    }

    @Test fun invalidAndMissingLutFilesRejectRestore() {
        for (files in listOf(mapOf("one.cube" to "not a LUT"), emptyMap())) {
            val contents = staged(defaults.copy(userLutProfiles = listOf(profile())), files)
            assertFailsWith<IllegalArgumentException> { store.prepare(contents, setOf(BackupSection.Luts)) }
            assertFalse(contents.lutFiles.exists())
        }
    }

    @Test fun failedPreparationLeavesExistingFilesAndRemovesNewFiles() {
        val dir = File(app.filesDir, "lut_profiles/files").apply { mkdirs() }
        val original = File(dir, "one.cube").apply { writeText(cube) }
        val before = dir.listFiles()!!.toSet()
        val contents = staged(defaults.copy(userLutProfiles = listOf(profile(), profile("two"))),
            mapOf("one.cube" to cube, "two.cube" to "invalid"))
        assertFailsWith<IllegalArgumentException> { store.prepare(contents, setOf(BackupSection.Luts)) }
        assertEquals(cube, original.readText())
        assertEquals(before, dir.listFiles()!!.toSet())
    }

    @Test fun lutRoundTripInstallsFreshValidatedFilesAndCanDiscard() {
        val dir = File(app.filesDir, "lut_profiles/files").apply { mkdirs() }
        File(dir, "one.cube").writeText(cube)
        val source = defaults.copy(userLutProfiles = listOf(profile()), selectedUserLutProfileId = "one")
        val prepared = store.prepare(store.read(export(source, setOf(BackupSection.Luts))), setOf(BackupSection.Luts))
        val result = prepared.merge(defaults)
        val restored = result.userLutProfiles.single().stages.single()
        assertNotEquals(profile().stages.single().relativePath, restored.relativePath)
        assertEquals("one", result.selectedUserLutProfileId)
        val file = File(app.filesDir, restored.relativePath)
        assertEquals(CubeNormalizer.normalize(cube.lineSequence()), file.readLines())
        prepared.discard()
        assertFalse(file.exists())
        assertEquals(cube, File(dir, "one.cube").readText())
    }

    @Test fun restoreMergesIntoLatestEditorState() = runTest {
        val editor = SettingsEditor(defaults, flowOf(defaults), {}, backgroundScope)
        runCurrent()
        val prepared = store.prepare(store.read(export(defaults, setOf(BackupSection.LensProfiles))),
            setOf(BackupSection.LensProfiles))
        editor.update { it.copy(locationTagging = true, jpegEnabled = false) }
        editor.update(prepared::merge)
        assertTrue(editor.state.value.locationTagging)
        assertFalse(editor.state.value.jpegEnabled)
        assertEquals(builtIn, editor.state.value.lensProfiles)
    }

    @Test fun exportSelectionSurvivesSavedStateAndIsConsumedOnce() {
        val original = BackupExportSelection().apply { sections = setOf(BackupSection.LensProfiles) }
        val state = Bundle()
        original.save(state)
        val recreated = BackupExportSelection().apply { restore(state) }
        assertEquals(setOf(BackupSection.LensProfiles), recreated.take())
        assertNull(recreated.take())
        val consumed = Bundle()
        recreated.save(consumed)
        assertNull(BackupExportSelection().apply { restore(consumed) }.take())
    }

    @Test fun damagedDcpCannotSilentlyDisappearDuringRestore() {
        val dcp = LensDcp("0".repeat(32) + ".dcp", "Damaged")
        val contents = staged(defaults.copy(lensProfiles = listOf(builtIn.first().copy(dcp = dcp))), emptyMap())
        File(contents.dcpFiles, dcp.file).writeText("bad")
        assertFailsWith<IllegalArgumentException> { store.prepare(contents, setOf(BackupSection.LensProfiles)) }
    }

    @Test fun currentBackupCanImportOnlyOneOfItsSections() {
        val uri = export(defaults.copy(locationTagging = true, lensProfiles = builtIn), BackupSection.entries.toSet())
        val result = store.prepare(store.read(uri), setOf(BackupSection.LensProfiles)).merge(defaults)
        assertFalse(result.locationTagging)
        assertEquals(builtIn, result.lensProfiles)
    }

    @Test fun oversizedCompressedLutIsRejectedDuringExtraction() {
        val file = temp()
        ZipOutputStream(file.outputStream()).use { zip ->
            zip.putNextEntry(ZipEntry("luts/huge.cube"))
            val block = ByteArray(8192)
            repeat(32 * 1024 * 1024 / block.size + 1) { zip.write(block) }
            zip.closeEntry()
        }
        assertFailsWith<IllegalArgumentException> { store.read(Uri.fromFile(file)) }
    }

    @Test fun oldSettingsSchemaIsMigratedAndNewerSchemaRejected() {
        val oldSchema = schema.replace("38", "37")
        val contents = store.read(archive(""""settings"""",
            """{$oldSchema,"location_tagging":{"type":"boolean","value":true}}"""))
        store.discard(contents)
        assertFailsWith<IllegalArgumentException> {
            store.read(archive(""""settings"""",
                """{${schema.replace("38", "999")},"location_tagging":{"type":"boolean","value":true}}"""))
        }
    }

    @Test fun settingsOnlyExportDoesNotResolveDeviceLenses() {
        SettingsBackupStore(app, defaults) { error("Not exporting lenses") }
            .export(defaults, setOf(BackupSection.Settings), ByteArrayOutputStream())
    }

    @Test fun malformedLutRecordsRejectTheWholeBackup() {
        val valid = LutProfileCodec.encode(listOf(profile()))
        for (raw in listOf("broken", valid + 10.toChar() + "broken",
            valid + 10.toChar() + valid, valid.substringBeforeLast('|') + "|broken")) {
            val settings = JSONObject("{$schema}").put("user_lut_profiles",
                JSONObject().put("type", "string").put("value", raw)).toString()
            assertFailsWith<IllegalArgumentException> {
                store.read(archive(""""luts"""", settings))
            }
        }
    }

    @Test fun emptyAndLegacyLutLibrariesRemainValid() {
        val valid = LutProfileCodec.encode(listOf(profile()))
        assertTrue(LutProfileCodec.decodeBackup("").isEmpty())
        assertEquals(listOf(profile()), LutProfileCodec.decodeBackup(valid.substringBeforeLast('|')))
    }

    @Test fun malformedLensRecordsRejectTheWholeBackup() {
        val raw = """{"lenses":[{"id":"W","cameraId":"0"},{"id":"missing-camera"}]}"""
        val settings = JSONObject("{$schema}").put("lens_profiles",
            JSONObject().put("type", "string").put("value", raw)).toString()
        assertFailsWith<IllegalArgumentException> { store.read(archive(""""lens_profiles"""", settings)) }
    }

    @Test fun failedSaveKeepsDurableAndLiveLutsThenSuccessfulRetryCleansOldFiles() = runTest {
        val original = File(app.filesDir, "lut_profiles/files/one.cube")
        original.parentFile!!.mkdirs()
        original.writeText(cube)
        val old = defaults.copy(userLutProfiles = listOf(profile()), selectedUserLutProfileId = "one")
        var durable = old
        var fail = true
        val files = SettingsLutFiles(LutProfileFileStore(app))
        val editor = SettingsEditor(defaults, flowOf(old), {
            if (fail) error("disk full")
            durable = it
        }, backgroundScope, onChanged = files::changed, onPersisted = files::persisted)
        runCurrent()
        val prepared = store.prepare(store.read(export(old, setOf(BackupSection.Luts))), setOf(BackupSection.Luts))
        val result = async { runCatching { editor.updateAndAwaitPersistence(prepared::merge) } }
        runCurrent()
        assertTrue(result.await().isFailure)
        assertEquals(old, durable)
        assertEquals(cube, original.readText())
        val restored = File(app.filesDir, editor.state.value.userLutProfiles.single().stages.single().relativePath)
        assertTrue(restored.isFile)
        // A process restart can still load the old durable library.
        SettingsLutFiles(LutProfileFileStore(app)).changed(durable)
        assertTrue(original.isFile)
        fail = false
        val retry = async { editor.updateAndAwaitPersistence { it } }
        runCurrent()
        retry.await()
        assertEquals(editor.state.value, durable)
        assertFalse(original.exists())
        assertTrue(restored.isFile)
    }

    @Test fun oldSaveCompletionDoesNotDeleteNewerLiveLut() = runTest {
        val original = File(app.filesDir, "lut_profiles/files/one.cube")
        original.parentFile!!.mkdirs()
        original.writeText(cube)
        val old = defaults.copy(userLutProfiles = listOf(profile()))
        val gate = CompletableDeferred<Unit>()
        var count = 0
        val files = SettingsLutFiles(LutProfileFileStore(app))
        val editor = SettingsEditor(defaults, flowOf(old), {
            if (++count == 1) gate.await() else error("second save fails")
        }, backgroundScope, onChanged = files::changed, onPersisted = files::persisted)
        runCurrent()
        editor.update { it.copy(locationTagging = true) }
        runCurrent()
        val prepared = store.prepare(store.read(export(old, setOf(BackupSection.Luts))), setOf(BackupSection.Luts))
        val restore = async { runCatching { editor.updateAndAwaitPersistence(prepared::merge) } }
        runCurrent()
        val latest = File(app.filesDir, editor.state.value.userLutProfiles.single().stages.single().relativePath)
        gate.complete(Unit)
        runCurrent()
        assertTrue(restore.await().isFailure)
        assertTrue(original.isFile)
        assertTrue(latest.isFile)
    }

    @Test fun failedMetadataPublicationLeavesExistingProfileAndSettingsUntouched() = runTest {
        val original = File(app.filesDir, "lut_profiles/files/one.cube")
        original.parentFile!!.mkdirs()
        original.writeText(cube)
        val old = defaults.copy(userLutProfiles = listOf(profile()))
        val files = SettingsLutFiles(LutProfileFileStore(app))
        val editor = SettingsEditor(defaults, flowOf(old), {}, backgroundScope,
            onChanged = files::changed, onPersisted = files::persisted)
        runCurrent()
        val root = File(app.filesDir, "lut_profiles")
        val metadata = File(root, "one.rawrprofile")
        val before = metadata.readText()
        val backup = old.copy(userLutProfiles = listOf(profile(), profile().copy(id = "two")))
        val prepared = store.prepare(store.read(export(backup, setOf(BackupSection.Luts))), setOf(BackupSection.Luts))
        File(root, "two.rawrprofile").mkdirs()
        val result = async { runCatching { editor.updateAndAwaitPersistence(prepared::merge) } }
        runCurrent()
        assertTrue(result.await().isFailure)
        assertEquals(old, editor.state.value)
        assertEquals(before, metadata.readText())
        assertTrue(root.listFiles()!!.none { it.name.startsWith(".metadata-") })
        prepared.discard()
    }

    @Test fun replacementFailureRollsBackBothExistingAndNewMetadata() {
        val root = temp().apply { mkdirs() }
        val existing = File(root, "one.rawrprofile").apply { writeText("original") }
        var replacements = 0
        assertFailsWith<IllegalStateException> {
            replaceLutMetadata(root, linkedMapOf("one.rawrprofile" to "changed", "two.rawrprofile" to "new")) { source, target ->
                check(source.renameTo(target))
                if (++replacements == 2) error("injected replacement failure")
            }
        }
        assertEquals("original", existing.readText())
        assertEquals(listOf(existing), root.listFiles()!!.toList())
    }

    @Test fun successfulBatchPublishesEveryMetadataFile() {
        val root = temp().apply { mkdirs() }
        File(root, "one.rawrprofile").writeText("original")
        replaceLutMetadata(root, linkedMapOf("one.rawrprofile" to "changed", "two.rawrprofile" to "new"))
        assertEquals("changed", File(root, "one.rawrprofile").readText())
        assertEquals("new", File(root, "two.rawrprofile").readText())
        assertEquals(2, root.listFiles()!!.size)
    }

    @Test fun nestedLensDamageIsRejectedThroughBackupRead() {
        for (nested in listOf(
            """"dcp":{"file":"bad.dcp","name":"Lost"}""",
            """"keys":[{"values":[1]}]"""
        )) {
            val raw = """{"lenses":[{"id":"W","cameraId":"0",$nested}]}"""
            val settings = JSONObject("{$schema}").put("lens_profiles",
                JSONObject().put("type", "string").put("value", raw)).toString()
            assertFailsWith<IllegalArgumentException> { store.read(archive(""""lens_profiles"""", settings)) }
        }
    }
}
