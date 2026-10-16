package com.rawr.camera.renderer.library

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.rawr.camera.renderer.RendererActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/** Keeps the DNG conversion queue running in the foreground with progress. */
class DngConvertService : Service() {
    private var wakeLock: PowerManager.WakeLock? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    override fun onBind(intent: Intent?) = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_CANCEL) {
            DngConvertQueue.get(this).cancelAll()
            return START_NOT_STICKY
        }
        channel(this)
        // Use the platform overload: older AndroidX masks out MEDIA_PROCESSING.
        startForeground(NOTIFICATION, notification(DngConvertQueue.get(this).state.value),
            if (Build.VERSION.SDK_INT >= 35) ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROCESSING else ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        if (wakeLock?.isHeld != true) {
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "rawr:dng_convert").apply { acquire(60 * 60 * 1000L) }
        }
        scope.coroutineContext[kotlinx.coroutines.Job]?.children?.forEach { it.cancel() }
        scope.launch {
            val manager = getSystemService(NotificationManager::class.java)
            DngConvertQueue.get(this@DngConvertService).state.collect { manager.notify(NOTIFICATION, notification(it)) }
        }
        return START_NOT_STICKY
    }

    override fun onTimeout(startId: Int, fgsType: Int) {
        DngConvertQueue.get(this).cancelAll()
        stopSelf()
    }

    override fun onDestroy() {
        scope.cancel()
        wakeLock?.let { if (it.isHeld) it.release() }
        super.onDestroy()
    }

    private fun notification(state: DngConvertState): Notification {
        val position = state.done + state.failed.size + 1
        val cancel = PendingIntent.getService(this, 0, Intent(this, DngConvertService::class.java).setAction(ACTION_CANCEL),
            PendingIntent.FLAG_IMMUTABLE)
        return NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(com.rawr.camera.R.drawable.ic_notification)
            .setContentTitle("Converting DNG $position of ${state.total.coerceAtLeast(1)}")
            .setContentText(state.current?.entry?.name ?: "Preparing")
            .setProgress(1000, state.permille, state.current == null)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .addAction(0, "Cancel", cancel)
            .setContentIntent(PendingIntent.getActivity(this, 0, Intent(this, RendererActivity::class.java), PendingIntent.FLAG_IMMUTABLE))
            .build()
    }

    companion object {
        private const val CHANNEL = "rawr_dng_convert"
        private const val NOTIFICATION = 4203
        private const val ACTION_CANCEL = "com.rawr.camera.DNG_CONVERT_CANCEL"

        private fun channel(context: Context) {
            context.getSystemService(NotificationManager::class.java)
                .createNotificationChannel(NotificationChannel(CHANNEL, "DNG conversion", NotificationManager.IMPORTANCE_LOW))
        }

        fun start(context: Context) {
            ContextCompat.startForegroundService(context, Intent(context, DngConvertService::class.java))
        }

        // Context.stopService matches an Intent component; it does not remove a SAM callback.
        @android.annotation.SuppressLint("ImplicitSamInstance")
        fun stop(context: Context) {
            context.stopService(Intent(context, DngConvertService::class.java))
        }
    }
}
