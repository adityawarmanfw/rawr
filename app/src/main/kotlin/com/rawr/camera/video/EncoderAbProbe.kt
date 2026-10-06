package com.rawr.camera.video

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.ImageFormat
import android.hardware.HardwareBuffer
import android.media.ImageWriter
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.BatteryManager
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.view.Surface
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.max

/**
 * Encoder input A/B (debug tooling): identical synthetic frames reach the HEVC
 * encoder either through the shipped A2B10G10R10 swapchain or as P010 buffers
 * rendered with VK_ANDROID_external_format_resolve. No camera, no RAW work.
 * A paced 30 fps phase measures drops, latency and power; an unpaced burst
 * measures the encoder's throughput ceiling.
 */
internal object EncoderAbProbe {
    init { System.loadLibrary("rawrcam_native") }

    private external fun nativeCreate(surface: Surface?, width: Int, height: Int, p010: Boolean): Long
    private external fun nativeRenderSwapchain(handle: Long, frame: Int, ptsNs: Long): Long
    private external fun nativeRenderBuffer(handle: Long, buffer: HardwareBuffer, frame: Int): Long
    private external fun nativeInfo(handle: Long): String
    private external fun nativeLastError(): String
    private external fun nativeDestroy(handle: Long)

    private const val PERIOD_NS = 1_000_000_000L / 30
    const val DEFAULT_P010_USAGE = HardwareBuffer.USAGE_GPU_COLOR_OUTPUT or HardwareBuffer.USAGE_VIDEO_ENCODE or
        HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE

