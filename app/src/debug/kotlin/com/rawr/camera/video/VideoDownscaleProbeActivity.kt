package com.rawr.camera.video

import android.app.Activity
import android.os.Bundle
import android.os.Process
import java.io.File

/**
 * Debug-only launcher for [VideoDownscaleProbe]. Never opens the camera; writes
 * files/downscale_probe.json plus the raw outputs, then kills its process.
 *
 * adb shell am start -n com.rawr.camera.debug/com.rawr.camera.video.VideoDownscaleProbeActivity
 */
class VideoDownscaleProbeActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Thread {
            val spvDir = File(filesDir, "spv_check")
            val report = when {
                intent.getBooleanExtra("compile_check", false) -> VideoDownscaleProbe.nativeCompileCheck(spvDir.absolutePath)
                intent.getBooleanExtra("demosaic_check", false) -> VideoDownscaleProbe.nativeDemosaicCheck()
                else -> VideoDownscaleProbe.nativeRun(filesDir.absolutePath)
            }
            File(filesDir, "downscale_probe.json").writeText(report)
            Process.killProcess(Process.myPid())
        }.start()
    }
}
