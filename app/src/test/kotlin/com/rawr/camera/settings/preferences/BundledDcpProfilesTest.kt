package com.rawr.camera.settings.preferences

import android.content.Context
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.test.core.app.ApplicationProvider
import com.rawr.camera.model.toViewfinderQuickState
import com.rawr.camera.settings.model.*
import java.io.File
import kotlin.test.*
import org.junit.Rule
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class BundledDcpProfilesTest {
    @get:Rule val temporary = TemporaryFolder()
    private val app = ApplicationProvider.getApplicationContext<Context>()
    private val settings = SettingsCatalog.initialState().values
    private val base = LensProfileCodec.decode(File("src/main/cpp/camera/profiles/vivo_x300_ultra.json").readText())!!
    private val defaults = BundledDcpProfiles.defaults(BundledDcpProfiles.DEVICE, base)

    @Test fun cameraMappingCoversModesAndCropsButOnlyOnX300Ultra() {
        assertEquals(base, BundledDcpProfiles.defaults("vivo_x300_pro", base))
        assertEquals(15, BundledDcpProfiles.entries.size)
        assertEquals(15, BundledDcpProfiles.entries.map { it.profile.file }.distinct().size)
        defaults.forEachIndexed { index, lens ->
            val expected = BundledDcpProfiles.entries.filter { it.cameraId == lens.camera.id }.map { it.profile }
            assertEquals(expected, lens.image.availableDcps, lens.name)
            assertEquals(base[index], lens.image { copy(dcpProfiles = emptyList()) })
            assertNull(lens.image.dcp)
        }
        val quick = settings.toViewfinderQuickState(defaults)
        assertEquals(defaults.map { it.image.availableDcps }, quick.dcps.map { it.profiles })
    }

    @Test fun installerValidatesOriginalHashesAndRepairsMissingOrDamagedCopies() {
        var inspected = 0
        val store = DcpFileStore(temporary.root, { inspected++; arrayOf("", "Name", "") }) { null }
        fun bytes(asset: String) = app.assets.open("${BundledDcpProfiles.ASSET_DIR}/$asset").use { it.readBytes() }
        BundledDcpProfiles.install(store, defaults, ::bytes)
        assertEquals(15, inspected)
        BundledDcpProfiles.entries.forEach { entry ->
            assertEquals(entry.profile.file, DcpFileStore.fileName(bytes(entry.asset)))
            assertContentEquals(bytes(entry.asset), File(store.dir, entry.profile.file).readBytes())
        }
        BundledDcpProfiles.install(store, defaults) { error("Intact profiles should not be extracted again") }
        val first = BundledDcpProfiles.entries.first().profile.file
        File(store.dir, first).writeText("corrupted")
        File(store.dir, BundledDcpProfiles.entries.last().profile.file).delete()
        BundledDcpProfiles.install(store, defaults, ::bytes)
        assertEquals(17, inspected)
        store.dir.listFiles()!!.forEach { it.setLastModified(1) }
        store.sync(defaults)
        assertEquals(15, store.dir.listFiles()!!.size)
    }

    @Test fun upgradePreservesCustomRoutesImportsAndSelectionsAndDoesNotReaddRemovals() {
        val repository = SettingsPreferencesRepository(app, settings)
        val custom = LensDcp("f".repeat(32) + ".dcp", "Personal")
        val lens = base.first { it.camera.id == "3" }.copy(name = "Mine", enabled = false)
            .image { copy(dcp = custom, mirror = LensMirror.Horizontal) }
        val physical = LensProfile("Other", "3", "physical")
        val before = settings.copy(lensProfiles = listOf(lens, physical), locationTagging = true)
        val prefs = repository.encodeForBackup(before).toMutablePreferences()
        repository.seedBundledDcps(prefs, defaults)
        val after = repository.decodeBackup(prefs)!!
        val upgraded = after.lensProfiles!!.first()
        assertEquals(lens, upgraded.image { copy(dcpProfiles = emptyList()) })
        assertEquals(6, upgraded.image.availableDcps.size)
        assertEquals(physical, after.lensProfiles!![1])
        assertEquals(before.copy(lensProfiles = after.lensProfiles), after)
        assertEquals(custom, upgraded.image.dcp)
        val removed = upgraded.image { removeDcp(BundledDcpProfiles.entries.first { it.cameraId == "3" }.profile.file) }
        val key = stringPreferencesKey("lens_profiles")
        prefs[key] = LensProfileCodec.encode(listOf(removed, physical))
        repository.seedBundledDcps(prefs, defaults)
        assertEquals(listOf(removed, physical), repository.decodeBackup(prefs)!!.lensProfiles)
        assertEquals(1, prefs[intPreferencesKey("bundled_dcp_version")])
    }

    @Test fun untouchedDefaultsStayImplicitAndUnrelatedDevicesDoNotSeed() {
        val repository = SettingsPreferencesRepository(app, settings)
        val prefs = repository.encodeForBackup(settings).toMutablePreferences()
        repository.seedBundledDcps(prefs, base)
        assertNull(prefs[intPreferencesKey("bundled_dcp_version")])
        repository.seedBundledDcps(prefs, defaults)
        assertNull(repository.decodeBackup(prefs)!!.lensProfiles)
        assertEquals(1, prefs[intPreferencesKey("bundled_dcp_version")])
    }
    @Test fun backupOfImplicitDeviceDefaultsIncludesAllFifteenOriginalFiles() {
        val dir = File(app.filesDir, DcpFileStore.DIR)
        val files = DcpFileStore(dir, { arrayOf("", "Profile", "") }) { null }
        BundledDcpProfiles.install(files, defaults) { asset ->
            app.assets.open("${BundledDcpProfiles.ASSET_DIR}/$asset").use { it.readBytes() }
        }
        val archive = temporary.newFile("backup.zip")
        val backup = SettingsBackupStore(app, settings) { defaults }
        archive.outputStream().use { backup.export(settings, setOf(BackupSection.LensProfiles), it) }
        val contents = backup.read(android.net.Uri.fromFile(archive))
        assertEquals(defaults, contents.values.lensProfiles)
        assertEquals(15, contents.dcpFiles.listFiles()!!.size)
        BundledDcpProfiles.entries.forEach { entry ->
            assertContentEquals(File(dir, entry.profile.file).readBytes(), File(contents.dcpFiles, entry.profile.file).readBytes())
        }
    }

    @Test fun knownMaZeFilesUseAuthorSuffixForImportsAndOlderBackupLabels() {
        val entry = BundledDcpProfiles.entries.first()
        val store = DcpFileStore(temporary.root, { arrayOf("", "MaZe · 5600 Sun", "") }) { null }
        val bytes = app.assets.open("${BundledDcpProfiles.ASSET_DIR}/${entry.asset}").use { it.readBytes() }
        val imported = store.add(bytes)
        assertEquals(entry.profile, imported)
        assertTrue(imported.name.endsWith(" · MaZe"))
        val old = entry.profile.copy(name = "MaZe · 5600 Sun")
        val lens = LensProfile("14", "4").image { copy(dcp = old, dcpProfiles = listOf(old)) }
        val decoded = LensProfileCodec.decodeBackup(LensProfileCodec.encode(listOf(lens))).single()
        assertEquals(entry.profile, decoded.image.dcp)
        assertEquals(listOf(entry.profile), decoded.image.availableDcps)
    }

}
