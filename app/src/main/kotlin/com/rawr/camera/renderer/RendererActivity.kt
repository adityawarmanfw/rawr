package com.rawr.camera.renderer

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import com.rawr.camera.ui.CaptureTheme

class RendererActivity : ComponentActivity() {
    private val viewModel: RendererViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        if (savedInstanceState == null) extractIncomingDngUri(intent, contentResolver::getType)?.let(viewModel::import)
        setContent { CaptureTheme { RendererRoute(viewModel, onExit = ::finish) } }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        extractIncomingDngUri(intent, contentResolver::getType)?.let(viewModel::import)
    }

    override fun onStart() { super.onStart(); viewModel.onVisible() }
    override fun onStop() { viewModel.onHidden(); super.onStop() }
}

/** Single incoming DNG from Open, Edit, or Share. The importer copies the granted
 * URI into app storage, so later editing does not depend on the sender's grant. */
internal fun extractIncomingDngUri(intent: Intent?, resolveType: (Uri) -> String? = { null }): Uri? {
    val uri =
        when (intent?.action) {
            Intent.ACTION_SEND -> intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
            Intent.ACTION_VIEW, Intent.ACTION_EDIT -> intent.data
            else -> return null
        } ?: return null
    if (uri.scheme != "content" && uri.scheme != "file") return null
    val mime = intent.type ?: runCatching { resolveType(uri) }.getOrNull()
    when (mime?.substringBefore(';')?.trim()?.lowercase(java.util.Locale.ROOT)) {
        "image/x-adobe-dng", "image/dng", "image/x-dng" -> Unit
        else -> return null
    }
    return uri
}
