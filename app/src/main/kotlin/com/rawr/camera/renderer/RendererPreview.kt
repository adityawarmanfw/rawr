package com.rawr.camera.renderer

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp

@Composable
internal fun RendererPreviewCard(job: RenderJob, bitmap: Bitmap?, previewFailed: Boolean,
    tiles: Map<RendererTileKey, Bitmap>, zoom: Float, offset: Offset, onZoomChange: (Float,Offset)->Unit,
    onViewportChange: (IntSize)->Unit, onCanvasChange: (IntSize)->Unit) {
    val tilePaint = remember { androidx.compose.ui.graphics.Paint().apply { isAntiAlias = false; filterQuality = androidx.compose.ui.graphics.FilterQuality.Low } }
    val density=LocalDensity.current
    val latestZoom by rememberUpdatedState(zoom)
    val latestOffset by rememberUpdatedState(offset)
    val latestChange by rememberUpdatedState(onZoomChange)
    var canvas by remember {mutableStateOf(IntSize(1,1))}
    val rotated=job.orientation in 5..8
    val iw=if(rotated)job.height else job.width
    val ih=if(rotated)job.width else job.height
    val fit=minOf(canvas.width.toFloat()/iw.coerceAtLeast(1),canvas.height.toFloat()/ih.coerceAtLeast(1))
    val fitted=IntSize((iw*fit).toInt().coerceAtLeast(1),(ih*fit).toInt().coerceAtLeast(1))
    var previousFit by remember(job.id){mutableStateOf<IntSize?>(null)}
    LaunchedEffect(fitted){
        previousFit?.let { before ->
            if(before!=fitted)latestChange(latestZoom,Offset(latestOffset.x*fitted.width/before.width,latestOffset.y*fitted.height/before.height))
        }
        previousFit=fitted
        onViewportChange(fitted)
    }
    Box(Modifier.fillMaxSize().clipEditorViewport().testTag("renderer_preview").onSizeChanged{canvas=it;onCanvasChange(it)}
        .pointerInput(job.id,canvas){detectTransformGestures { centroid,pan,scale,_ ->
            val current=latestZoom;val next=(current*scale).coerceIn(.5f,32f)
            val ratio=next/current;val center=Offset(canvas.width/2f,canvas.height/2f)
            val translated=latestOffset*ratio+pan+(centroid-center)*(1-ratio)
            latestChange(next,Offset(translated.x.coerceIn(-canvas.width*next,canvas.width*next),translated.y.coerceIn(-canvas.height*next,canvas.height*next)))
        }}.pointerInput(job.id,fitted){detectTapGestures(onDoubleTap={
            latestChange(if(latestZoom>1.05f)1f else (1f/fit).coerceIn(1f,32f),Offset.Zero)
        })},contentAlignment=Alignment.Center) {
        val imageModifier=Modifier.size(with(density){fitted.width.toDp()},with(density){fitted.height.toDp()})
            .graphicsLayer{scaleX=zoom;scaleY=zoom;translationX=offset.x;translationY=offset.y}
        if(bitmap!=null)Image(bitmap.asImageBitmap(),"Preview",contentScale=ContentScale.FillBounds,modifier=imageModifier)
        if (tiles.isNotEmpty() && zoom > 1.05f) androidx.compose.foundation.Canvas(Modifier.fillMaxSize()) {
            val imageWidth = fitted.width * zoom
            val imageHeight = fitted.height * zoom
            val originX = (size.width - imageWidth) / 2 + offset.x
            val originY = (size.height - imageHeight) / 2 + offset.y
            for ((key, pixels) in tiles) {
                val region = tileDisplayRegion(key, job)
                val left = originX + region[0] * imageWidth
                val top = originY + region[1] * imageHeight
                val width = region[2] * imageWidth
                val height = region[3] * imageHeight
                // Exact shared float edges; integer-sized Image layouts can open seams on zoom.
                val canvas = drawContext.canvas
                canvas.save()
                canvas.clipRect(left, top, left + width, top + height)
                canvas.translate(left, top)
                canvas.scale(width / pixels.width, height / pixels.height)
                canvas.drawImage(pixels.asImageBitmap(), Offset.Zero, tilePaint)
                canvas.restore()
            }
        }
        if(bitmap==null) {
            if(previewFailed)Text("Preview unavailable",style=MaterialTheme.typography.bodySmall)
            else CircularProgressIndicator(Modifier.size(20.dp),strokeWidth=2.dp)
        }
    }
}
