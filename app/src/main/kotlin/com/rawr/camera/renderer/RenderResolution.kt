package com.rawr.camera.renderer

import kotlin.math.roundToInt
import kotlin.math.sqrt

internal object RenderResolution {
    private const val NATIVE_DEFAULT_MAX_PIXELS = 50_500_000L

    fun is200Mp(width: Int, height: Int): Boolean =
        width % 2 == 0 && height % 2 == 0 && width.toLong() * height in 180_000_000L..220_000_000L

    fun workingDimensions(width: Int, height: Int): Pair<Int, Int> =
        if (is200Mp(width, height)) width / 2 to height / 2 else width to height

    fun defaultMegapixels(width: Int, height: Int, rawr: Boolean): Double =
        if (is200Mp(width, height)) 50.0 else if (rawr || width.toLong() * height <= NATIVE_DEFAULT_MAX_PIXELS) 0.0 else 25.0
    fun choices(width: Int, height: Int, rawr: Boolean): List<Double> = buildList {
        val mp = width.toLong() * height / 1_000_000.0
        if (!is200Mp(width, height) && (rawr || width.toLong() * height <= NATIVE_DEFAULT_MAX_PIXELS)) add(0.0)
        listOf(50.0, 25.0, 12.5).filterTo(this) { it < mp * .99 }
    }
    fun dimensions(width: Int, height: Int, megapixels: Double): Pair<Int, Int> {
        require(width > 0 && height > 0 && megapixels.isFinite() && megapixels >= 0)
        if (megapixels == 0.0) return workingDimensions(width, height)
        // 200MP -> 50MP is a true 2x Bayer bin. Preserve its exact geometry
        // rather than scaling the demosaiced result to an arbitrary 50.0MP.
        if (megapixels == 50.0 && width % 2 == 0 && height % 2 == 0 &&
            width.toLong() * height in 180_000_000L..220_000_000L) return width / 2 to height / 2
        val scale = minOf(sqrt((megapixels * 1_000_000 / (width.toLong() * height)).coerceAtMost(1.0)), if (is200Mp(width,height)) .5 else 1.0)
        return ((width * scale / 2).roundToInt() * 2).coerceIn(minOf(2, width), width) to
            ((height * scale / 2).roundToInt() * 2).coerceIn(minOf(2, height), height)
    }
}
