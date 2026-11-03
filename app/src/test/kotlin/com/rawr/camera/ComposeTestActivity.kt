package com.rawr.camera

import android.content.ComponentName
import android.content.Context
import androidx.activity.ComponentActivity
import androidx.test.core.app.ApplicationProvider
import org.robolectric.Shadows.shadowOf

/** Release JVM tests need a host activity too, without adding a test activity to the release APK. */
internal fun registerComposeTestActivity() {
    val context = ApplicationProvider.getApplicationContext<Context>()
    shadowOf(context.packageManager).addActivityIfNotPresent(ComponentName(context, ComponentActivity::class.java))
}
