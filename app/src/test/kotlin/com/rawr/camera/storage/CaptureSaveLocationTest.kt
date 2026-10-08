package com.rawr.camera.storage

import com.rawr.camera.storage.CaptureSaveLocation.Companion.IMAGE_ROOTS
import com.rawr.camera.storage.CaptureSaveLocation.Companion.VIDEO_ROOTS
import com.rawr.camera.storage.CaptureSaveLocation.Companion.mediaStorePlacement
import org.junit.Assert.*
import org.junit.Test

class CaptureSaveLocationTest {
    private val authority = "com.android.externalstorage.documents"

    @Test fun primaryMediaSubtreeMapsToMediaStorePath() {
        assertEquals("external_primary" to "DCIM/Rawr", mediaStorePlacement(authority, "primary:DCIM/Rawr", IMAGE_ROOTS))
        assertEquals("external_primary" to "Pictures/a/b", mediaStorePlacement(authority, "primary:Pictures/a/b/", IMAGE_ROOTS))
        assertEquals("external_primary" to "DCIM", mediaStorePlacement(authority, "primary:DCIM", IMAGE_ROOTS))
    }

    @Test fun removableVolumeUsesLowercaseVolumeName() {
        assertEquals("1a2b-3c4d" to "DCIM/Rawr", mediaStorePlacement(authority, "1A2B-3C4D:DCIM/Rawr", IMAGE_ROOTS))
    }

    @Test fun foldersMediaStoreRejectsStayOnDocumentTree() {
        assertNull(mediaStorePlacement(authority, "primary:", IMAGE_ROOTS))
        assertNull(mediaStorePlacement(authority, "primary:Download/Rawr", IMAGE_ROOTS))
        assertNull(mediaStorePlacement(authority, "primary:Movies/Rawr", IMAGE_ROOTS))
        assertNull(mediaStorePlacement("com.google.android.apps.docs.storage", "primary:DCIM/Rawr", IMAGE_ROOTS))
        assertNull(mediaStorePlacement(null, "primary:DCIM/Rawr", IMAGE_ROOTS))
    }

    @Test fun videosAlsoAcceptMovies() {
        assertEquals("external_primary" to "Movies/Rawr", mediaStorePlacement(authority, "primary:Movies/Rawr", VIDEO_ROOTS))
    }
}
