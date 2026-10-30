package com.rawr.camera.location

import android.Manifest
import android.annotation.SuppressLint
import android.app.AppOpsManager
import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.location.LocationRequest
import android.os.CancellationSignal
import android.os.Process
import android.os.SystemClock
import com.rawr.camera.location.LocationPolicy as Policy
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.Executor
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference

/**
 * Process-wide location collector. Capture reads [snapshot] atomically; location-service calls and access
 * checks run on [worker], reached through the non-main [handoff]. Callbacks publish directly through the store.
 */
class CameraLocation internal constructor(
    private val context: Application,
    private val worker: Executor = ScheduledThreadPoolExecutor(1) { task ->
        Thread({
            try { Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND) } catch (_: RuntimeException) { }
            task.run()
        }, "rawr-camera-location")
    }.apply {
        removeOnCancelPolicy = true
        setKeepAliveTime(30L, TimeUnit.SECONDS)
        allowCoreThreadTimeOut(true)
    },
    private val defer: (Runnable, Long) -> (() -> Unit) = { task, delay ->
        val future = (worker as ScheduledExecutorService).schedule(task, delay, TimeUnit.MILLISECONDS)
        val cancel: () -> Unit = { future.cancel(false); Unit }
        cancel
    },
    private val handoff: (Runnable) -> Unit = { task -> handoffScope.launch { task.run() } },
    private val nowNanos: () -> Long = SystemClock::elapsedRealtimeNanos
) {
    private val store = GpsSnapshotStore()
    private val wakePending = AtomicBoolean()
    private val refreshHint = AtomicBoolean()
    private val waiters = ConcurrentLinkedQueue<CompletableDeferred<Boolean>>()
    private val oneShot = AtomicReference<CancellationSignal?>()
    private val oneShotEndedNanos = AtomicLong(Long.MIN_VALUE)
    private val ingressExecutor = Executor { task -> try { task.run() } catch (_: RuntimeException) { } }
    private val manager by lazy { context.getSystemService(LocationManager::class.java) }
    private val appOps by lazy { context.getSystemService(AppOpsManager::class.java) }

    // Worker-confined collection state.
    private val passive = Path(listOf(LocationManager.PASSIVE_PROVIDER),
        LocationRequest.Builder(LocationRequest.PASSIVE_INTERVAL)
            .setQuality(LocationRequest.QUALITY_LOW_POWER)
            .setMinUpdateIntervalMillis(Policy.PASSIVE_MIN_INTERVAL_MILLIS)
            .setDurationMillis(Policy.REQUEST_DURATION_MILLIS)
            .build())
    private val balanced = Path(Policy.CONTINUING_PROVIDERS,
        LocationRequest.Builder(Policy.CONTINUING_INTERVAL_MILLIS)
            .setQuality(LocationRequest.QUALITY_BALANCED_POWER_ACCURACY)
            .setMinUpdateIntervalMillis(Policy.CONTINUING_MIN_INTERVAL_MILLIS)
            .setMinUpdateDistanceMeters(Policy.CONTINUING_MIN_DISTANCE_METERS)
            .setMaxUpdateDelayMillis(0L)
            .setDurationMillis(Policy.REQUEST_DURATION_MILLIS)
            .build())
    private val oneShotRequest = LocationRequest.Builder(0L)
        .setQuality(LocationRequest.QUALITY_HIGH_ACCURACY)
        .setDurationMillis(Policy.ONE_SHOT_DURATION_MILLIS)
        .build()
    private var collectionIdentity: Long? = null
    private var observing = false
    private var cancelMaintenance: (() -> Unit)? = null

    private val opListener = AppOpsManager.OnOpChangedListener { _, packageName ->
        if (packageName == context.packageName) signal()
    }
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) = signal()
    }

    /** Synchronous tagging gate: OFF clears the candidate and settles validation before returning. */
    fun setEnabled(value: Boolean) {
        if (!store.setEnabled(value)) return
        if (!value) settle(drainWaiters(), false)
        signal()
    }

    fun setForeground(value: Boolean) {
        if (store.setForeground(value) && store.state.enabled) signal()
    }

    /** Admission read: atomic, never waits. A candidate missing the refresh target hints a later refresh. */
    fun snapshot(): CaptureGps? {
        val now = nowNanos()
        val current = store.state
        val gps = current.admissible(now)
        if (current.enabled && current.foreground && current.missesRefreshTarget(now) &&
            refreshHint.compareAndSet(false, true)) signal()
        return gps
    }

    /** Microphone-continuation only: completes after the next worker access classification, before acquisition. */
    suspend fun validateAccess(): Boolean {
        if (!store.state.enabled) return false
        val waiter = CompletableDeferred<Boolean>()
        waiters.add(waiter)
        if (!store.state.enabled) waiter.complete(false)
        signal()
        return waiter.await()
    }

    private fun signal() {
        if (wakePending.compareAndSet(false, true)) handoff(Runnable(::submit))
    }

    private fun submit() {
        try {
            worker.execute {
                wakePending.set(false)
                control(maintenanceDue = false)
            }
        } catch (_: RuntimeException) {
            rejected()
        } catch (_: OutOfMemoryError) {
            rejected()
        }
    }

    private fun rejected() {
        wakePending.set(false)
        settle(drainWaiters(), false)
    }

    private fun drainWaiters(): List<CompletableDeferred<Boolean>> = generateSequence { waiters.poll() }.toList()

    private fun settle(waiting: List<CompletableDeferred<Boolean>>, usable: Boolean) = waiting.forEach { it.complete(usable) }

    private fun control(maintenanceDue: Boolean) {
        val waiting = drainWaiters()
        try {
            reconcile(waiting, maintenanceDue, refreshHint.getAndSet(false))
        } catch (_: RuntimeException) {
        } finally {
            settle(waiting, false)
        }
    }

    private fun reconcile(waiting: List<CompletableDeferred<Boolean>>, maintenanceDue: Boolean, hint: Boolean) {
        if (!store.state.enabled) {
            shutdown()
            return
        }
        val foreground = store.state.foreground
        try {
            classify(foreground)
        } catch (_: RuntimeException) {
            store.suppress()
        }
        val current = store.state
        settle(waiting, current.usable)
        if (!current.enabled) {
            shutdown()
            return
        }
        if (!current.foreground || !current.usable) {
            removeCollection()
            cancelOneShot()
            val remaining = store.expire(nowNanos())
            if (current.foreground || remaining != null) startObserving() else stopObserving()
            if (!current.foreground && remaining != null) arm(remaining / 1_000_000L + 1L) else disarm()
            return
        }
        val renew = collectionIdentity != current.identity || maintenanceDue
        // Arm renewal before provider registration so failures still reattempt at the next cadence.
        if (renew) arm(Policy.MAINTENANCE_MILLIS)
        startObserving()
        if (collectionIdentity != current.identity) {
            removeCollection()
            cancelOneShot()
            collectionIdentity = current.identity
            seed(current.identity)
            register(passive, current.identity)
            register(balanced, current.identity)
            attemptOneShot(current.identity)
            return
        }
        if (maintenanceDue) {
            register(passive, current.identity)
            register(balanced, current.identity)
        } else if (balanced.selected() != balanced.provider) {
            register(balanced, current.identity)
        }
        if (hint && store.state.missesRefreshTarget(nowNanos())) attemptOneShot(current.identity)
    }

    @Suppress("DEPRECATION")
    private fun classify(foreground: Boolean) {
        val manager = manager
        val appOps = appOps
        if (manager == null || appOps == null || !manager.isLocationEnabled) {
            store.classify(LocationPrecision.NONE, false)
            return
        }
        val uid = Process.myUid()
        fun granted(permission: String) = context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED
        fun retained(operation: String) = when (appOps.unsafeCheckOpRawNoThrow(operation, uid, context.packageName)) {
            AppOpsManager.MODE_ALLOWED, AppOpsManager.MODE_FOREGROUND, AppOpsManager.MODE_DEFAULT -> true
            else -> false
        }
        val coarse = granted(Manifest.permission.ACCESS_COARSE_LOCATION) && retained(AppOpsManager.OPSTR_COARSE_LOCATION)
        val precision = when {
            !coarse -> LocationPrecision.NONE
            granted(Manifest.permission.ACCESS_FINE_LOCATION) && retained(AppOpsManager.OPSTR_FINE_LOCATION) -> LocationPrecision.FINE
            else -> LocationPrecision.COARSE
        }
        if (precision == LocationPrecision.NONE || !foreground) {
            store.classify(precision, null)
            return
        }
        // Effective foreground access; raw modes cannot express whether FOREGROUND currently applies.
        val operations = if (precision == LocationPrecision.FINE) {
            listOf(AppOpsManager.OPSTR_FINE_LOCATION, AppOpsManager.OPSTR_COARSE_LOCATION)
        } else listOf(AppOpsManager.OPSTR_COARSE_LOCATION)
        var suppressed = false
        for (operation in operations) {
            when (appOps.noteOpNoThrow(operation, uid, context.packageName, context.attributionTag, null)) {
                AppOpsManager.MODE_ALLOWED, AppOpsManager.MODE_DEFAULT, AppOpsManager.MODE_FOREGROUND -> Unit
                AppOpsManager.MODE_IGNORED -> suppressed = true
                else -> {
                    store.classify(LocationPrecision.NONE, false)
                    return
                }
            }
        }
        store.classify(precision, suppressed)
    }

    private fun wanted(identity: Long): Boolean = store.state.let { it.foreground && it.usable && it.identity == identity }

    private fun ingest(identity: Long, location: Location) {
        store.offer(identity, location.provider, location.elapsedRealtimeNanos, nowNanos(), location.latitude,
            location.longitude, if (location.hasAltitude()) location.altitude else null,
            if (location.hasAccuracy()) location.accuracy.toDouble() else null, location.time)
    }

    private inner class Ingress(val identity: Long) : LocationListener {
        override fun onLocationChanged(location: Location) = ingest(identity, location)
        override fun onLocationChanged(locations: MutableList<Location>) = locations.forEach { ingest(identity, it) }
    }

    private inner class Path(val choices: List<String>, val request: LocationRequest) {
        var provider: String? = null
        var listener: Ingress? = null
        fun selected(): String? = choices.firstOrNull { checkNotNull(manager).isProviderEnabled(it) }
    }

    /** Registers or renews one continuing path; the same listener/provider pair overwrites its prior request. */
    @SuppressLint("MissingPermission")
    private fun register(path: Path, identity: Long) {
        val manager = checkNotNull(manager)
        val provider = path.selected()
        if (provider != path.provider || path.listener?.identity != identity) remove(path)
        if (provider == null || !wanted(identity)) return
        val listener = path.listener ?: Ingress(identity).also {
            path.listener = it
            path.provider = provider
        }
        try {
            manager.requestLocationUpdates(provider, path.request, ingressExecutor, listener)
        } catch (_: RuntimeException) {
            // Retained for cleanup; the next maintenance cycle reattempts.
        }
        if (!wanted(identity)) remove(path)
    }

    private fun remove(path: Path) {
        val listener = path.listener ?: return
        path.listener = null
        path.provider = null
        try { manager?.removeUpdates(listener) } catch (_: RuntimeException) { }
    }

    private fun removeCollection() {
        collectionIdentity = null
        remove(passive)
        remove(balanced)
    }

    @SuppressLint("MissingPermission")
    private fun seed(identity: Long) {
        val manager = checkNotNull(manager)
        val providers = try { manager.getProviders(true).sorted() } catch (_: RuntimeException) { emptyList() }
        for (provider in providers) {
            if (!wanted(identity)) return
            try { manager.getLastKnownLocation(provider)?.let { ingest(identity, it) } } catch (_: RuntimeException) { }
        }
    }

    @SuppressLint("MissingPermission")
    private fun attemptOneShot(identity: Long) {
        val ended = oneShotEndedNanos.get()
        if (oneShot.get() != null || !wanted(identity) ||
            (ended != Long.MIN_VALUE && nowNanos() - ended < Policy.ONE_SHOT_COOLDOWN_NANOS)) return
        val manager = checkNotNull(manager)
        val provider = Policy.ONE_SHOT_PROVIDERS.firstOrNull { manager.isProviderEnabled(it) } ?: return
        val signal = CancellationSignal()
        if (!oneShot.compareAndSet(null, signal)) return
        try {
            manager.getCurrentLocation(provider, oneShotRequest, signal, ingressExecutor) { location ->
                if (location != null) ingest(identity, location)
                finishOneShot(signal)
            }
        } catch (_: RuntimeException) {
            cancelOneShot()
        }
    }

    private fun finishOneShot(signal: CancellationSignal): Boolean {
        if (!oneShot.compareAndSet(signal, null)) return false
        oneShotEndedNanos.set(nowNanos())
        return true
    }

    private fun cancelOneShot() {
        val signal = oneShot.get() ?: return
        if (finishOneShot(signal)) try { signal.cancel() } catch (_: RuntimeException) { }
    }

    private fun startObserving() {
        if (observing) return
        try {
            appOps?.startWatchingMode(AppOpsManager.OPSTR_FINE_LOCATION, context.packageName,
                AppOpsManager.WATCH_FOREGROUND_CHANGES, opListener)
            appOps?.startWatchingMode(AppOpsManager.OPSTR_COARSE_LOCATION, context.packageName,
                AppOpsManager.WATCH_FOREGROUND_CHANGES, opListener)
            context.registerReceiver(receiver, IntentFilter().apply {
                addAction(LocationManager.MODE_CHANGED_ACTION)
                addAction(LocationManager.PROVIDERS_CHANGED_ACTION)
            }, Context.RECEIVER_EXPORTED)
        } catch (failure: RuntimeException) {
            try { appOps?.stopWatchingMode(opListener) } catch (_: RuntimeException) { }
            throw failure
        }
        observing = true
    }

    private fun stopObserving() {
        if (!observing) return
        observing = false
        try { appOps?.stopWatchingMode(opListener) } catch (_: RuntimeException) { }
        try { context.unregisterReceiver(receiver) } catch (_: IllegalArgumentException) { }
    }

    private fun arm(delayMillis: Long) {
        disarm()
        cancelMaintenance = defer(Runnable {
            cancelMaintenance = null
            control(maintenanceDue = true)
        }, delayMillis)
    }

    private fun disarm() {
        cancelMaintenance?.invoke()
        cancelMaintenance = null
    }

    private fun shutdown() {
        disarm()
        removeCollection()
        cancelOneShot()
        stopObserving()
    }

    companion object {
        private val handoffScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        @Volatile private var instance: CameraLocation? = null

        fun get(context: Context): CameraLocation = instance ?: synchronized(this) {
            instance ?: CameraLocation(context.applicationContext as Application).also { instance = it }
        }
    }
}