    fun run(context: Context, p010: Boolean, width: Int, height: Int, pacedSeconds: Int, burstSeconds: Int,
            dump: java.io.File? = null, dumpFrames: Int = 0, p010Usage: Long = DEFAULT_P010_USAGE): JSONObject {
        val report = JSONObject().put("mode", if (p010) "p010" else "rgb10a2")
            .put("width", width).put("height", height)
        val power = context.getSystemService(PowerManager::class.java)
        val battery = context.getSystemService(BatteryManager::class.java)
        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_HEVC)
        report.put("codec", codec.name)
        // Same configuration as NativeAvRecorder for an Open Gate 10-bit recording.
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_HEVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10)
            setInteger(MediaFormat.KEY_BIT_RATE, 60_000_000)
            setInteger(MediaFormat.KEY_FRAME_RATE, 30)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
            setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
            setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
        }
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val surface = codec.createInputSurface()
        var writer: ImageWriter? = null
        var releaseThread: HandlerThread? = null
        var handle = 0L
        val running = AtomicBoolean(true)
        val encoded = AtomicLong()
        val latencySumUs = AtomicLong()
        val latencyMaxUs = AtomicLong()
        var drainError: Throwable? = null
        val drain = Thread {
            val info = MediaCodec.BufferInfo()
            // Optional elementary-stream dump of the first frames, to check the content decodes.
            val out = dump?.takeIf { dumpFrames > 0 }?.outputStream()
            var dumped = 0
            try {
                while (running.get()) {
                    val index = codec.dequeueOutputBuffer(info, 10_000)
                    if (index < 0) continue
                    if (out != null && dumped <= dumpFrames && info.size > 0) {
                        val bytes = ByteArray(info.size)
                        codec.getOutputBuffer(index)!!.apply { position(info.offset) }.get(bytes)
                        out.write(bytes)
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) dumped++
                        if (dumped > dumpFrames) out.close()
                    }
                    if (info.size > 0 && info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
                        val latency = System.nanoTime() / 1000 - info.presentationTimeUs
                        encoded.incrementAndGet()
                        latencySumUs.addAndGet(latency)
                        latencyMaxUs.accumulateAndGet(latency, ::max)
                    }
                    codec.releaseOutputBuffer(index, false)
                }
            } catch (t: Throwable) {
                drainError = t
            } finally {
                runCatching { out?.close() }
            }
        }
        try {
            if (p010) {
                releaseThread = HandlerThread("EncoderAbRelease").apply { start() }
                writer = ImageWriter.Builder(surface)
                    .setImageFormat(ImageFormat.YCBCR_P010)
                    .setMaxImages(11)
                    .setUsage(p010Usage)
                    .build()
                report.put("requestedUsage", p010Usage)
                handle = nativeCreate(null, width, height, true)
            } else {
                handle = nativeCreate(surface, width, height, false)
            }
            check(handle != 0L) { "probe create failed: ${nativeLastError()}" }
            codec.start()
            drain.start()

            // ImageWriter has no timed dequeue. Track buffers held by the
            // encoder so a full queue counts as a drop, like the swapchain's
            // 3 ms acquire in the app.
            val inFlight = AtomicInteger()
            writer?.setOnImageReleasedListener({ inFlight.decrementAndGet() }, Handler(releaseThread!!.looper))
            val maxImages = writer?.maxImages ?: 0
            var frame = 0
            fun produce(ptsNs: Long): Long {
                return if (writer == null) {
                    nativeRenderSwapchain(handle, frame, ptsNs)
                } else {
                    if (inFlight.get() >= maxImages) return 0
                    val image = writer.dequeueInputImage()
                    val buffer = image.hardwareBuffer ?: error("ImageWriter image has no HardwareBuffer")
                    if (frame == 0) report.put("bufferUsage", buffer.usage).put("bufferFormat", buffer.format)
                    val gpuUs = nativeRenderBuffer(handle, buffer, frame)
                    buffer.close()
                    if (gpuUs < 0) { image.close(); return gpuUs }
                    image.timestamp = ptsNs
                    inFlight.incrementAndGet()
                    writer.queueInputImage(image)
                    gpuUs
                }
            }

            val samples = JSONArray()
            var drops = 0L
            var gpuSumUs = 0L
            var gpuMaxUs = 0L
            var rendered = 0L
            var powerSum = 0.0
            var powerSamples = 0
            val thermalStart = power.currentThermalStatus
            val start = System.nanoTime()
            var nextSample = start + 1_000_000_000L
            var lastEncoded = 0L
            var lastLatencySum = 0L
            var lastSampleRendered = 0L
            fun sample(phase: String) {
                val now = System.nanoTime()
                val enc = encoded.get()
                val frames = enc - lastEncoded
                val currentUa = battery.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
                val voltageMv = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
                    ?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0) ?: 0
                val powerMw = kotlin.math.abs(currentUa) / 1000.0 * voltageMv / 1000.0
                if (phase == "paced") { powerSum += powerMw; powerSamples++ }
                samples.put(JSONObject().put("t", (now - start) / 1e9).put("phase", phase)
                    .put("encodedFps", frames).put("rendered", rendered - lastSampleRendered).put("drops", drops)
                    .put("latencyAvgMs", if (frames > 0) (latencySumUs.get() - lastLatencySum) / frames / 1000.0 else 0.0)
                    .put("powerMw", powerMw).put("thermal", power.currentThermalStatus)
                    .put("headroom", power.getThermalHeadroom(0).toDouble()))
                lastEncoded = enc
                lastLatencySum = latencySumUs.get()
                lastSampleRendered = rendered
                nextSample = now + 1_000_000_000L
            }

            // Paced: one frame per 30 fps slot; a slot that is already gone
            // (producer stalled) or finds no free encoder buffer is a drop.
            val pacedEnd = start + pacedSeconds * 1_000_000_000L
            var slot = start
            while (slot < pacedEnd && drainError == null) {
                val now = System.nanoTime()
                if (now < slot) Thread.sleep((slot - now) / 1_000_000, ((slot - now) % 1_000_000).toInt())
                if (System.nanoTime() > slot + PERIOD_NS) {
                    drops++
                } else {
                    val gpu = produce(slot)
                    if (gpu < 0) error("render failed: ${nativeLastError()}")
                    if (gpu == 0L) drops++ else { rendered++; gpuSumUs += gpu; gpuMaxUs = max(gpuMaxUs, gpu) }
                }
                frame++
                slot += PERIOD_NS
                if (System.nanoTime() >= nextSample) sample("paced")
            }
            val pacedEncoded = encoded.get()
            val pacedLatencySum = latencySumUs.get()
            val pacedLatencyMax = latencyMaxUs.get()
            val pacedFrames = frame
            report.put("paced", JSONObject().put("seconds", pacedSeconds).put("slots", pacedFrames)
                .put("rendered", rendered).put("drops", drops).put("encoded", pacedEncoded)
                .put("latencyAvgMs", if (pacedEncoded > 0) pacedLatencySum / pacedEncoded / 1000.0 else 0.0)
                .put("latencyMaxMs", pacedLatencyMax / 1000.0)
                .put("gpuAvgMs", if (rendered > 0) gpuSumUs / rendered / 1000.0 else 0.0)
                .put("gpuMaxMs", gpuMaxUs / 1000.0)
                .put("powerAvgMw", if (powerSamples > 0) powerSum / powerSamples else 0.0)
                .put("thermalStart", thermalStart).put("thermalEnd", power.currentThermalStatus))

            // Burst: feed as fast as buffers come back; PTS stays on the 30 fps grid.
            val burstStartEncoded = encoded.get()
            val burstStart = System.nanoTime()
            val burstEnd = burstStart + burstSeconds * 1_000_000_000L
            while (System.nanoTime() < burstEnd && drainError == null) {
                val gpu = produce(slot)
                if (gpu < 0) error("render failed: ${nativeLastError()}")
                if (gpu > 0) { frame++; slot += PERIOD_NS; rendered++ } else Thread.sleep(1)
                if (System.nanoTime() >= nextSample) sample("burst")
            }
            val burstElapsed = (System.nanoTime() - burstStart) / 1e9
            report.put("burst", JSONObject().put("seconds", burstElapsed)
                .put("encodedFps", (encoded.get() - burstStartEncoded) / burstElapsed)
                .put("thermalEnd", power.currentThermalStatus))
            report.put("native", JSONObject(nativeInfo(handle)))
            report.put("charging", battery.isCharging)
            report.put("samples", samples)
            drainError?.let { report.put("drainError", it.toString()) }
            report.put("success", drainError == null)
        } catch (t: Throwable) {
            report.put("success", false).put("error", t.toString())
        } finally {
            running.set(false)
            runCatching { drain.join(2_000) }
            runCatching { codec.stop() }
            runCatching { codec.release() }
            writer?.close()
            releaseThread?.quitSafely()
            if (handle != 0L) nativeDestroy(handle)
            surface.release()
        }
        return report
    }
}
