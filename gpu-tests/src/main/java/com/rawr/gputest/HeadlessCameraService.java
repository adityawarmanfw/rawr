package com.rawr.gputest;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Camera foreground ownership without an Activity or overlay. */
public final class HeadlessCameraService extends Service {
    private static volatile CountDownLatch ready = new CountDownLatch(1);
    public static void resetReady() { ready = new CountDownLatch(1); }
    public static boolean awaitReady() throws InterruptedException { return ready.await(10, TimeUnit.SECONDS); }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        getSystemService(NotificationManager.class).createNotificationChannel(
            new NotificationChannel("rawr-camera-tests", "Headless camera tests", NotificationManager.IMPORTANCE_LOW));
        Notification notification = new Notification.Builder(this, "rawr-camera-tests")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentTitle("RAWR headless camera test")
            .setContentText("Camera2 capture test running; no camera UI")
            .setOngoing(true).build();
        startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA);
        ready.countDown();
        return START_NOT_STICKY;
    }
    @Override public void onDestroy() { stopForeground(STOP_FOREGROUND_REMOVE); super.onDestroy(); }
    @Override public IBinder onBind(Intent intent) { return null; }
}
