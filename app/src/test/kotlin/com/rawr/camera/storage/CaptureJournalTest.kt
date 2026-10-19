package com.rawr.camera.storage

import java.io.File
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Test

class CaptureJournalTest {
    private fun withJournal(test: (File, CaptureJournal) -> Unit) {
        val dir = Files.createTempDirectory("rawr-journal-test").toFile()
        try { test(dir, CaptureJournal(dir)) } finally { dir.deleteRecursively() }
    }

    @Test fun jpegOnlyMultiframeRetainsKindAndRecipeWithoutDng() = withJournal { _, journal ->
        val entry = CaptureJournal.Entry("capture-id", listOf(
            CaptureJournal.Target("content://media/jpeg", "capture_MF8.jpg", role = "jpeg")
        ), multiframe = true, capturedAt = 12345, recipe = "{\"version\":1,\"filmSimEnabled\":true}")
        journal.write(entry)
        assertEquals(entry, journal.readAll().single())
        assertFalse(entry.complete)
    }

    @Test fun fallbackDecisionAndSkippedJpegSurviveRestart() = withJournal { dir, journal ->
        val entry = CaptureJournal.Entry("capture-id", listOf(
            CaptureJournal.Target("content://media/raw", "capture_MF8.dng", role = "merged"),
            CaptureJournal.Target("content://media/jpeg", "capture_MF8.jpg", role = "jpeg", skipped = true)
        ), multiframe = true, recipe = "original film intent", fallback = true)
        journal.write(entry)
        val restored = CaptureJournal(dir).readAll().single()
        assertEquals(entry, restored)
        assertTrue(journal.hasFallback(restored))
        assertFalse(restored.complete)
        assertTrue(restored.copy(targets = restored.targets.map {
            if (it.role == "merged") it.copy(published = true) else it
        }).complete)
    }

    @Test fun nativeFallbackBeforePollingIsRecognizedAndCleanedUp() = withJournal { dir, journal ->
        val entry = CaptureJournal.Entry("capture-id", listOf(
            CaptureJournal.Target("content://media/raw", "capture.dng", role = "single", conditional = true),
            CaptureJournal.Target("content://media/jpeg", "capture.jpg", role = "jpeg")
        ))
        journal.write(entry)
        File(dir, "capture-id.job.fallback").writeText("film_memory\n")
        assertTrue(CaptureJournal(dir).hasFallback(entry))
        assertTrue(journal.readAll().single().targets.first().conditional)
        journal.remove(entry)
        assertFalse(File(dir, "capture-id.job.fallback").exists())
    }

    @Test fun publishedJpegCancelsConditionalRawWithoutReopeningPhoto() = withJournal { _, _ ->
        val entry = CaptureJournal.Entry("id", listOf(
            CaptureJournal.Target("raw", "raw.dng", role = "single", conditional = true),
            CaptureJournal.Target("jpeg", "photo.jpg", role = "jpeg", published = true)
        ))
        val resolved = entry.resolveForRecovery(memoryFallback = false)
        assertTrue(resolved.complete)
        assertTrue(resolved.targets.first().skipped)
        assertTrue(resolved.targets.last().published)
        assertFalse(resolved.targets.last().skipped)
        assertEquals(resolved, resolved.resolveForRecovery(memoryFallback = true))
    }

    @Test fun memoryFallbackPromotesOnlyConditionalMergedRaw() = withJournal { _, _ ->
        val entry = CaptureJournal.Entry("id", listOf(
            CaptureJournal.Target("raw", "raw_MF4.dng", role = "merged", conditional = true),
            CaptureJournal.Target("jpeg", "photo.jpg", role = "jpeg")
        ), multiframe = true, recipe = "original recipe")
        val resolved = entry.resolveForRecovery(memoryFallback = true)
        assertTrue(resolved.fallback)
        assertFalse(resolved.complete)
        assertFalse(resolved.targets.first().conditional)
        assertTrue(resolved.targets.last().skipped)
        assertEquals("original recipe", resolved.recipe)
        assertEquals(resolved, resolved.resolveForRecovery(memoryFallback = true))
        assertEquals(listOf("merged"), resolved.targets.filterNot { it.skipped }.map { it.role })
    }

    @Test fun legacyJournalInfersBaseAndMergedRoles() = withJournal { dir, journal ->
        File(dir, "old.dng.properties").writeText("""
            version=1
            name=old.dng
            count=3
            0.uri=content://media/base
            0.name=old.dng
            0.published=true
            1.uri=content://media/merged
            1.name=old_MF8.dng
            2.uri=content://media/jpeg
            2.name=old_MF8.jpg
        """.trimIndent())
        val restored = journal.readAll().single()
        assertTrue(restored.multiframe)
        assertEquals(listOf("base", "merged", "jpeg"), restored.targets.map { it.role })
        assertTrue(restored.targets.first().published)
        assertEquals("", restored.recipe)
    }

