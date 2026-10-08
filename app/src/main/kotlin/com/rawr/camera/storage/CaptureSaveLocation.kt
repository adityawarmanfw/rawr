package com.rawr.camera.storage

import android.net.Uri
import android.provider.DocumentsContract
import android.provider.MediaStore

/**
 * Shared destination selection for photo and video captures.
 *
 * A picked folder that MediaStore can address (a DCIM/Pictures/Movies subtree
 * on a local volume) resolves to a [relativePath] on [volumeName], so captures
 * are inserted as our own pending rows: MediaStore records rawr as the owner and
 * indexes size/orientation when the finished file is published. Writing through
 * the document provider instead makes `com.android.externalstorage` the owner and
 * indexes the file while it is still empty (galleries show it rotated or late).
 * [treeUri] stays set as the fallback when the volume is not mounted.
 */
internal data class CaptureSaveLocation(
    val relativePath: String,
    val treeUri: String? = null,
    val volumeName: String = MediaStore.VOLUME_EXTERNAL_PRIMARY
) {
    companion object {
        private const val EXTERNAL_STORAGE_AUTHORITY = "com.android.externalstorage.documents"
        val IMAGE_ROOTS = setOf("DCIM", "Pictures")
        val VIDEO_ROOTS = setOf("DCIM", "Movies", "Pictures")

        fun fromId(id: String, mediaRoots: Set<String> = IMAGE_ROOTS): CaptureSaveLocation = when {
            id.startsWith("storage.tree:") -> {
                val tree = id.removePrefix("storage.tree:")
                val placement = runCatching {
                    val uri = Uri.parse(tree)
                    mediaStorePlacement(uri.authority, DocumentsContract.getTreeDocumentId(uri), mediaRoots)
                }.getOrNull()
                if (placement != null) CaptureSaveLocation(placement.second, tree, placement.first)
                else CaptureSaveLocation(relativePath = "", treeUri = tree)
            }
            id == "storage.pictures_raw" -> CaptureSaveLocation("Pictures/RAW Camera")
            else -> CaptureSaveLocation("DCIM/Camera")
        }

        /** (volume, relative path) for an ExternalStorageProvider tree MediaStore accepts, else null. */
        fun mediaStorePlacement(authority: String?, documentId: String, mediaRoots: Set<String>): Pair<String, String>? {
            if (authority != EXTERNAL_STORAGE_AUTHORITY) return null
            val root = documentId.substringBefore(':', "")
            val path = documentId.substringAfter(':', "").trim('/')
            if (root.isEmpty() || path.isEmpty()) return null
            if (mediaRoots.none { it.equals(path.substringBefore('/'), ignoreCase = true) }) return null
            val volume = if (root == "primary") MediaStore.VOLUME_EXTERNAL_PRIMARY else root.lowercase()
            return volume to path
        }
    }

    fun imagesCollection(): Uri = MediaStore.Images.Media.getContentUri(volumeName)
    fun videoCollection(): Uri = MediaStore.Video.Media.getContentUri(volumeName)
}
