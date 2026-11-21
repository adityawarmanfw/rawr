package com.rawr.camera.storage

import android.content.Context
import android.net.Uri
import androidx.core.content.FileProvider
import androidx.test.core.app.ApplicationProvider
import java.io.File
import java.io.FileNotFoundException
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.shadows.ShadowContentResolver
import org.robolectric.annotation.Config

/**
 * Failure classification for still-capture recovery: only provably permanent
 * output loss may quarantine an entry. Everything else keeps today's
 * retry behavior.
 */
class StillOutputStoreRecoveryTest {

    @Test fun directMissingFileIsPermanent() {
        assertTrue(
            isOutputGonePermanently(
                FileNotFoundException("Missing file for primary:DCIM/Rawr/RAWR_1.dng")
            )
        )
    }

    @Test fun providerWrappedMissingFileIsPermanent() {
        // Observed MediaStore shape on device: IllegalArgumentException
        // ("... is child of ...") caused by FileNotFoundException
        // ("Missing file ...").
        val wrapped =
            IllegalArgumentException(
                "Failed to determine if primary:DCIM/Rawr/RAWR_1.dng is child of primary:DCIM/Rawr",
                FileNotFoundException("Missing file for primary:DCIM/Rawr/RAWR_1.dng")
            )
        assertTrue(isOutputGonePermanently(wrapped))
    }

    @Test fun deeplyWrappedMissingFileIsPermanent() {
        val root = FileNotFoundException("gone")
        val wrapped = RuntimeException("open failed", IllegalStateException("wrap", root))
        assertTrue(isOutputGonePermanently(wrapped))
    }

    @Test fun providerFlattenedMissingFileMessageIsPermanent() {
        // Some providers flatten the signal into the message text with no
        // typed cause to walk (observed via DatabaseUtils parcel decoding).
        val flattened =
            IllegalArgumentException(
                "Failed to determine if primary:DCIM/Rawr/RAWR_1.dng is child of " +
                    "primary:DCIM/Rawr: java.io.FileNotFoundException: Missing file " +
                    "for primary:DCIM/Rawr/RAWR_1.dng"
            )
        assertTrue(isOutputGonePermanently(flattened))
    }

    @Test fun transientIoFailureIsNotPermanent() {
        assertFalse(isOutputGonePermanently(java.io.IOException("I/O error")))
    }

    @Test fun securityFailureIsNotPermanent() {
        // A revoked grant can be re-granted; never quarantine on a guess.
        assertFalse(isOutputGonePermanently(SecurityException("Permission Denial")))
    }

    @Test fun unrelatedIllegalArgumentIsNotPermanent() {
        assertFalse(isOutputGonePermanently(IllegalArgumentException("bad mode")))
    }

    @Test fun nullFdWithoutCauseIsNotPermanent() {
        assertFalse(isOutputGonePermanently(IllegalStateException("Required value was null")))
    }
}

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class StillRecoveryOwnerTest {
    @Test fun recoveryDoesNotDeletePhotoRetiredByAnotherOwner() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val journal = CaptureJournal(File(context.filesDir, "still_jobs"))
        val target = CaptureJournal.Target("content://media/external/images/media/17", "photo.jpg", role = "jpeg")
        val entry = CaptureJournal.Entry("photo.dng", listOf(target))
        val store = StillOutputStore(context)
        ShadowContentResolver.registerProviderInternal("media", object : FileProvider() {
            override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int =
                throw AssertionError("Recovery deleted retired output: $uri")
        })
        journal.write(entry)
        assertTrue(StillOutputStore.claimForRenderer(context.filesDir, entry.name))
        try {
            assertNull(store.nextRecovery())
            journal.remove(entry) // The owner finished publishing and retired the job.
        } finally {
            StillOutputStore.rendererRelease(context.filesDir, entry.name)
        }
        assertNull(store.nextRecovery())
    }
}
