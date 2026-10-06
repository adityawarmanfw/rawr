package com.rawr.camera.video

/**
 * 1080p downscale probe (debug tooling): runs the recording RAW stage on a
 * synthetic Bayer zone plate with the box and anti-aliasing filters and writes
 * downscale_<filter>.f16 (1920x1080 RGBA16F) to [outDir]. Returns a JSON report.
 */
internal object VideoDownscaleProbe {
    init { System.loadLibrary("rawrcam_native") }

    external fun nativeRun(outDir: String): String

    /** Tries every *.spv in [dir] as the strip pass; JSON of VkResult per file. */
    external fun nativeCompileCheck(dir: String): String

    /** 4K and Open Gate RAW stage on the zone plate: timing plus output/clip hashes. */
    external fun nativeDemosaicCheck(): String
}
