package com.rawr.camera.renderer

import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import androidx.test.core.app.ApplicationProvider
import android.content.Context
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class RendererIncomingDngTest {
    private val uri = Uri.parse("content://photos/document/123")

    @Test fun openEditAndShareAcceptDngMimeAliases() {
        for (mime in listOf("image/x-adobe-dng", "image/dng", "image/x-dng")) {
            for (action in listOf(Intent.ACTION_VIEW, Intent.ACTION_EDIT)) {
                assertEquals(uri, extractIncomingDngUri(Intent(action).setDataAndType(uri, mime)))
            }
            assertEquals(uri, extractIncomingDngUri(Intent(Intent.ACTION_SEND).setType(mime)
                .putExtra(Intent.EXTRA_STREAM, uri)))
        }
    }

    @Test fun openResolvesProviderMimeWhenSenderOmitsIt() {
        assertEquals(uri, extractIncomingDngUri(Intent(Intent.ACTION_VIEW, uri)) { "image/x-adobe-dng" })
        assertNull(extractIncomingDngUri(Intent(Intent.ACTION_VIEW, uri)) { throw SecurityException() })
        assertNull(extractIncomingDngUri(Intent(Intent.ACTION_VIEW).setDataAndType(uri, "image/jpeg")))
        assertNull(extractIncomingDngUri(Intent(Intent.ACTION_VIEW)
            .setDataAndType(Uri.parse("https://example.com/photo.dng"), "image/x-adobe-dng")))
    }

    @Test fun manifestOffersRendererForOpeningDngButNotJpeg() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        fun handlers(mime: String) = context.packageManager.queryIntentActivities(
            Intent(Intent.ACTION_VIEW).setDataAndType(uri, mime), PackageManager.MATCH_DEFAULT_ONLY)
            .map { it.activityInfo.name }
        for (mime in listOf("image/x-adobe-dng", "image/dng", "image/x-dng")) {
            assertTrue(RendererActivity::class.java.name in handlers(mime))
        }
        assertFalse(RendererActivity::class.java.name in handlers("image/jpeg"))
    }
}
