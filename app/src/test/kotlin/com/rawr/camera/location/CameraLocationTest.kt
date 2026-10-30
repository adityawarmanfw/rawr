package com.rawr.camera.location

import android.Manifest
import android.app.AppOpsManager
import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.location.Location
import android.location.LocationManager
import android.location.LocationManager.FUSED_PROVIDER
import android.location.LocationManager.GPS_PROVIDER
import android.location.LocationManager.NETWORK_PROVIDER
import android.location.LocationManager.PASSIVE_PROVIDER
import android.location.LocationRequest
import android.os.CancellationSignal
import android.os.Looper
import android.os.SystemClock
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import com.rawr.camera.architecture.CaptureAction
import com.rawr.camera.architecture.CaptureReducer
import com.rawr.camera.architecture.CaptureScreenController
import com.rawr.camera.architecture.PresentationCaptureAction
import com.rawr.camera.fixtures.CaptureFixtures
import com.rawr.camera.integration.CaptureForeground
import com.rawr.camera.integration.StillCaptureDispatchQueue
import com.rawr.camera.model.CaptureMode
import com.rawr.camera.model.CaptureOutputFormat
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.settings.architecture.PersistentSettingsController
import com.rawr.camera.settings.architecture.SetLocationTagging
import com.rawr.camera.settings.architecture.SettingsEditor
import com.rawr.camera.settings.model.SettingsCatalog
import com.rawr.camera.video.MicrophoneRecordingPermission
import com.rawr.camera.video.RecordingCoordinator
import com.rawr.camera.video.VideoRecording
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import org.robolectric.shadows.ShadowAppOpsManager
import org.robolectric.shadows.ShadowLocationManager
import org.robolectric.shadows.ShadowSystemClock
import java.time.Duration
import java.util.concurrent.Executor
import java.util.function.Consumer

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], application = CameraLocationTest.TrackingApplication::class,
    shadows = [CameraLocationTest.ControlledLocationManager::class, CameraLocationTest.ForegroundAppOps::class])
@OptIn(ExperimentalCoroutinesApi::class)
@Suppress("DEPRECATION")
class CameraLocationTest {
    private val app get() = RuntimeEnvironment.getApplication() as TrackingApplication
    private val platform get() = shadowOf(app.getSystemService(LocationManager::class.java))
    private val appOps get() = shadowOf(app.getSystemService(AppOpsManager::class.java))

    @org.junit.Before fun resetShadowControls() {
        ForegroundAppOps.resetChecks()
    }

    class TrackingApplication : Application() {
        var receiverFlags: Int? = null
        var permissionChecks = 0

        override fun checkSelfPermission(permission: String): Int {
            permissionChecks++
            return super.checkSelfPermission(permission)
        }

        override fun registerReceiver(receiver: BroadcastReceiver?, filter: IntentFilter, flags: Int): Intent? {
            if (filter.hasAction(LocationManager.MODE_CHANGED_ACTION)) {
                receiverFlags = flags
            }
            return super.registerReceiver(receiver, filter, flags)
        }
    }

    @Implements(LocationManager::class)
    class ControlledLocationManager : ShadowLocationManager() {
        @Implementation
        override fun getCurrentLocation(provider: String, request: LocationRequest, signal: CancellationSignal?,
                                        executor: Executor, consumer: Consumer<Location>) = Unit
    }

    @Implements(AppOpsManager::class)
    class ForegroundAppOps : ShadowAppOpsManager() {
        @Implementation
        override fun noteOpNoThrow(op: Int, uid: Int, packageName: String?, attributionTag: String?, message: String?): Int {
            return effectiveMode
        }

        @Implementation
        override fun startWatchingMode(op: Int, packageName: String?, flags: Int, callback: AppOpsManager.OnOpChangedListener) {
            listener = callback
            super.startWatchingMode(op, packageName, flags, callback)
        }

        companion object {
            var effectiveMode = AppOpsManager.MODE_ALLOWED
            var listener: AppOpsManager.OnOpChangedListener? = null
            fun resetChecks() {
                effectiveMode = AppOpsManager.MODE_ALLOWED
                listener = null
            }
        }
    }

