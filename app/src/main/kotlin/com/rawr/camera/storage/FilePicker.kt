package com.rawr.camera.storage

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContract

/**
 * Opens a single file for a one-shot import. Every caller copies the file
 * straight away, so no persistable grant is needed.
 *
 * ACTION_OPEN_DOCUMENT is only served by the system DocumentsUI; some vivo
 * builds disable or replace it, and launch() then throws. Those phones' own
 * file managers do serve ACTION_GET_CONTENT, so that is the fallback.
 */
class PickFile : ActivityResultContract<PickFile.Request, Uri?>() {
    class Request(val mimeTypes: Array<String>, val getContent: Boolean)

    override fun createIntent(context: Context, input: Request): Intent {
        val action = if (input.getContent) Intent.ACTION_GET_CONTENT else Intent.ACTION_OPEN_DOCUMENT
        return Intent(action)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType(if (input.mimeTypes.size == 1) input.mimeTypes[0] else "*/*")
            .putExtra(Intent.EXTRA_MIME_TYPES, input.mimeTypes)
    }

    override fun parseResult(resultCode: Int, intent: Intent?): Uri? =
        intent?.data?.takeIf { resultCode == Activity.RESULT_OK }
}

/** Launches the picker, falling back to GET_CONTENT; reports when neither exists. */
fun ActivityResultLauncher<PickFile.Request>.launchPicker(context: Context, mimeTypes: Array<String>) {
    for (getContent in listOf(false, true)) {
        try {
            launch(PickFile.Request(mimeTypes, getContent))
            if (getContent) Log.w(TAG, "OPEN_DOCUMENT unavailable; used GET_CONTENT")
            return
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "No activity for ${if (getContent) "GET_CONTENT" else "OPEN_DOCUMENT"}", e)
        }
    }
    Log.e(TAG, "FILE_PICKER_UNAVAILABLE mimeTypes=${mimeTypes.joinToString()}")
    Toast.makeText(context, "No file picker available. Enable the system Files app.", Toast.LENGTH_LONG).show()
}

private const val TAG = "RawrFilePicker"
