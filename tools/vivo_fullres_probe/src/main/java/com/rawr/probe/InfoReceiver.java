package com.rawr.probe;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

import java.io.File;
import java.io.PrintWriter;
import java.lang.reflect.Method;

/**
 * am broadcast -n com.rawr.probe/.InfoReceiver --es tag NAME: VivoCameraManager.getInfo(0..63) into files/info_NAME.txt,
 * without bringing an activity to the front (another app's camera stays open).
 */
public final class InfoReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        String tag = intent.getStringExtra("tag");
        File out = new File(context.getExternalFilesDir(null), "info_" + (tag == null ? "now" : tag) + ".txt");
        try (PrintWriter w = new PrintWriter(out)) {
            Class<?> api = Class.forName("android.hardware.vivocamera.VivoCameraManager");
            Object manager = api.getMethod("getInstance").invoke(null);
            Method get = api.getMethod("getInfo", int.class);
            // --es set "1=2,55=-2": write values first (puts the store back after a killed client)
            String set = intent.getStringExtra("set");
            if (set != null && !set.isEmpty()) {
                Method put = api.getMethod("setValue", int.class, int.class);
                for (String kv : set.split(",")) {
                    String[] p = kv.split("=");
                    put.invoke(manager, Integer.parseInt(p[0].trim()), Integer.parseInt(p[1].trim()));
                }
                Thread.sleep(150);
            }
            for (int key = 0; key < 64; key++) {
                try {
                    w.println(key + " = " + get.invoke(manager, key));
                } catch (Exception e) {
                    w.println(key + " ! " + (e.getCause() != null ? e.getCause() : e));
                }
            }
        } catch (Exception e) {
            android.util.Log.e("RAWRPROBE", "info failed", e);
        }
    }
}
