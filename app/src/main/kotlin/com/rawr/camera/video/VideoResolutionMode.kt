package com.rawr.camera.video

import org.json.JSONObject

data class VideoFrameSize(val width: Int, val height: Int)

enum class VideoResolutionMode(val label: String) {
    HD1080("1080p"), UHD4K("4K"), OPEN_GATE("Open Gate");

    fun next(): VideoResolutionMode = entries[(ordinal + 1) % entries.size]

    fun resolve(cameraSnapshot: JSONObject): VideoFrameSize {
        val rawWidth = cameraSnapshot.optInt("rawWidth")
        val rawHeight = cameraSnapshot.optInt("rawHeight")
        require(rawWidth > 0 && rawHeight > 0) { "Active RAW camera geometry is unavailable" }
        // Image area without padded raw edges (lens profile cropEdges): Open Gate records just that.
        val keptWidth = cameraSnapshot.optInt("keptWidth").takeIf { it in 1..rawWidth } ?: rawWidth
        val keptHeight = cameraSnapshot.optInt("keptHeight").takeIf { it in 1..rawHeight } ?: rawHeight
        val size = when (this) {
            HD1080 -> VideoFrameSize(1920, 1080)
            UHD4K -> VideoFrameSize(3840, 2160)
            OPEN_GATE -> VideoFrameSize(keptWidth and -2, keptHeight and -2)
        }
        require(size.width > 0 && size.height > 0 &&
                size.width <= rawWidth && size.height <= rawHeight) {
            "$label needs at least ${size.width}×${size.height} RAW; active camera is ${rawWidth}×${rawHeight}"
        }
        return size
    }

    companion object {
        fun fromName(value: String?): VideoResolutionMode = when (value?.lowercase()) {
            "4k", "uhd", "2160p" -> UHD4K
            "open_gate", "open-gate", "opengate", "open gate" -> OPEN_GATE
            else -> HD1080
        }
    }
}
