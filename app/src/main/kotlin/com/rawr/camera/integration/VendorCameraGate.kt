package com.rawr.camera.integration

import android.content.Context
import android.content.SharedPreferences
import android.os.SystemClock
import android.util.Log
import androidx.annotation.Keep

/**
 * Vendor state a lens route needs while its streams are configured (profile lens field `vendorGate`). Native
 * code calls [acquire] before the device opens and [release] as soon as the session is configured (on any thread).
 */
@Keep
class VendorCameraGate internal constructor(private val gates: Map<String, Gate>) {
    constructor(context: Context) : this(mapOf(VivoFullSizeGate.NAME to VivoFullSizeGate.create(context)))

    interface Gate {
        fun acquire(): Boolean
        fun release()
    }

    @Synchronized
    fun acquire(name: String): Boolean {
        val gate = gates[name] ?: return false.also { Log.w(TAG, "Unknown vendor gate $name") }
        return try {
            gate.acquire()
        } catch (e: Exception) {
            Log.w(TAG, "Vendor gate $name refused", e)
            false
        }
    }

    @Synchronized
    fun release(name: String) {
        try {
            gates[name]?.release()
        } catch (e: Exception) {
            Log.w(TAG, "Vendor gate $name release failed", e)
        }
    }

    private companion object {
        const val TAG = "RawrVendorGate"
    }
}

/**
 * vivo's full-size RAW permission: VivoCameraManager key 40 ("useVivoCamera") = 1 while the streams are configured.
 * cameraserver (Camera3Device::setUseVivoCamera) reads it when it configures streams and then classifies the client as vivo's
 * own camera (AppType 1), whose sensor-mode branch holds the 50 / 200 MP modes; without it they fail to
 * configure ("Function not implemented"). Key 33 (VCFAppType) is rewritten by cameraserver at open, so it is
 * left alone. The value is system-wide: the one found is saved before the change and given back on release;
 * a value left by a process that died is given back at the next start, while it is still ours and no camera
 * is open.
 */
internal class VivoFullSizeGate(
    private val store: Store,
    private val journal: Journal,
    private val pid: Int,
    private val camerasIdle: () -> Boolean,
    private val settle: () -> Unit = { SystemClock.sleep(SETTLE_MS) }
) : VendorCameraGate.Gate {
    interface Store {
        fun get(key: Int): Int
        fun set(key: Int, value: Int)
    }

    /** The key 40 value found before a change: {pid, value}, or null when nothing is held. */
    interface Journal {
        fun read(): IntArray?
        fun write(pid: Int, previous: Int)
        fun clear()
    }

    private var held: Int? = null

    override fun acquire(): Boolean {
        if (held != null) return true
        val previous = store.get(KEY)
        journal.write(pid, previous)
        held = previous
        store.set(KEY, OURS)
        settle()  // setValue is a one-way Binder call
        if (store.get(KEY) == OURS) return true
        release()
        return false
    }

    override fun release() {
        val previous = held ?: return
        held = null
        store.set(KEY, previous)
        settle()
        if (store.get(KEY) == previous) journal.clear()
        else Log.w(TAG, "vivo key $KEY not restored; journal kept for the next start")
    }

    /** Gives back values a dead process of ours left set. Returns what happened, for the log. */
    fun recoverStale(): String {
        val stale = journal.read() ?: return "none"
        if (stale[0] == pid) return "own"
        if (store.get(KEY) != OURS) {
            journal.clear()
            return "not ours"
        }
        if (!camerasIdle()) return "busy"
        store.set(KEY, stale[1])
        settle()
        journal.clear()
        return "restored"
    }

    companion object {
        const val NAME = "vivoFullSize"
        /** VivoCameraManager key read by cameraserver as "useVivoCamera". */
        const val KEY = 40
        const val OURS = 1
        private const val SETTLE_MS = 100L
        private const val TAG = "RawrVendorGate"

        fun create(context: Context): VendorCameraGate.Gate {
            val store = ReflectionStore.open() ?: return Unavailable
            val app = context.applicationContext
            val gate = VivoFullSizeGate(
                store,
                PrefsJournal(app.getSharedPreferences("rawr_vivo_fullsize_gate", Context.MODE_PRIVATE)),
                android.os.Process.myPid(),
                { CameraIdleCheck.allIdle(app) }
            )
            val recovery = gate.recoverStale()
            if (recovery != "none") Log.i(TAG, "vivo full-size values of a previous process: $recovery")
            return gate
        }
    }

    /** Not a vivo framework: every acquire is refused, so full-size lenses don't open. */
    private object Unavailable : VendorCameraGate.Gate {
        override fun acquire() = false
        override fun release() = Unit
    }

    // android.hardware.vivocamera.VivoCameraManager is a hidden vivo framework class: reached by reflection,
    // isolated here so firmware that removes or blocks it only disables the full-size lenses.
    private class ReflectionStore(private val manager: Any, private val get: java.lang.reflect.Method,
                                  private val set: java.lang.reflect.Method) : Store {
        override fun get(key: Int) = get.invoke(manager, key) as Int
        override fun set(key: Int, value: Int) {
            set.invoke(manager, key, value)
        }

        companion object {
            fun open(): ReflectionStore? = try {
                val api = Class.forName("android.hardware.vivocamera.VivoCameraManager")
                ReflectionStore(
                    api.getMethod("getInstance").invoke(null)!!,
                    api.getMethod("getInfo", Int::class.javaPrimitiveType),
                    api.getMethod("setValue", Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
                )
            } catch (e: Exception) {
                null
            }
        }
    }

    private class PrefsJournal(private val prefs: SharedPreferences) : Journal {
        override fun read(): IntArray? =
            if (!prefs.contains(PID)) null else intArrayOf(prefs.getInt(PID, 0), prefs.getInt(PREVIOUS, 0))

        // commit(): the process may die right after the change.
        override fun write(pid: Int, previous: Int) {
            prefs.edit().putInt(PID, pid).putInt(PREVIOUS, previous).commit()
        }

        override fun clear() {
            prefs.edit().clear().commit()
        }

        private companion object {
            const val PID = "pid"
            const val PREVIOUS = "previous"
        }
    }
}

/** True when no camera is in use by anyone: the availability callback reports every camera at registration. */
private object CameraIdleCheck {
    fun allIdle(context: Context): Boolean {
        val manager = context.getSystemService(android.hardware.camera2.CameraManager::class.java) ?: return false
        val busy = java.util.concurrent.ConcurrentHashMap.newKeySet<String>()
        val thread = android.os.HandlerThread("VivoGateIdle").apply { start() }
        val callback = object : android.hardware.camera2.CameraManager.AvailabilityCallback() {
            override fun onCameraAvailable(id: String) {
                busy.remove(id)
            }

            override fun onCameraUnavailable(id: String) {
                busy.add(id)
            }
        }
        return try {
            manager.registerAvailabilityCallback(callback, android.os.Handler(thread.looper))
            SystemClock.sleep(200)
            busy.isEmpty()
        } catch (e: RuntimeException) {
            false
        } finally {
            runCatching { manager.unregisterAvailabilityCallback(callback) }
            thread.quitSafely()
        }
    }
}
