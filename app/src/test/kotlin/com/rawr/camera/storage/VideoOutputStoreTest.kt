package com.rawr.camera.storage

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.provider.MediaStore
import com.rawr.camera.video.NativeVideoRecorder
import java.io.File
import kotlin.test.*
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowContentResolver

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class VideoOutputStoreTest {
    private class Provider : ContentProvider() {
        val updates = mutableListOf<Pair<Uri, ContentValues>>()
        var updatedRows = 1
        override fun onCreate() = true
        override fun getType(uri: Uri) = "video/mp4"
        override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor? = null
        override fun insert(uri: Uri, values: ContentValues?): Uri? = null
        override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?): Int {
            updates += uri to ContentValues(values)
            return updatedRows
        }
        override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 1
    }

    private val context get() = RuntimeEnvironment.getApplication()
    private fun provider(authority: String) = Provider().also {
        it.attachInfo(context, android.content.pm.ProviderInfo().apply { this.authority = authority })
        ShadowContentResolver.registerProviderInternal(authority, it)
    }

    @Test fun playableAndReconstructedJobsInBothRootsPublishWithoutReselectingGps() {
        val media = provider(MediaStore.AUTHORITY)
        val roots = listOf(context.filesDir, File(context.noBackupFilesDir, "video_jobs").apply { mkdirs() })
        val rebuilt = mutableSetOf<Uri>()
        var reconstructionCalls = 0
        for ((number, root) in roots.withIndex()) {
            for (playable in listOf(true, false)) {
                val stem = "rawr_native_video_${number}_$playable"
                val uri = Uri.parse("content://media/external_primary/video/media/${number}_$playable")
                File(root, "$stem.jsonl").writeText("{\"event\":\"start\",\"uri\":\"$uri\"}\n")
                File(root, "$stem.idx").writeText("synthetic index")
                if (playable) rebuilt += uri
            }
        }
        NativeVideoRecorder.recoverInterrupted(context, { it in rebuilt }) { uri, index, _ ->
            assertTrue(index.exists())
            reconstructionCalls++
            rebuilt += uri
        }
        assertEquals(2, reconstructionCalls)
        assertEquals(4, media.updates.size)
        for (root in roots) {
            assertTrue(root.listFiles().orEmpty().none { it.extension == "idx" })
            root.listFiles().orEmpty().filter { it.extension == "jsonl" }.forEach {
                assertTrue(it.readText().contains("recovered"))
                assertFalse(it.readText().contains("latitude"))
            }
        }
    }

    @Test fun corruptUnsupportedAndPublicationFailuresRetainSidecarsAndDoNotBlockOtherJobs() {
        val media = provider(MediaStore.AUTHORITY)
        val root = File(context.noBackupFilesDir, "video_jobs").apply { mkdirs() }
        val corrupt = File(root, "rawr_native_video_0_corrupt.jsonl").apply { writeText("not json\n") }
        val failed = File(root, "rawr_native_video_1_failed.jsonl").apply {
            writeText("{\"event\":\"start\",\"uri\":\"content://media/external_primary/video/media/failed\"}\n")
        }
        val index = File(failed.path.removeSuffix(".jsonl") + ".idx").apply { writeText("RMX9") }
        NativeVideoRecorder.recoverInterrupted(context, { false }) { _, _, _ -> error("unsupported sample index") }
        assertTrue(corrupt.exists())
        assertEquals("RMX9", index.readText())
        assertFalse(failed.readText().contains("recovered"))
        media.updatedRows = 0
        NativeVideoRecorder.recoverInterrupted(context, { true }) { _, _, _ -> error("must not reconstruct playable movie") }
        assertTrue(index.exists())
        assertFalse(failed.readText().contains("recovered"))
    }
}