package com.rawr.camera.video

/**
 * 1080p downscale probe (debug tooling): runs the recording RAW stage on a
 * synthetic Bayer zone plate with the box and anti-aliasing filters and writes
 * downscale_<filter>.f16 (1920x1080 RGBA16F) to [outDir]. Returns a JSON report.
 */
internal object VideoDownscaleProbe {
    init { System.loadLibrary("rawrcam_native") }

    external fun nativeRun(outDir: String): String
}
