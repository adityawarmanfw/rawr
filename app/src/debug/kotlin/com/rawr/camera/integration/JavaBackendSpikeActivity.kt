package com.rawr.camera.integration

import android.app.Activity
import android.os.Bundle
import android.os.Process
import android.util.Log
import java.io.File

/**
 * Debug-only launcher for [JavaBackendSpike]; writes files/java_backend_spike.txt, then kills its process.
 *
 * adb shell am start -n com.rawr.camera.debug/com.rawr.camera.integration.JavaBackendSpikeActivity \
 *   --es cam 4 --ei w 4096 --ei h 3072 --ei fmt 32 --ei cw 320 --ei ch 240 --el usecase 0 --ei opmode 0
 */
class JavaBackendSpikeActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val e = intent
        Thread {
            val lines = StringBuilder()
            val log: (String) -> Unit = { line -> lines.appendLine(line); Log.i("JavaBackendSpike", line) }
            val options = JavaBackendSpike.Options(
                cameraId = e.getStringExtra("cam") ?: "4",
                width = e.getIntExtra("w", 4096),
                height = e.getIntExtra("h", 3072),
                format = e.getIntExtra("fmt", 0x20),
                companionWidth = e.getIntExtra("cw", 0),
                companionHeight = e.getIntExtra("ch", 0),
                streamUseCase = e.getLongExtra("usecase", 0L),
                opMode = e.getIntExtra("opmode", 0),
                template = e.getIntExtra("template", 1),
                seconds = e.getIntExtra("seconds", 3),
                physicalId = e.getStringExtra("physical"),
            )
            log("options cam=${options.cameraId} ${options.width}x${options.height} fmt=${options.format} " +
                "companion=${options.companionWidth}x${options.companionHeight} usecase=${options.streamUseCase} opmode=${options.opMode}")
            log("RESULT " + JavaBackendSpike(applicationContext).run(options, log))
            log("END")
            File(filesDir, "java_backend_spike.txt").writeText(lines.toString())
            Process.killProcess(Process.myPid())
        }.start()
    }
}
