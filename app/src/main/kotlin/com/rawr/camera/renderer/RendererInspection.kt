package com.rawr.camera.renderer

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.dp
import com.rawr.camera.ui.CaptureDimens
import androidx.compose.foundation.border
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.sp
import com.rawr.camera.ui.CaptureColors
import org.json.JSONObject
import java.util.Locale

/** Native image_scopes output: same measurement, density and grid as the viewfinder. */
@Composable
internal fun RendererWaveform(bitmap: Bitmap?, modifier: Modifier = Modifier) {
    val shape = RoundedCornerShape(9.dp)
    Box(modifier.size(CaptureDimens.ScopeCompactWidth, CaptureDimens.ScopeCompactHeight)
        .clip(shape).background(CaptureColors.Panel.copy(alpha = .94f))
        .border(1.dp, Color.White.copy(alpha = .10f), shape)) {
        if (bitmap != null) Image(bitmap.asImageBitmap(), "RGB waveform", Modifier.fillMaxSize(), contentScale = ContentScale.Fit)
        Text("WAVEFORM · RGB", style = EditorHeadingStyle.copy(fontSize = 8.sp, letterSpacing = 1.sp),
            color = Color.White.copy(alpha = .62f), modifier = Modifier.padding(7.dp))
        if (bitmap == null) Text("WAITING…", style = EditorHeadingStyle.copy(fontSize = 8.sp),
            color = Color.Gray, modifier = Modifier.align(Alignment.Center))
    }
}

@Composable
internal fun RendererMetadata(job: RenderJob, session: RendererPreviewSession, onDismiss: () -> Unit) {
    var metadata by remember(job.id) { mutableStateOf<JSONObject?>(null) }
    var error by remember(job.id) { mutableStateOf<String?>(null) }
    LaunchedEffect(job.id) {
        try { metadata = JSONObject(session.metadata()) }
        catch (cancel: kotlinx.coroutines.CancellationException) { throw cancel }
        catch (failure: Exception) { error = failure.message ?: "Could not read source metadata" }
    }
    EditorDialog("Source metadata", onDismiss) {
        Column(Modifier.fillMaxWidth().heightIn(max = 380.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(14.dp)) {
            @Composable fun field(label: String, value: String) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(label.uppercase(Locale.ROOT), style = EditorHeadingStyle, color = Color.Gray)
                    Text(value, style = EditorLabelStyle, color = Color.White)
                }
            }
            field("File", job.name)
            field("Source dimensions", "${job.width} × ${job.height}")
            field("Source", if (job.rawr) "Rawr DNG" else "Imported DNG")
            field("EXIF orientation", job.orientation.toString())
            val working = RenderResolution.dimensions(job.width, job.height, if (RenderResolution.is200Mp(job.width, job.height)) 50.0 else 0.0)
            field("Working resolution", "${working.first} × ${working.second}")
            metadata?.let { info ->
                field("ISO", info.optInt("iso").takeIf { it > 0 }?.toString() ?: "Not recorded")
                val seconds = info.optDouble("exposureNs", 0.0) / 1e9
                field("Shutter", when { seconds <= 0 -> "Not recorded"; seconds < 1 -> "1/${(1 / seconds).toInt()} s"; else -> String.format(Locale.US, "%.2f s", seconds) })
                field("Aperture", info.optDouble("aperture", 0.0).takeIf { it > 0 }?.let { String.format(Locale.US, "f/%.1f", it) } ?: "Not recorded")
                field("White level", info.optDouble("whiteLevel").toString())
                field("Black levels", info.optJSONArray("blackLevels")?.toString() ?: "Not recorded")
            } ?: Text(error ?: "Reading source metadata…", style = EditorLabelStyle, color = Color.Gray)
        }
    }
}
