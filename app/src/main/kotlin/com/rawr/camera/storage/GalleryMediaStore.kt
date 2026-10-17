package com.rawr.camera.storage

import androidx.core.content.edit
import android.content.Context
import android.net.Uri
import android.provider.MediaStore

/** Read-only MediaStore access for the capture-screen "last image" affordance. */
class GalleryMediaStore(private val context: Context) {
    private val prefs = context.getSharedPreferences("gallery_thumbnail", Context.MODE_PRIVATE)

    fun rememberPublishedJpeg(uri: Uri) {
        prefs.edit { putString("latest_jpeg_uri", uri.toString()) }
    }

    /**
     * Newest Rawr capture: the remembered JPEG, else the newest RAWR_ JPEG or
     * DNG (a DNG-only capture still gets a thumbnail). A JPEG published with
     * the same name as the newest DNG is preferred; it decodes faster.
     */
    fun findLatestRawrImage(): Uri? {
        val collection = MediaStore.Images.Media.EXTERNAL_CONTENT_URI
        val projection = arrayOf(MediaStore.Images.Media._ID, MediaStore.Images.Media.DISPLAY_NAME)
        val selection =
            "(${MediaStore.Images.Media.DISPLAY_NAME} LIKE ? OR ${MediaStore.Images.Media.DISPLAY_NAME} LIKE ?) AND " +
                "${MediaStore.Images.Media.IS_PENDING} = 0"
        val selectionArgs = arrayOf("RAWR_%.jpg", "RAWR_%.dng")
        val sortOrder = "${MediaStore.Images.Media.DATE_ADDED} DESC, ${MediaStore.Images.Media._ID} DESC"
        val newest = context.contentResolver.query(collection, projection, selection, selectionArgs, sortOrder)?.use { cursor ->
            val rows = buildList {
                while (size < 4 && cursor.moveToNext()) add(cursor.getLong(0) to cursor.getString(1).orEmpty())
            }
            val first = rows.firstOrNull() ?: return@use null
            val base = first.second.substringBeforeLast('.')
            (rows.firstOrNull { it.second.endsWith(".jpg", true) && it.second.substringBeforeLast('.') == base } ?: first)
        } ?: return rememberedJpeg()
        return Uri.withAppendedPath(collection, newest.first.toString())
    }

    private fun rememberedJpeg(): Uri? =
        prefs.getString("latest_jpeg_uri", null)?.let(Uri::parse)?.takeIf { remembered ->
            runCatching { context.contentResolver.openFileDescriptor(remembered, "r")?.use { true } ?: false }.getOrDefault(false)
        }

    /** True for DNG content (loadThumbnail cannot decode most RAWs). */
    fun isDng(uri: Uri): Boolean =
        context.contentResolver.getType(uri)?.let { it == "image/x-adobe-dng" || it == "image/dng" || it == "image/x-dng" } == true
}
