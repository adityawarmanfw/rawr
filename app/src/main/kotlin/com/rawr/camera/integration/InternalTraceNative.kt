package com.rawr.camera.integration

object InternalTraceNative {
    init {
        System.loadLibrary("rawrcam_native")
    }

    external fun recordZslArtifactPublished(success: Boolean, bytes: Long)
}
