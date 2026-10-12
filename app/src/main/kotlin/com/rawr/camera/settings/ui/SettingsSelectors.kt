package com.rawr.camera.settings.ui

import androidx.compose.runtime.Composable
import com.rawr.camera.settings.architecture.SetChoice
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.settings.model.ChoiceCandidate
import com.rawr.camera.settings.model.ChoiceSelectorKind
import com.rawr.camera.settings.model.FilmSimDiscreteField
import com.rawr.camera.settings.model.FilmStocks
import com.rawr.camera.settings.model.options
import com.rawr.camera.settings.model.SettingsUiState

internal fun selectorTitle(kind: ChoiceSelectorKind): String = when (kind) {
    ChoiceSelectorKind.SaveLocation -> "Save Location"
    ChoiceSelectorKind.FalseColorPreset -> "False Color Preset"
    ChoiceSelectorKind.PeakingSensitivity -> "Focus Peaking Sensitivity"
    ChoiceSelectorKind.OutputColorSpace -> "Output Color Space"
    ChoiceSelectorKind.TransferFunction -> "Transfer Function"
    ChoiceSelectorKind.JpegChromaSubsampling -> "Chroma Subsampling"
    ChoiceSelectorKind.DngCompression -> "DNG Compression"
    ChoiceSelectorKind.MaxPostGain -> "Boost Limit"
    ChoiceSelectorKind.AutoMinFps -> "Auto Minimum FPS"
    ChoiceSelectorKind.FilmOutputSpace -> "Film Output Color Space"
}

@Composable
internal fun ChoiceSelectorScreen(state: SettingsUiState, kind: ChoiceSelectorKind, dispatch: SettingsDispatch) {
    val selector = selectorModel(state, kind)
    SettingsPageContainer(testTag = SettingsTestTags.selectorRoot(kind.name)) {
        SettingsGroup {
            selector.choices.forEachIndexed { index, choice ->
                SettingsSelectionRow(
                    title = choice.label,
                    selected = choice.id == selector.selectedId,
                    testTag = SettingsTestTags.selectorOption(kind.name, SettingsTestTags.slug(choice.id)),
                    onClick = { dispatch.invoke(SetChoice(kind, choice.id)) }
                )
                if (index != selector.choices.lastIndex) SettingDivider()
            }
        }
    }
}

private data class SelectorModel(val choices: List<ChoiceCandidate>, val selectedId: String)

private fun selectorModel(state: SettingsUiState, kind: ChoiceSelectorKind): SelectorModel = when (kind) {
    ChoiceSelectorKind.SaveLocation -> {
        SelectorModel(state.capabilities.saveLocationChoices, state.values.saveLocationId)
    }

    ChoiceSelectorKind.FalseColorPreset -> {
        SelectorModel(
            state.capabilities.falseColorPresets.map { ChoiceCandidate(it.id, it.displayName, it.description) },
            state.values.falseColorPresetId
        )
    }

    ChoiceSelectorKind.PeakingSensitivity -> {
        SelectorModel(state.capabilities.peakingSensitivityChoices, state.values.peakingSensitivityId)
    }

    ChoiceSelectorKind.OutputColorSpace -> {
        SelectorModel(state.capabilities.outputColorSpaces, state.values.imageTone.outputColorSpaceId)
    }

    ChoiceSelectorKind.TransferFunction -> {
        SelectorModel(state.capabilities.transferFunctions, state.values.imageTone.transferFunctionId)
    }

    ChoiceSelectorKind.JpegChromaSubsampling -> {
        SelectorModel(
            state.capabilities.jpegChromaSubsamplingChoices,
            state.values.imageTone.jpegChromaSubsamplingId
        )
    }

    ChoiceSelectorKind.DngCompression -> {
        SelectorModel(
            state.capabilities.dngCompressionChoices,
            state.values.dngCompressionId
        )
    }

    ChoiceSelectorKind.MaxPostGain -> {
        SelectorModel(state.capabilities.maxPostGainChoices, state.values.maxPostGainId)
    }

    ChoiceSelectorKind.AutoMinFps -> {
        SelectorModel(state.capabilities.autoMinFpsChoices, state.values.autoMinFpsId)
    }

    ChoiceSelectorKind.FilmOutputSpace -> {
        // Display-referred encodings only: the full 26-space table exists in
        // the engine, but log/linear spaces are not useful on a phone panel.
        // Labels come from the catalog; ids stay stored indices (see the
        // FilmOutputSpace reducer branch).
        val options = FilmSimDiscreteField.OutputColorSpace.options()
        val supported = FilmStocks.previewSupportedOutputSpaces.sorted()
        SelectorModel(
            supported.mapIndexed { position, index ->
                ChoiceCandidate(id = index.toString(), label = options[position])
            },
            state.values.filmSimLook.outputColorSpace.toString()
        )
    }
}
