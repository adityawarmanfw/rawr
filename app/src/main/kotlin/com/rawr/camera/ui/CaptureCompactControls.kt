package com.rawr.camera.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import com.rawr.camera.ui.icons.outlined.Iso
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.testTag
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.CaptureFilmEvent
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.ExposureParameter
import com.rawr.camera.model.FilmSimQuickState
import com.rawr.camera.model.RenderProfileQuickState

/**
 * V2 compact control strip.
 *
 * The WB · SS · ISO · EV strip floats over the viewfinder bottom
 * ([CompactViewfinderStrip]) so the control area below the monitor holds only
 * the lens selector + shutter row and the shutter sits as high as possible.
 * All four are scrubbable; long-press locks, double-tap returns to auto
 * (EV: double-tap resets to +0.0, long-press is haptic-only).
 */
@Composable
internal fun CompactViewfinderStrip(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    modifier: Modifier = Modifier,
    forceTonemapStrip: Boolean = false,
    renderProfiles: RenderProfileQuickState = RenderProfileQuickState(),
    onSelectRenderProfile: (com.rawr.camera.model.RenderProfileSelection) -> Unit = {},
    filmQuick: FilmSimQuickState? = null,
    onFilmEvent: (CaptureFilmEvent) -> Unit = {}
) {
    // No scrim: legibility on bright and dark scenes comes from drop shadows
    // on every glyph instead of a background wash.
    Column(
        modifier.pointerInput(Unit) {
            // Swallow plain taps so they never fall through to tap-to-focus;
            // the buttons handle their own scrub/long-press/double-tap.
            detectTapGestures(onTap = {})
        },
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // Exactly one render owner is surfaced: Film Sim's preset/look strip
        // when ON, otherwise the regular tonemap profile/PARAMS strip. Both
        // share the same strip language and the same bottom slot geometry.
        // Video mode always shows the tonemap strip (film controls hidden).
        if (state.filmSimEnabled && filmQuick != null && !forceTonemapStrip) {
            FilmStrip(
                quick = filmQuick,
                onSelectPreset = { onFilmEvent(CaptureFilmEvent.SelectPreset(it)) },
                onScrubParam = { key, value -> onFilmEvent(CaptureFilmEvent.ScrubNumeric(
                    com.rawr.camera.settings.model.FilmSimNumericParameter.valueOf(key), value)) },
                onResetParam = { key -> onFilmEvent(CaptureFilmEvent.ResetNumeric(
                    com.rawr.camera.settings.model.FilmSimNumericParameter.valueOf(key))) },
                onScrubDiscrete = { key, index -> onFilmEvent(CaptureFilmEvent.ScrubDiscrete(
                    com.rawr.camera.settings.model.FilmSimDiscreteField.valueOf(key), index)) },
                onToggleFlag = { key, enabled -> onFilmEvent(CaptureFilmEvent.SetFlag(
                    com.rawr.camera.settings.model.FilmSimFlag.valueOf(key), enabled)) },
                modifier = Modifier.fillMaxWidth()
            )
        } else if (!state.filmSimEnabled || forceTonemapStrip) {
            TonemapStrip(
                profiles = renderProfiles,
                state = state,
                onSelectProfile = onSelectRenderProfile,
                dispatch = dispatch,
                modifier = Modifier.fillMaxWidth()
            )
        }
        CompactParamRow(
            state = state,
            dispatch = dispatch,
            modifier = Modifier.fillMaxWidth()
        )
    }
}

@Composable
internal fun CompactParamRow(state: CaptureUiState, dispatch: CaptureDispatch, modifier: Modifier = Modifier) {
    val weights = CaptureDimens.CompactParamColumnWeights
    Row(
        modifier
            .fillMaxWidth()
            .height(CaptureDimens.CompactParamTwoLineHeight)
            .padding(horizontal = CaptureDimens.CompactParamRowEdgeInset)
            .testTag(CaptureTestTags.COMPACT_PARAM_ROW),
        horizontalArrangement = Arrangement.spacedBy(CaptureDimens.ControlGap)
    ) {
        CompactExposureButton(ExposureParameter.Iso, state, dispatch, Modifier.weight(weights[0]).fillMaxHeight())
        CompactExposureButton(ExposureParameter.Shutter, state, dispatch, Modifier.weight(weights[1]).fillMaxHeight())
        CompactExposureButton(ExposureParameter.Ev, state, dispatch, Modifier.weight(weights[2]).fillMaxHeight())
        CompactWbButton(state, dispatch, Modifier.weight(weights[3]).fillMaxHeight())
        CompactFocusButton(state, dispatch, Modifier.weight(weights[4]).fillMaxHeight())
    }
}