    private class Tasks : Executor {
        val handoffs = ArrayDeque<Runnable>()
        val pending = ArrayDeque<Runnable>()
        override fun execute(command: Runnable) {
            pending.addLast(command)
        }
        fun handoff(task: Runnable) { handoffs.addLast(task) }
        fun drain() {
            var count = 0
            while (handoffs.isNotEmpty() || pending.isNotEmpty()) {
                check(++count < 50)
                (handoffs.removeFirstOrNull() ?: pending.removeFirst()).run()
            }
        }
    }

    private class Host : LifecycleOwner {
        override val lifecycle = LifecycleRegistry.createUnsafe(this)
        fun event(event: Lifecycle.Event) = lifecycle.handleLifecycleEvent(event)
    }

    private fun fix(provider: String = FUSED_PROVIDER, accuracy: Float? = 0f, latitude: Double = 0.0) = Location(provider).apply {
        this.latitude = latitude
        longitude = 0.0
        if (accuracy != null) this.accuracy = accuracy
        elapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos()
        time = 1793059200123L
    }

    private fun listener(provider: String) = platform.getLocationUpdateListeners(provider).single()

    private fun noRequests() = listOf(PASSIVE_PROVIDER, FUSED_PROVIDER, NETWORK_PROVIDER, GPS_PROVIDER)
        .all { platform.getLocationRequests(it).isEmpty() }

