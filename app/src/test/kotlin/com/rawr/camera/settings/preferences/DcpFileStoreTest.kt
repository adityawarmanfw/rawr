package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.LensDcp
import java.io.File
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlin.test.assertNull

class DcpFileStoreTest {
    @get:Rule val temporary = TemporaryFolder()

    private fun store(inspect: (String) -> Array<String> = { arrayOf("", "Test profile", "") }) =
        DcpFileStore(temporary.root, inspect) { null }

    @Test fun equalImportsShareOneFileAndKeepTheProfileName() {
        val files = store()
        val bytes = byteArrayOf(1, 2, 3)
        val first = files.add(bytes)
        assertEquals(first, files.add(bytes))
        assertEquals("Test profile", first.name)
        assertTrue(DcpFileStore.isFileName(first.file))
        assertTrue(File(files.dir, first.file).readBytes().contentEquals(bytes))
        assertEquals(1, files.dir.listFiles()!!.size)
    }

    @Test fun rejectedOrFailedInspectionLeavesNoFiles() {
        assertFailsWith<IllegalArgumentException> {
            store { arrayOf("invalid matrices", "", "") }.add(byteArrayOf(1))
        }
        assertFailsWith<IllegalStateException> {
            store { error("inspection failed") }.add(byteArrayOf(2))
        }
        assertTrue(temporary.root.listFiles()!!.isEmpty())
    }

    @Test fun cleanupKeepsReferencesAndUnsavedImports() {
        val files = store()
        val used = files.add(byteArrayOf(1))
        val old = files.add(byteArrayOf(2))
        val fresh = files.add(byteArrayOf(3))
        File(files.dir, used.file).setLastModified(1)
        File(files.dir, old.file).setLastModified(1)
        files.sync(listOf(LensProfile("35", "3", dcp = used)))
        assertTrue(File(files.dir, used.file).exists())
        assertFalse(File(files.dir, old.file).exists())
        assertTrue(File(files.dir, fresh.file).exists())
    }

    @Test fun sizeLimitAppliesBeforeWritingOrInspecting() {
        val files = store { error("must not inspect oversized input") }
        assertFailsWith<IllegalArgumentException> { files.add(ByteArray(DcpFileStore.MAX_BYTES + 1)) }
        assertTrue(temporary.root.listFiles()!!.isEmpty())
    }

    @Test fun restoreChecksHashAndValidationAndKeepsLensesWithMissingProfiles() {
        val staging = temporary.newFolder("backup")
        val dir = temporary.newFolder("installed")
        val files = DcpFileStore(dir, { path ->
            arrayOf(if (File(path).readBytes()[0] == 3.toByte()) "unsupported" else "", "Profile", "")
        }) { null }
        val lenses = (1..4).map { value ->
            val bytes = byteArrayOf(value.toByte())
            val name = DcpFileStore.fileName(bytes)
            if (value != 4) File(staging, name).writeBytes(if (value == 2) byteArrayOf(0) else bytes)
            LensProfile("Lens $value", "$value", dcp = LensDcp(name, "Profile"))
        }
        val restored = requireNotNull(files.restore(staging, lenses))
        assertEquals(lenses[0], restored[0])
        for (index in 1..3) {
            assertEquals(lenses[index].name, restored[index].name)
            assertNull(restored[index].dcp)
        }
        assertEquals(listOf(lenses[0].dcp!!.file), dir.list()!!.toList())
    }
}
