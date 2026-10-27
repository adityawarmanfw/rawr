package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*
import kotlin.test.*
import kotlinx.coroutines.async
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest

@OptIn(ExperimentalCoroutinesApi::class)
class SettingsEditorTest {
    private val defaults = SettingsCatalog.initialState().values

    @Test fun editsBeforeLoadMergeIntoPersistedValues() = runTest {
        val loaded = CompletableDeferred<SettingsValues>()
        val saved = mutableListOf<SettingsValues>()
        val editor = SettingsEditor(defaults, flow { emit(loaded.await()) }, { saved += it }, backgroundScope)
        editor.update { it.copy(selfTimer = SelfTimer.FiveSeconds) }
        loaded.complete(defaults.copy(videoFps = 24))
        runCurrent()
        assertEquals(24, editor.state.value.videoFps)
        assertEquals(SelfTimer.FiveSeconds, editor.state.value.selfTimer)
        assertEquals(listOf(editor.state.value), saved)
    }

    @Test fun slowPersistenceNeverRestoresAnOlderOptimisticSnapshot() = runTest {
        val writerGate = CompletableDeferred<Unit>()
        val saved = mutableListOf<SettingsValues>()
        val editor = SettingsEditor(defaults, flowOf(defaults), {
            saved += it
            if (saved.size == 1) writerGate.await()
        }, backgroundScope)
        runCurrent()
        editor.update { it.copy(videoFps = 24) }
        runCurrent()
        editor.update { it.copy(selfTimer = SelfTimer.ThreeSeconds) }
        editor.update { it.copy(jpegEnabled = false) }
        assertEquals(24, editor.state.value.videoFps)
        assertEquals(SelfTimer.ThreeSeconds, editor.state.value.selfTimer)
        assertFalse(editor.state.value.jpegEnabled)
        writerGate.complete(Unit)
        runCurrent()
        assertEquals(2, saved.size)
        assertEquals(editor.state.value, saved.last())
    }

    @Test fun settingsControllerEditsLatestGlobalValuesDespiteStalePresentation() = runTest {
        val editor = SettingsEditor(defaults, flowOf(defaults), {}, backgroundScope)
        runCurrent()
        val controller = PersistentSettingsController(valuesEditor = editor)
        editor.update { it.copy(videoFps = 24, rawrBaseTone = it.rawrBaseTone.copy(contrast = 23f)) }
        controller.dispatch(SetSelfTimer(SelfTimer.TwoSeconds))
        assertEquals(24, editor.state.value.videoFps)
        assertEquals(23f, editor.state.value.rawrBaseTone.contrast)
        assertEquals(SelfTimer.TwoSeconds, controller.state.value.values.selfTimer)
    }

    @Test fun rendererStyleLocalControllerDoesNotWriteGlobalSettings() = runTest {
        val saved = mutableListOf<SettingsValues>()
        val editor = SettingsEditor(defaults, flowOf(defaults), { saved += it }, backgroundScope)
        runCurrent()
        val local = PersistentSettingsController()
        local.dispatch(SetFilmSimNumericValue(FilmSimNumericParameter.PrintGamma, 1.4f))
        runCurrent()
        assertEquals(defaults, editor.state.value)
        assertTrue(saved.isEmpty())
        assertEquals(1.4f, local.state.value.values.filmSimLook.printGamma)
    }

    @Test fun saveFailureIsReportedWithoutRevertingTheLiveEdit() = runTest {
        val editor = SettingsEditor(defaults, flowOf(defaults), { error("disk full") }, backgroundScope)
        runCurrent()
        editor.update { it.copy(videoFps = 24) }
        runCurrent()
        assertEquals(24, editor.state.value.videoFps)
        assertEquals("disk full", editor.saveFailure.value?.message)
    }

    @Test fun persistenceAcknowledgementWaitsForSave() = runTest {
        val gate = CompletableDeferred<Unit>()
        val editor = SettingsEditor(defaults, flowOf(defaults), { gate.await() }, backgroundScope)
        runCurrent()
        val result = async { editor.updateAndAwaitPersistence { it.copy(videoFps = 24) } }
        runCurrent()
        assertEquals(24, editor.state.value.videoFps)
        assertFalse(result.isCompleted)
        gate.complete(Unit)
        runCurrent()
        result.await()
    }

    @Test fun failedAcknowledgementCanRetryUnchangedValues() = runTest {
        var fail = true
        val editor = SettingsEditor(defaults, flowOf(defaults), {
            if (fail) error("disk full")
        }, backgroundScope)
        runCurrent()
        val first = async { runCatching { editor.updateAndAwaitPersistence { it.copy(videoFps = 24) } } }
        runCurrent()
        assertEquals("disk full", first.await().exceptionOrNull()?.message)
        fail = false
        val retry = async { editor.updateAndAwaitPersistence { it } }
        runCurrent()
        retry.await()
        assertNull(editor.saveFailure.value)
    }

    @Test fun conflatedSaveAcknowledgesRestoreAndProtectsNewerLiveState() = runTest {
        val gate = CompletableDeferred<Unit>()
        val persisted = mutableListOf<Pair<SettingsValues, SettingsValues>>()
        var writes = 0
        val editor = SettingsEditor(defaults, flowOf(defaults), {
            if (++writes == 1) gate.await()
        }, backgroundScope, onPersisted = { saved, live -> persisted += saved to live })
        runCurrent()
        editor.update { it.copy(videoFps = 24) }
        runCurrent()
        val restored = async { editor.updateAndAwaitPersistence { it.copy(locationTagging = true) } }
        runCurrent()
        editor.update { it.copy(jpegEnabled = false) }
        gate.complete(Unit)
        runCurrent()
        restored.await()
        assertEquals(2, writes)
        assertFalse(persisted.first().first.locationTagging)
        assertTrue(persisted.first().second.locationTagging)
        assertFalse(persisted.last().first.jpegEnabled)
    }
}