    private fun availableCollector(tasks: Tasks): CameraLocation {
        shadowOf(app).grantPermissions(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
        for (provider in listOf(GPS_PROVIDER, FUSED_PROVIDER, NETWORK_PROVIDER, PASSIVE_PROVIDER)) {
            platform.setProviderEnabled(provider, true)
        }
        platform.setLocationEnabled(true)
        return CameraLocation(app, tasks, { _, _ -> {} }, tasks::handoff)
    }

    private fun preparedCollector(tasks: Tasks): CameraLocation {
        val collector = availableCollector(tasks)
        collector.setEnabled(true)
        collector.setForeground(true)
        tasks.drain()
        listener(FUSED_PROVIDER).onLocationChanged(fix())
        requireNotNull(collector.snapshot())
        return collector
    }

    private fun idle() = shadowOf(Looper.getMainLooper()).idle()

    @Test fun permissionStartReusesRetainedFixAfterForegroundAppOpNotification() = runTest {
        val tasks = Tasks()
        val collector = availableCollector(tasks)
        val host = Host()
        host.event(Lifecycle.Event.ON_CREATE)
        host.event(Lifecycle.Event.ON_START)
        host.event(Lifecycle.Event.ON_RESUME)
        val foreground = CaptureForeground(collector::setForeground)
        foreground.resume(host)
        collector.setEnabled(true)
        tasks.drain()
        listener(FUSED_PROVIDER).onLocationChanged(fix().apply { altitude = -12.5 })
        val prepared = requireNotNull(collector.snapshot())
        val controller = object : CaptureScreenController {
            override val state = MutableStateFlow(CaptureUiState(CaptureFixtures.baseline(), captureMode = CaptureMode.Video))
            override fun dispatch(action: CaptureAction) {
                if (action is PresentationCaptureAction) state.value = CaptureReducer.reduce(state.value, action)
            }
        }
        var reads = 0
        var launches = 0
        var admitted: CaptureGps? = null
        val dispatcher = StandardTestDispatcher(testScheduler)
        val recording = RecordingCoordinator(controller, { gps ->
            admitted = gps
            object : VideoRecording {
                override val isRecording = true
                override val outputUri: android.net.Uri? = null
                override fun stats() = JSONObject()
                override fun journalSnapshot() = Unit
                override fun close() = Unit
            }
        }, SystemClock::elapsedRealtimeNanos, foreground::token, collector::validateAccess, { reads++; collector.snapshot() },
            dispatcher = UnconfinedTestDispatcher(testScheduler), ioDispatcher = dispatcher)
        val permission = MicrophoneRecordingPermission(host.lifecycle, { true }, { launches++ },
            recording::startAfterMicrophonePermission)
        try {
            permission.request(requireNotNull(recording.permissionTicket()))
            assertEquals(1, launches)
            host.event(Lifecycle.Event.ON_PAUSE)
            foreground.pause(host)
            tasks.drain()
            assertTrue(noRequests())
            assertEquals("Protected system broadcasts need an exported receiver", Context.RECEIVER_EXPORTED, app.receiverFlags)
            ShadowSystemClock.advanceBy(Duration.ofSeconds(5))
            appOps.setMode(AppOpsManager.OPSTR_FINE_LOCATION, app.applicationInfo.uid, app.packageName, AppOpsManager.MODE_FOREGROUND)
            idle()
            tasks.drain()
            permission.onResult(true)
            runCurrent()
            assertEquals(0, reads)
            foreground.resume(host)
            host.event(Lifecycle.Event.ON_RESUME)
            runCurrent()
            assertEquals("Only the permission request waits for its access check", 0, reads)
            tasks.drain()
            runCurrent()
            assertEquals(1, reads)
            assertSame("First recording admission must reuse the retained fix without a new callback", prepared, admitted)
            permission.onResult(true)
            runCurrent()
            assertEquals("A duplicate grant must not read or start again", 1, reads)
            recording.stop()
            runCurrent()
            recording.toggle()
            runCurrent()
            assertEquals("A later ordinary start does not inherit the validation wait", 2, reads)
            assertTrue(tasks.pending.isEmpty())
            assertSame(prepared, admitted)
            ShadowSystemClock.advanceBy(Duration.ofSeconds(116))
            assertNull("Return must not refresh measurement age", collector.snapshot())
        } finally {
            recording.closeWhenIdle {}
            runCurrent()
            collector.setEnabled(false)
            tasks.drain()
            host.event(Lifecycle.Event.ON_DESTROY)
        }
    }

    @Test fun ordinaryAdmissionNeverWaitsForWorkerAndFreezesOnce() = runTest {
        val tasks = Tasks()
        val collector = preparedCollector(tasks)
        val prepared = requireNotNull(collector.snapshot())
        collector.setForeground(false)
        tasks.drain()
        appOps.setMode(AppOpsManager.OPSTR_COARSE_LOCATION, app.applicationInfo.uid, app.packageName, AppOpsManager.MODE_DEFAULT)
        idle()
        collector.setForeground(true)
        val checks = app.permissionChecks
        val frozen = mutableListOf<CaptureGps?>()
        val queue = StillCaptureDispatchQueue(backgroundScope, { frozen.add(collector.snapshot()) }, {}, { _, _ -> })
        queue.enqueue(CaptureOutputFormat.DngAndJpeg)
        assertSame("Ordinary return admission must not wait for the worker", prepared, frozen.single())
        repeat(100) { collector.snapshot() }
        assertEquals("Admission performs no permission IPC", checks, app.permissionChecks)
        assertTrue("Admission never reaches the worker", tasks.pending.isEmpty())
        collector.setEnabled(false)
        queue.enqueue(CaptureOutputFormat.DngAndJpeg)
        assertNull("A later OFF admission stays absent", frozen.last())
        assertSame("Admitted values are never rewritten", prepared, frozen.first())
        tasks.drain()
    }

    @Test fun unclassifiedNotificationsRetainButClassifiedLossClears() {
        for (loss in listOf("provider", "foregroundIgnored", "approximate", "coarseIgnored", "system")) {
            val tasks = Tasks()
            val collector = preparedCollector(tasks)
            val prepared = requireNotNull(collector.snapshot())
            val obsolete = listener(FUSED_PROVIDER)
            when (loss) {
                "provider" -> {
                    platform.setProviderEnabled(FUSED_PROVIDER, false)
                    app.sendBroadcast(Intent(LocationManager.PROVIDERS_CHANGED_ACTION))
                }
                "approximate" -> {
                    shadowOf(app).denyPermissions(Manifest.permission.ACCESS_FINE_LOCATION)
                    appOps.setMode(AppOpsManager.OPSTR_FINE_LOCATION, app.applicationInfo.uid, app.packageName, AppOpsManager.MODE_IGNORED)
                }
                "coarseIgnored" -> appOps.setMode(AppOpsManager.OPSTR_COARSE_LOCATION, app.applicationInfo.uid, app.packageName,
                    AppOpsManager.MODE_IGNORED)
                "system" -> {
                    platform.setLocationEnabled(false)
                    app.sendBroadcast(Intent(LocationManager.MODE_CHANGED_ACTION))
                }
                "foregroundIgnored" -> {
                    ForegroundAppOps.effectiveMode = AppOpsManager.MODE_IGNORED
                    requireNotNull(ForegroundAppOps.listener).onOpChanged(AppOpsManager.OPSTR_FINE_LOCATION, app.packageName)
                }
            }
            idle()
            assertSame("A notification alone keeps the last classified access: $loss", prepared, collector.snapshot())
            tasks.drain()
            if (loss == "provider") {
                assertSame(prepared, collector.snapshot())
            } else {
                assertNull(loss, collector.snapshot())
                obsolete.onLocationChanged(fix(latitude = 1.0))
                assertNull("Old precise callbacks cannot republish: $loss", collector.snapshot())
            }
            if (loss == "foregroundIgnored") {
                ForegroundAppOps.effectiveMode = AppOpsManager.MODE_ALLOWED
                requireNotNull(ForegroundAppOps.listener).onOpChanged(AppOpsManager.OPSTR_FINE_LOCATION, app.packageName)
                tasks.drain()
                assertSame("Effective denial suppresses without discarding", prepared, collector.snapshot())
            } else if (loss == "approximate") {
                listener(FUSED_PROVIDER).onLocationChanged(fix(accuracy = 2000f))
                assertNotNull("Fresh coarse observations are usable", collector.snapshot())
            } else if (loss != "provider" && loss != "foregroundIgnored") {
                appOps.setMode(AppOpsManager.OPSTR_COARSE_LOCATION, app.applicationInfo.uid, app.packageName, AppOpsManager.MODE_ALLOWED)
                platform.setLocationEnabled(true)
                collector.setForeground(false)
                collector.setForeground(true)
                tasks.drain()
                assertNull("Restoring $loss cannot resurrect the cleared observation", collector.snapshot())
            }
            collector.setEnabled(false)
            tasks.drain()
        }
    }

    @Test fun lateStopFromOldOwnerKeepsNewerForegroundIdentity() {
        val changes = mutableListOf<Boolean>()
        val foreground = CaptureForeground { changes += it }
        val first = Any()
        val second = Any()
        foreground.resume(first)
        foreground.resume(second)
        foreground.pause(first)
        assertNotNull(foreground.token())
        foreground.pause(second)
        assertNull(foreground.token())
        assertEquals(listOf(true, true, false), changes)
    }

    @Test fun actualSettingsOffGatesStillAndVideoBeforePersistenceFlowOrWorker() = runTest {
        val tasks = Tasks()
        val collector = preparedCollector(tasks)
        val loaded = SettingsCatalog.initialState().values.copy(locationTagging = true)
        val persistence = CompletableDeferred<Unit>()
        val editor = SettingsEditor(loaded, flowOf(loaded), { persistence.await() }, backgroundScope,
            onChanged = { collector.setEnabled(it.locationTagging) })
        runCurrent()
        val settings = PersistentSettingsController(valuesEditor = editor)
        assertTrue(editor.state.value.locationTagging)
        assertNotNull(collector.snapshot())
        val obsolete = listener(FUSED_PROVIDER)
        settings.dispatch(SetLocationTagging(false))
        assertFalse("Still/video settings snapshots freeze absence", editor.state.value.locationTagging)
        assertNull("The collector gate is OFF before persistence, flow delivery or worker", collector.snapshot())
        assertTrue(tasks.pending.isEmpty())
        assertFalse("Persistence remains parked", persistence.isCompleted)
        persistence.complete(Unit)
        tasks.drain()
        assertTrue(noRequests())
        collector.setEnabled(true)
        tasks.drain()
        obsolete.onLocationChanged(fix())
        assertNull("OFF/ON cannot revive an observation through an old callback", collector.snapshot())
        collector.setEnabled(false)
        tasks.drain()
    }
}
