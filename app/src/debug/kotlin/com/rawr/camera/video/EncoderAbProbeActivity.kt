package com.rawr.camera.video

import android.app.Activity
import android.os.Bundle
import android.os.Process
import android.view.WindowManager
import java.io.File

/**
 * Debug-only launcher for [EncoderAbProbe]. Never opens the camera. Writes
 * files/encoder_ab_<mode>.json, then kills its own process so nothing keeps
 * running (and heating the phone) after the test.
 *
 * adb shell am start -n com.rawr.camera.debug/com.rawr.camera.video.EncoderAbProbeActivity \
 *   --es mode p010|rgb10a2 --ei paced 90 --ei burst 15 [--ei dumpFrames 45]
 */
class EncoderAbProbeActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val p010 = intent.getStringExtra("mode") == "p010"
        val paced = intent.getIntExtra("paced", 90).coerceIn(5, 300)
        val burst = intent.getIntExtra("burst", 15).coerceIn(0, 60)
        val width = intent.getIntExtra("width", 4080)
        val height = intent.getIntExtra("height", 3072)
        Thread {
            val name = "encoder_ab_${if (p010) "p010" else "rgb10a2"}"
            val dumpFrames = intent.getIntExtra("dumpFrames", 0)
            val report = EncoderAbProbe.run(applicationContext, p010, width, height, paced, burst,
                File(filesDir, "$name.hevc"), dumpFrames,
                intent.getLongExtra("usage", EncoderAbProbe.DEFAULT_P010_USAGE))
            File(filesDir, "$name.json").writeText(report.toString(2))
            android.util.Log.i("RawrEncoderAb", "Wrote encoder_ab report success=${report.optBoolean("success")}")
            Process.killProcess(Process.myPid())
        }.start()
    }
}
