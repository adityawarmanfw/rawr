package com.rawr.camera.integration

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class VendorCameraGateTest {
    private class FakeStore(initial: Int, private val accepts: Boolean = true) : VivoFullSizeGate.Store {
        val values = mutableMapOf(VivoFullSizeGate.KEY to initial)
        val refused = mutableSetOf<Int>()
        val writes = mutableListOf<Pair<Int, Int>>()
        override fun get(key: Int) = values[key] ?: 0
        override fun set(key: Int, value: Int) {
            writes += key to value
            if (key !in refused && (accepts || value != VivoFullSizeGate.OURS)) values[key] = value
        }
    }

    private class FakeJournal : VivoFullSizeGate.Journal {
        var entry: IntArray? = null
        override fun read() = entry
        override fun write(pid: Int, previous: Int, previousVcf: Int?) {
            entry = if (previousVcf == null) intArrayOf(pid, previous) else intArrayOf(pid, previous, previousVcf)
        }
        override fun clear() {
            entry = null
        }
    }

    private fun gate(store: FakeStore, journal: FakeJournal, pid: Int = 100, idle: Boolean = true, vcf: Boolean = false) =
        VivoFullSizeGate(store, journal, pid, { idle }, settle = {}, vcf = vcf)

    @Test fun acquireJournalsTheFoundValueSetsKey40AndReleaseRestoresIt() {
        val store = FakeStore(initial = 0)
        val journal = FakeJournal()
        val g = gate(store, journal)
        assertTrue(g.acquire())
        assertEquals(VivoFullSizeGate.OURS, store.values[VivoFullSizeGate.KEY])
        assertEquals(listOf(100, 0), journal.entry?.toList())
        // Only key 40 is written; cameraserver owns key 33.
        assertTrue(store.writes.all { it.first == VivoFullSizeGate.KEY })
        assertTrue(g.acquire())  // Already held: no second journal entry or write.
        assertEquals(1, store.writes.size)
        g.release()
        assertEquals(0, store.values[VivoFullSizeGate.KEY])
        assertNull(journal.entry)
        g.release()  // Not held: nothing written.
        assertEquals(2, store.writes.size)
    }

    @Test fun refusedWhenTheStoreKeepsItsValueAndThePreviousValueIsBack() {
        val store = FakeStore(initial = 0, accepts = false)
        val journal = FakeJournal()
        assertFalse(gate(store, journal).acquire())
        assertEquals(0, store.values[VivoFullSizeGate.KEY])
        assertNull(journal.entry)
    }

    @Test fun releaseKeepsTheJournalWhenTheRestoreDoesNotStick() {
        // The one-way restore is lost: the store keeps OURS, so the next start must still recover it.
        val store = object : VivoFullSizeGate.Store {
            var value = 0
            override fun get(key: Int) = value
            override fun set(key: Int, value: Int) {
                if (value == VivoFullSizeGate.OURS) this.value = value
            }
        }
        val journal = FakeJournal()
        val g = VivoFullSizeGate(store, journal, 100, { true }, settle = {})
        assertTrue(g.acquire())
        g.release()
        assertEquals(VivoFullSizeGate.OURS, store.value)
        assertEquals(listOf(100, 0), journal.entry?.toList())
    }

    @Test fun recoveryRestoresOnlyAStaleValueThatIsStillOursWhileCamerasAreIdle() {
        // Nothing journaled.
        assertEquals("none", gate(FakeStore(0), FakeJournal()).recoverStale())

        // Journaled by this same process: left alone.
        FakeJournal().apply { entry = intArrayOf(100, 0) }.let { j ->
            assertEquals("own", gate(FakeStore(1), j).recoverStale())
            assertEquals(listOf(100, 0), j.entry?.toList())
        }

        // Someone else changed the value since: drop the journal, touch nothing.
        FakeJournal().apply { entry = intArrayOf(7, 0) }.let { j ->
            val store = FakeStore(5)
            assertEquals("not ours", gate(store, j).recoverStale())
            assertNull(j.entry)
            assertEquals(5, store.values[VivoFullSizeGate.KEY])
        }

        // Still ours but a camera is open (maybe another client's own lease): wait for the next start.
        FakeJournal().apply { entry = intArrayOf(7, 0) }.let { j ->
            val store = FakeStore(VivoFullSizeGate.OURS)
            assertEquals("busy", gate(store, j, idle = false).recoverStale())
            assertEquals(VivoFullSizeGate.OURS, store.values[VivoFullSizeGate.KEY])
            assertEquals(listOf(7, 0), j.entry?.toList())
        }

        // Still ours and idle: the dead process's previous value comes back.
        FakeJournal().apply { entry = intArrayOf(7, 3) }.let { j ->
            val store = FakeStore(VivoFullSizeGate.OURS)
            assertEquals("restored", gate(store, j).recoverStale())
            assertEquals(3, store.values[VivoFullSizeGate.KEY])
            assertNull(j.entry)
        }
    }

    @Test fun vcfGateAlsoSetsKey33AndGivesBothBack() {
        val store = FakeStore(initial = 0).apply { values[VivoFullSizeGate.VCF_KEY] = 7 }
        val journal = FakeJournal()
        val g = gate(store, journal, vcf = true)
        assertTrue(g.acquire())
        assertEquals(VivoFullSizeGate.OURS, store.values[VivoFullSizeGate.KEY])
        assertEquals(VivoFullSizeGate.VCF_OURS, store.values[VivoFullSizeGate.VCF_KEY])
        assertEquals(listOf(100, 0, 7), journal.entry?.toList())
        g.release()
        assertEquals(0, store.values[VivoFullSizeGate.KEY])
        assertEquals(7, store.values[VivoFullSizeGate.VCF_KEY])
        assertNull(journal.entry)
    }

    @Test fun vcfGateIsRefusedWhenKey33DoesNotStickAndEverythingIsBack() {
        val store = FakeStore(initial = 0).apply {
            values[VivoFullSizeGate.VCF_KEY] = 7
            refused += VivoFullSizeGate.VCF_KEY
        }
        val journal = FakeJournal()
        assertFalse(gate(store, journal, vcf = true).acquire())
        assertEquals(0, store.values[VivoFullSizeGate.KEY])
        assertEquals(7, store.values[VivoFullSizeGate.VCF_KEY])
        assertNull(journal.entry)
    }

    @Test fun plainGateNeverTouchesKey33() {
        val store = FakeStore(initial = 0).apply { values[VivoFullSizeGate.VCF_KEY] = 7 }
        val g = gate(store, FakeJournal())
        assertTrue(g.acquire())
        g.release()
        assertEquals(7, store.values[VivoFullSizeGate.VCF_KEY])
        assertTrue(store.writes.none { it.first == VivoFullSizeGate.VCF_KEY })
    }

    @Test fun vcfRecoveryNeedsBothKeysStillOursAndRestoresBoth() {
        FakeJournal().apply { entry = intArrayOf(7, 3, 9) }.let { j ->
            val store = FakeStore(VivoFullSizeGate.OURS).apply { values[VivoFullSizeGate.VCF_KEY] = VivoFullSizeGate.VCF_OURS }
            assertEquals("restored", gate(store, j, vcf = true).recoverStale())
            assertEquals(3, store.values[VivoFullSizeGate.KEY])
            assertEquals(9, store.values[VivoFullSizeGate.VCF_KEY])
            assertNull(j.entry)
        }
        // Key 33 was changed by someone since: not ours, nothing touched.
        FakeJournal().apply { entry = intArrayOf(7, 3, 9) }.let { j ->
            val store = FakeStore(VivoFullSizeGate.OURS).apply { values[VivoFullSizeGate.VCF_KEY] = 1 }
            assertEquals("not ours", gate(store, j, vcf = true).recoverStale())
            assertEquals(VivoFullSizeGate.OURS, store.values[VivoFullSizeGate.KEY])
            assertNull(j.entry)
        }
    }

    @Test fun describeReadsKeys1And33And40AndMarksARefusedRead() {
        val store = FakeStore(initial = 1).apply {
            values[VivoFullSizeGate.APP_TYPE_KEY] = 21
            values[VivoFullSizeGate.VCF_KEY] = 2
        }
        assertEquals("key1=21 key33=2 key40=1", gate(store, FakeJournal()).describe())
        val refusing = object : VivoFullSizeGate.Store {
            override fun get(key: Int): Int = if (key == VivoFullSizeGate.APP_TYPE_KEY) throw IllegalStateException() else 4
            override fun set(key: Int, value: Int) = Unit
        }
        assertEquals("key1=? key33=4 key40=4", VivoFullSizeGate(refusing, FakeJournal(), 1, { true }, settle = {}).describe())
    }

    @Test fun routesByNameAndTurnsFailuresIntoRefusals() {
        var released = 0
        val ok = object : VendorCameraGate.Gate {
            override fun acquire() = true
            override fun release() {
                released++
            }
        }
        val throwing = object : VendorCameraGate.Gate {
            override fun acquire(): Boolean = throw IllegalStateException("hidden API blocked")
            override fun release(): Unit = throw IllegalStateException("hidden API blocked")
        }
        val gates = VendorCameraGate(mapOf("ok" to ok, "bad" to throwing))
        assertEquals("", gates.describe("ok"))
        assertEquals("", gates.describe("missing"))
        assertTrue(gates.acquire("ok"))
        assertFalse(gates.acquire("missing"))
        assertFalse(gates.acquire("bad"))
        gates.release("ok")
        gates.release("bad")  // Swallowed: a release runs on a native close thread.
        gates.release("missing")
        assertEquals(1, released)
    }
}