    @Test fun recoveryAttemptsSurviveRestartAndClearOnRemove() = withJournal { dir, journal ->
        val entry = CaptureJournal.Entry("burst", listOf(CaptureJournal.Target("content://test/1", "burst.dng")))
        journal.write(entry)
        assertEquals(0, journal.recoveryAttempts(entry))
        assertEquals(1, journal.recordRecoveryAttempt(entry))
        assertEquals(2, CaptureJournal(dir).recordRecoveryAttempt(entry))
        assertEquals(2, CaptureJournal(dir).recoveryAttempts(entry))
        journal.remove(entry)
        assertEquals(0, journal.recoveryAttempts(entry))
        assertTrue(dir.listFiles().orEmpty().none { it.name.startsWith("burst") })
    }

    @Test fun backlogBeyondEightSurvivesNewOwner() = withJournal { dir, journal ->
        repeat(40) { i ->
            val name = "RAWR_$i.dng"
            journal.write(CaptureJournal.Entry(name, listOf(CaptureJournal.Target("content://test/$i", name))))
            File(dir, "$name.job").writeBytes(byteArrayOf(1, 2, 3))
        }
        val restored = CaptureJournal(dir).readAll()
        assertEquals(40, restored.size)
        assertTrue(restored.all { journal.hasInput(it) && !it.complete })
        assertEquals(40, restored.flatMap { it.targets }.map { it.uri }.distinct().size)
    }

    @Test fun interruptedPublicationPreservesOriginalIdentitiesAndProgress() = withJournal { dir, journal ->
        val targets = listOf("base.dng", "merged.dng", "photo.jpg").mapIndexed { i, name ->
            CaptureJournal.Target("content://media/$i", name)
        }
        val entry = CaptureJournal.Entry("base.dng", targets)
        journal.write(entry)
        journal.write(entry.copy(targets = targets.mapIndexed { i, t -> t.copy(published = i == 0) }))
        val restored = CaptureJournal(dir).readAll().single()
        assertEquals(targets.map { it.uri }, restored.targets.map { it.uri })
        assertTrue(restored.targets[0].published)
        assertFalse(restored.complete)
    }

    @Test fun unfinishedAndUnsupportedJournalsAreNotRecoveredOrDeleted() = withJournal { dir, journal ->
        File(dir, "pending.properties.tmp").writeText("partial write")
        File(dir, "future.properties").writeText("version=99\nname=future\ncount=1\n")
        assertTrue(journal.readAll().isEmpty())
        assertTrue(File(dir, "future.properties").exists())
    }

    @Test fun completionCleanupCanBeRepeatedAfterRestart() = withJournal { dir, journal ->
        val entry = CaptureJournal.Entry("base.dng", listOf(CaptureJournal.Target("content://media/1", "base.dng", true)))
        journal.write(entry)
        File(dir, "base.dng.job").writeText("committed raw")
        File(dir, "base.dng.job.tmp").writeText("uncommitted raw")
        assertTrue(CaptureJournal(dir).readAll().single().complete)
        journal.remove(entry)
        CaptureJournal(dir).remove(entry)
        assertTrue(dir.listFiles().orEmpty().isEmpty())
    }

    @Test fun skippedEntriesStayListedButReportSkipped() = withJournal { dir, journal ->
        val entry = CaptureJournal.Entry("base.dng", listOf(CaptureJournal.Target("content://media/1", "base.dng")))
        journal.write(entry)
        File(dir, "base.dng.job").writeText("committed raw")
        assertFalse(journal.isSkipped(entry))
        journal.markSkipped(entry)
        assertTrue(journal.isSkipped(entry))
        // The marker is invisible to recovery enumeration and preserves entry + payload.
        assertEquals(listOf(entry), journal.readAll())
        assertTrue(journal.hasInput(entry))
        assertTrue(File(dir, "base.dng.job").exists())
    }

    @Test fun removeClearsSkipMarker() = withJournal { dir, journal ->
        val entry = CaptureJournal.Entry("base.dng", listOf(CaptureJournal.Target("content://media/1", "base.dng")))
        journal.write(entry)
        journal.markSkipped(entry)
        assertTrue(journal.isSkipped(entry))
        journal.remove(entry)
        assertFalse(journal.isSkipped(entry))
        assertTrue(journal.readAll().isEmpty())
    }

    @Test fun skipMarkerRejectsUnsafeNames() = withJournal { dir, journal ->
        val bad = CaptureJournal.Entry("../evil", listOf(CaptureJournal.Target("content://media/1", "x")))
        var thrown = false
        try {
            journal.markSkipped(bad)
        } catch (_: IllegalArgumentException) {
            thrown = true
        }
        assertTrue(thrown)
        assertTrue(dir.listFiles().orEmpty().none { it.name.contains("evil") })
    }
}
