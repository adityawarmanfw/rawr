package com.rawr.camera.integration

import com.rawr.camera.model.CaptureOutputFormat
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.ExperimentalCoroutinesApi

@OptIn(ExperimentalCoroutinesApi::class)
class StillCaptureDispatchQueueTest {
    @Test
    fun foregroundOwnershipRejectsObsoleteRequestsWithoutReorderingAdmittedOnes() = runTest {
        var token = Any()
        val admitted = mutableListOf<CaptureOutputFormat>()
        val acquired = mutableListOf<CaptureOutputFormat>()
        var rejected = 0
        val queue = StillCaptureDispatchQueue(backgroundScope, admitted::add, acquired::add,
            onRejected = { _, _ -> rejected++ })
        val formats = List(8) { CaptureOutputFormat.entries[it % 3] }
        for (format in formats) {
            val pressed = token
            assertTrue(queue.enqueue(format) { token === pressed }, "Admission is synchronous at the press")
            val departed = token
            token = Any()
            assertFalse(queue.enqueue(format) { token === departed })
        }
        assertEquals(8, rejected)
        assertEquals(formats, admitted)
        assertTrue(acquired.isEmpty(), "Shutter caller must not execute provider/native work")
        runCurrent()
        assertEquals(formats, acquired)
        var published = 0
        queue.poll { published = acquired.size }
        assertEquals(8, published)
    }

    @Test
    fun slowPreparationDoesNotBlockFurtherPressesOrLetCompletionOvertakeRegistration() {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val entered = CountDownLatch(1)
        val finishPreparation = CountDownLatch(1)
        val captured = CountDownLatch(8)
        val pollFinished = CountDownLatch(1)
        try {
            val queue = StillCaptureDispatchQueue(scope, {}, {
                entered.countDown()
                check(finishPreparation.await(5, TimeUnit.SECONDS))
                captured.countDown()
            }, { _, _ -> })
            queue.enqueue(CaptureOutputFormat.DngAndJpeg)
            assertTrue(entered.await(5, TimeUnit.SECONDS))
            repeat(7) { queue.enqueue(CaptureOutputFormat.DngAndJpeg) }
            scope.launch { queue.poll { pollFinished.countDown() } }
            assertTrue(!pollFinished.await(100, TimeUnit.MILLISECONDS))
            finishPreparation.countDown()
            assertTrue(captured.await(5, TimeUnit.SECONDS))
            assertTrue(pollFinished.await(5, TimeUnit.SECONDS))
        } finally {
            finishPreparation.countDown()
            scope.cancel()
        }
    }
}
