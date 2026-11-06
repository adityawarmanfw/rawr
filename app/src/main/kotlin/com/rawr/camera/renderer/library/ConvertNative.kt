package com.rawr.camera.renderer.library

/** Native DNG probe, RAW thumbnail and DNG-to-DNG conversion (cpp/jni/ConvertJni.cpp). */
internal object ConvertNative {
    init { System.loadLibrary("rawrcam_native") }

    /** "ok\nwidth\nheight\nbits\ncompression\norientation\nrawr\nconvertible\nmake\nmodel" or "error\nmessage". */
    external fun probe(path: String): String

    /** [width, height, argb...] unrotated, or null. */
    external fun thumbnail(path: String, maxSide: Int): IntArray?

    external fun newJob(): Long
    external fun releaseJob(job: Long)
    /** Current file progress in permille. */
    external fun progress(job: Long): Int
    external fun cancel(job: Long)

    /**
     * Blocks; "" on success, "cancelled", or an error message. Mode: see [DngConversion.nativeMode].
     */
    external fun convert(job: Long, source: String, destination: String, mode: Int,
                         wallClockUnixMillis: Long, utcOffsetMinutes: Int): String
}
