package com.rawr.camera.location

import android.Manifest
import android.app.AppOpsManager
import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.location.Location
import android.location.LocationListener
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
import kotlinx.coroutines.async
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
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ScheduledThreadPoolExecutor
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
    private val oneShots get() = ControlledLocationManager.currentRequests

    @org.junit.Before fun resetShadowControls() {
        ControlledLocationManager.resetControls()
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

    class OneShot(val provider: String, val request: LocationRequest, val signal: CancellationSignal?,
                  val consumer: Consumer<Location?>)

    @Implements(LocationManager::class)
    class ControlledLocationManager : ShadowLocationManager() {
        @Implementation
        override fun requestLocationUpdates(provider: String, request: LocationRequest, executor: Executor, listener: LocationListener) {
            registrations += provider
            onRegistration?.invoke()
            if (provider in failing) throw RuntimeException("Injected registration failure")
            super.requestLocationUpdates(provider, request, executor, listener)
        }

        @Implementation
        override fun isProviderEnabled(provider: String): Boolean {
            if (throwingQuery) throw RuntimeException("Injected provider query failure")
            return super.isProviderEnabled(provider)
        }

        @Implementation
        override fun getCurrentLocation(provider: String, request: LocationRequest, signal: CancellationSignal?,
                                        executor: Executor, consumer: Consumer<Location>) {
            @Suppress("UNCHECKED_CAST")
            currentRequests += OneShot(provider, request, signal, consumer as Consumer<Location?>)
        }

        companion object {
            val failing = mutableSetOf<String>()
            val registrations = mutableListOf<String>()
            val currentRequests = mutableListOf<OneShot>()
            var onRegistration: (() -> Unit)? = null
            var throwingQuery = false
            fun resetControls() {
                throwingQuery = false
                failing.clear()
                registrations.clear()
                currentRequests.clear()
                onRegistration = null
            }
        }
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

    /** Parked handoffs and worker tasks; nothing runs until a test drains or advances it. */
    private class Tasks : Executor {
        val handoffs = ArrayDeque<Runnable>()
        val pending = ArrayDeque<Runnable>()
        val delayed = mutableMapOf<Runnable, Long>()
        var now = 0L
        var rejecting = false
        override fun execute(command: Runnable) {
            if (rejecting) throw RejectedExecutionException("Injected rejection")
            pending.addLast(command)
        }
        fun handoff(task: Runnable) { handoffs.addLast(task) }
        fun defer(command: Runnable, delay: Long): () -> Unit {
            delayed[command] = now + delay
            return { delayed.remove(command); Unit }
        }
        fun drain() {
            var count = 0
            while (handoffs.isNotEmpty() || pending.isNotEmpty()) {
                check(++count < 50)
                (handoffs.removeFirstOrNull() ?: pending.removeFirst()).run()
            }
        }
        fun advance(millis: Long) {
            ShadowSystemClock.advanceBy(Duration.ofMillis(millis))
            now += millis
            delayed.filterValues { it <= now }.keys.forEach { delayed.remove(it); pending.addLast(it) }
            drain()
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
        return CameraLocation(app, tasks, tasks::defer, tasks::handoff)
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
        assertEquals("Control events coalesce into one handoff", 1, tasks.handoffs.size)
        tasks.rejecting = true
        tasks.drain()
        queue.enqueue(CaptureOutputFormat.DngAndJpeg)
        assertSame("Rejected submission neither blocks capture nor clears eligible GPS", prepared, frozen.last())
        tasks.rejecting = false
        collector.setEnabled(false)
        queue.enqueue(CaptureOutputFormat.DngAndJpeg)
        assertNull("A later OFF admission stays absent", frozen.last())
        assertSame("Admitted values are never rewritten", prepared, frozen.first())
    }

    @Test fun failedReturnValidationSettlesWithoutWaitingForRetryOrPosition() = runTest {
        val tasks = Tasks()
        val collector = preparedCollector(tasks)
        collector.setForeground(false)
        tasks.drain()
        collector.setForeground(true)
        ControlledLocationManager.failing += FUSED_PROVIDER
        var settledBeforeRegistration: Boolean? = null
        val waiting = async(UnconfinedTestDispatcher(testScheduler)) { collector.validateAccess() }
        ControlledLocationManager.onRegistration = {
            if (settledBeforeRegistration == null) settledBeforeRegistration = waiting.isCompleted
        }
        assertFalse(waiting.isCompleted)
        tasks.drain()
        assertEquals("Validation completes before any registration IPC", true, settledBeforeRegistration)
        assertTrue(waiting.await())
        ControlledLocationManager.onRegistration = null
        collector.setForeground(false)
        collector.setForeground(true)
        val parked = async(UnconfinedTestDispatcher(testScheduler)) { collector.validateAccess() }
        assertFalse(parked.isCompleted)
        collector.setEnabled(false)
        assertFalse("OFF bypasses the validation wait without the worker", parked.await())
        tasks.drain()
        val rejected = CameraLocation(app, Executor { throw RejectedExecutionException("Injected rejection") },
            handoff = { it.run() })
        rejected.setEnabled(true)
        assertFalse("Rejected submission settles to no GPS", rejected.validateAccess())
    }

    @Test fun foregroundRequestsFollowProviderStrategyAndRejectLateCallbacks() {
        val tasks = Tasks()
        val collector = availableCollector(tasks)
        collector.setEnabled(true)
        collector.setForeground(true)
        tasks.drain()
        val passive = platform.getLocationRequests(PASSIVE_PROVIDER).single()
        assertEquals(LocationRequest.PASSIVE_INTERVAL, passive.intervalMillis)
        assertEquals(0L, passive.minUpdateIntervalMillis)
        assertEquals(120_000L, passive.durationMillis)
        val balanced = platform.getLocationRequests(FUSED_PROVIDER).single()
        assertEquals(30_000L, balanced.intervalMillis)
        assertEquals(10_000L, balanced.minUpdateIntervalMillis)
        assertEquals(10f, balanced.minUpdateDistanceMeters, 0f)
        assertEquals(LocationRequest.QUALITY_BALANCED_POWER_ACCURACY, balanced.quality)
        assertEquals(0L, balanced.maxUpdateDelayMillis)
        assertEquals(120_000L, balanced.durationMillis)
        assertTrue(platform.getLocationRequests(NETWORK_PROVIDER).isEmpty())
        assertTrue(platform.getLocationRequests(GPS_PROVIDER).isEmpty())
        val oneShot = oneShots.single()
        assertEquals(GPS_PROVIDER, oneShot.provider)
        assertEquals(30_000L, oneShot.request.durationMillis)
        val passiveListener = listener(PASSIVE_PROVIDER)
        passiveListener.onLocationChanged(mutableListOf(fix(GPS_PROVIDER, 3f), fix(NETWORK_PROVIDER, 900f)))
        val best = requireNotNull(collector.snapshot())
        assertEquals("Every delivered list member goes through the selector", 3.0, best.nativeValues()[3], 0.0)
        collector.setForeground(false)
        passiveListener.onLocationChanged(fix(GPS_PROVIDER, 1f, latitude = 1.0))
        assertSame("Pause rejects late callbacks immediately", best, collector.snapshot())
        tasks.drain()
        assertTrue(noRequests())
        assertTrue(requireNotNull(oneShot.signal).isCanceled)
        platform.setProviderEnabled(FUSED_PROVIDER, false)
        collector.setForeground(true)
        tasks.drain()
        assertEquals(1, platform.getLocationRequests(NETWORK_PROVIDER).size)
        assertTrue(platform.getLocationRequests(FUSED_PROVIDER).isEmpty())
        assertSame("Losing one provider keeps the retained observation", best, collector.snapshot())
        platform.setProviderEnabled(FUSED_PROVIDER, true)
        app.sendBroadcast(Intent(LocationManager.PROVIDERS_CHANGED_ACTION))
        idle()
        tasks.drain()
        assertEquals(1, platform.getLocationRequests(FUSED_PROVIDER).size)
        assertTrue(platform.getLocationRequests(NETWORK_PROVIDER).isEmpty())
        repeat(100) { collector.setForeground(false); collector.setForeground(true) }
        assertEquals(1, tasks.handoffs.size)
        collector.setEnabled(false)
        tasks.drain()
        assertTrue(noRequests())
    }

    @Test fun startupAndRefreshAttemptsRespectOutstandingRequestAndCooldown() {
        val tasks = Tasks()
        val collector = availableCollector(tasks)
        platform.setLastKnownLocation(NETWORK_PROVIDER, fix(NETWORK_PROVIDER, 3f))
        collector.setEnabled(true)
        collector.setForeground(true)
        tasks.drain()
        val seeded = requireNotNull(collector.snapshot())
        assertEquals("Startup attempts once even after a good seed", 1, oneShots.size)
        assertTrue(tasks.handoffs.isEmpty())
        oneShots[0].consumer.accept(null)
        collector.setForeground(false)
        collector.setForeground(true)
        tasks.drain()
        assertEquals("Pause/resume cannot reset the completion cooldown", 1, oneShots.size)
        assertEquals("Only maintenance is scheduled; no delayed positioning retry", 1, tasks.delayed.size)
        tasks.advance(31_000)
        assertEquals(1, oneShots.size)
        assertSame(seeded, collector.snapshot())
        tasks.drain()
        assertEquals("A post-admission hint refreshes for later captures", 2, oneShots.size)
        collector.snapshot()
        tasks.drain()
        assertEquals("At most one outstanding one-shot", 2, oneShots.size)
        oneShots[1].consumer.accept(null)
        collector.snapshot()
        tasks.drain()
        tasks.advance(31_000)
        assertEquals("A hint during cooldown is discarded, not retried", 2, oneShots.size)
        assertSame(seeded, collector.snapshot())
        tasks.drain()
        assertEquals(3, oneShots.size)
        oneShots[2].consumer.accept(fix(GPS_PROVIDER, 2f))
        assertNotSame(seeded, collector.snapshot())
        tasks.advance(47_999)
        collector.snapshot()
        tasks.advance(1)
        assertEquals("A hint racing maintenance is still consumed", 4, oneShots.size)
        oneShots[3].consumer.accept(null)
        tasks.advance(110_000)
        assertEquals("Routine renewal never initiates a one-shot", 4, oneShots.size)
        collector.setEnabled(false)
        tasks.drain()
    }

    @Test fun maintenanceRenewsBothPathsIndependentlyAndReattemptsFailuresOnlyOnCadence() {
        val tasks = Tasks()
        val collector = availableCollector(tasks)
        ControlledLocationManager.failing += FUSED_PROVIDER
        collector.setEnabled(true)
        collector.setForeground(true)
        tasks.drain()
        assertEquals(1, platform.getLocationRequests(PASSIVE_PROVIDER).size)
        assertTrue(platform.getLocationRequests(FUSED_PROVIDER).isEmpty())
        val passiveListener = listener(PASSIVE_PROVIDER)
        passiveListener.onLocationChanged(fix())
        val prepared = requireNotNull(collector.snapshot())
        fun attempts(provider: String) = ControlledLocationManager.registrations.count { it == provider }
        tasks.advance(109_999)
        assertEquals("No fast retry outside the cadence", 1, attempts(FUSED_PROVIDER))
        ControlledLocationManager.failing.clear()
        tasks.advance(1)
        assertEquals(1, platform.getLocationRequests(FUSED_PROVIDER).size)
        assertSame("Renewal reuses the listener/provider pair", passiveListener, listener(PASSIVE_PROVIDER))
        assertEquals(1, platform.getLocationRequests(PASSIVE_PROVIDER).size)
        assertSame("Failures never clear an eligible candidate", prepared, collector.snapshot())
        ControlledLocationManager.failing += PASSIVE_PROVIDER
        tasks.advance(110_000)
        assertEquals("One path's failure does not skip the other", 3, attempts(FUSED_PROVIDER))
        ControlledLocationManager.failing.clear()
        ControlledLocationManager.throwingQuery = true
        tasks.advance(110_000)
        ControlledLocationManager.throwingQuery = false
        tasks.advance(110_000)
        assertEquals("A cycle that throws still reattempts at the next cadence", 4, attempts(FUSED_PROVIDER))
        ControlledLocationManager.onRegistration = {
            ControlledLocationManager.onRegistration = null
            collector.setEnabled(false)
        }
        tasks.advance(110_000)
        assertTrue("A registration completing after OFF is removed, not renewed", noRequests())
        assertTrue(tasks.delayed.isEmpty())
        collector.setEnabled(true)
        tasks.drain()
        assertNotSame(passiveListener, listener(PASSIVE_PROVIDER))
        passiveListener.onLocationChanged(fix())
        assertNull("Old handles cannot publish after OFF -> ON", collector.snapshot())
        collector.setEnabled(false)
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

    @Test fun inactiveRetentionUsesOriginalAgeThenReleasesObservers() {
        for (off in listOf(false, true)) {
            val tasks = Tasks()
            val collector = preparedCollector(tasks)
            tasks.advance(100_000)
            collector.setForeground(false)
            tasks.drain()
            assertEquals("Protected system broadcasts need an exported receiver", Context.RECEIVER_EXPORTED, app.receiverFlags)
            assertTrue(noRequests())
            assertEquals("The maintenance handle now owns original-age expiry", 1, tasks.delayed.size)
            app.sendBroadcast(Intent(LocationManager.PROVIDERS_CHANGED_ACTION))
            idle()
            assertEquals("Observers remain while the candidate is eligible", 1, tasks.handoffs.size)
            tasks.drain()
            if (off) {
                collector.setEnabled(false)
                assertNull(collector.snapshot())
                tasks.drain()
            } else {
                tasks.advance(20_001)
                assertNull(collector.snapshot())
            }
            assertTrue(tasks.delayed.isEmpty())
            app.sendBroadcast(Intent(LocationManager.PROVIDERS_CHANGED_ACTION))
            appOps.setMode(AppOpsManager.OPSTR_FINE_LOCATION, app.applicationInfo.uid, app.packageName, AppOpsManager.MODE_ALLOWED)
            idle()
            assertTrue("Released observers schedule no work", tasks.handoffs.isEmpty())
            collector.setEnabled(true)
            collector.setForeground(true)
            tasks.drain()
            assertNull("OFF or expiry discards the retained observation", collector.snapshot())
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
        settings.dispatch(SetLocationTagging(false))
        assertFalse("Still/video settings snapshots freeze absence", editor.state.value.locationTagging)
        assertNull("The collector gate is OFF before persistence, flow delivery or worker", collector.snapshot())
        assertTrue(tasks.pending.isEmpty())
        assertFalse("Persistence remains parked", persistence.isCompleted)
        persistence.complete(Unit)
        tasks.drain()
    }
}
