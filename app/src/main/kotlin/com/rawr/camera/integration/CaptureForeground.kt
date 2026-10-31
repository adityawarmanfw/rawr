package com.rawr.camera.integration

import java.util.concurrent.atomic.AtomicReference

/**
 * Local camera-request admission identity. Pause/stop invalidates every pending request immediately;
 * resume issues a new identity. It is not location authorization and never waits for location work.
 */
class CaptureForeground(private val onChanged: (Boolean) -> Unit = {}) {
    private class Token(val owner: Any)
    private val current = AtomicReference<Token?>(null)

    fun token(): Any? = current.get()

    fun resume(owner: Any) {
        current.set(Token(owner))
        onChanged(true)
    }

    /** A late pause from a replaced owner leaves the newer identity intact. */
    fun pause(owner: Any) {
        val token = current.get() ?: return
        if (token.owner === owner && current.compareAndSet(token, null)) onChanged(false)
    }
}
