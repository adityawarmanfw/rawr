package com.rawr.camera.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.ScrubTone
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.RenderProfileQuickState
import com.rawr.camera.model.TonemapCatalog
import com.rawr.camera.model.TonemapParam
import kotlin.math.roundToInt

/**
 * Regular-tonemap (Film Sim OFF) companion to [FilmStrip].
 *
 * Surfaced identically: a collapsed profile/PARAMS header, a profile picker,
 * and a single PARAMS row exposing every [TonemapCatalog] tone slider. The
 * catalog owns the parameter set, order, labels and defaults, so this file
 * only presents them.
 */
private enum class TonemapStripMode { Collapsed, Profiles, Params }

@Composable
internal fun TonemapStrip(
    profiles: RenderProfileQuickState,
    state: CaptureUiState,
    onSelectProfile: (com.rawr.camera.model.RenderProfileSelection) -> Unit,
    dispatch: CaptureDispatch,
    modifier: Modifier = Modifier
) {
    var mode by remember(profiles.log) { mutableStateOf(TonemapStripMode.Collapsed) }
    val profileLabel = profiles.options.firstOrNull { it.id == profiles.selectedId }?.label ?: "RAWR NTRL"
    // Render exposure counts too: the manual-mode EV button edits it from capture.
    val modified = !profiles.log && (state.renderExposureTenths != 0 ||
        TonemapCatalog.tone.any { !TonemapCatalog.isDefault(state, requireNotNull(it.param)) })
    Column(
        modifier.testTag(CaptureTestTags.TONEMAP_STRIP),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        when (mode) {
            TonemapStripMode.Collapsed -> TonemapCollapsedRow(
                profileLabel = profileLabel,
                modified = modified,
                paramsEnabled = !profiles.log,
                onOpenProfiles = { mode = TonemapStripMode.Profiles },
                onOpenParams = { mode = TonemapStripMode.Params }
            )

            TonemapStripMode.Profiles -> TonemapProfilesRow(
                profiles = profiles,
                onBack = { mode = TonemapStripMode.Collapsed },
                onSelect = onSelectProfile,
                onOpenParams = { mode = TonemapStripMode.Params }
            )

            TonemapStripMode.Params -> TonemapParamsRow(
                state = state,
                profileLabel = profileLabel,
                modified = modified,
                onBack = { mode = TonemapStripMode.Collapsed },
                onOpenProfiles = { mode = TonemapStripMode.Profiles },
                dispatch = dispatch
            )
        }
    }
}

@Composable
private fun TonemapCollapsedRow(
    profileLabel: String,
    modified: Boolean,
    paramsEnabled: Boolean,
    onOpenProfiles: () -> Unit,
    onOpenParams: () -> Unit
) {
    val haptics = LocalCaptureHaptics.current
    Row(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 2.dp, end = StripDimens.TrailingInset),
        horizontalArrangement = Arrangement.spacedBy(28.dp, Alignment.End),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            Modifier
                .widthIn(max = 180.dp)
                .height(CaptureDimens.CompactParamRowHeight)
                .testTag(CaptureTestTags.TONEMAP_STRIP_PROFILE)
                .captureClickable(shape = CaptureTextRippleShape) {
                    haptics.selection()
                    onOpenProfiles()
                }.semantics {
                    contentDescription = "Render profile $profileLabel. Tap to choose profile."
                    onClick {
                        haptics.selection()
                        onOpenProfiles()
                        true
                    }
                },
            contentAlignment = Alignment.CenterEnd
        ) {
            Text(
                if (modified) "$profileLabel •" else profileLabel,
                color = if (modified) CaptureColors.AccentSoft else Color.White.copy(alpha = .95f),
                fontFamily = CaptureMono,
                fontWeight = FontWeight.SemiBold,
                fontSize = 10.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = ViewfinderTextStyle
            )
        }
        if (paramsEnabled) StripParamsEntry(onOpenParams)
    }
}

@Composable
private fun TonemapProfilesRow(
    profiles: RenderProfileQuickState,
    onBack: () -> Unit,
    onSelect: (com.rawr.camera.model.RenderProfileSelection) -> Unit,
    onOpenParams: () -> Unit
) {
    val haptics = LocalCaptureHaptics.current
    Row(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 2.dp, end = StripDimens.TrailingInset),
        verticalAlignment = Alignment.CenterVertically
    ) {
        StripBackButton(onBack)
        LazyRow(
            Modifier
                .weight(1f)
                .height(CaptureDimens.CompactParamRowHeight),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(StripDimens.ParamSpacing, Alignment.End)
        ) {
            items(profiles.options, key = { it.id.key }) { option ->
                val isSelected = option.id == profiles.selectedId
                Box(
                    Modifier
                        .height(CaptureDimens.CompactParamRowHeight)
                        .testTag(CaptureTestTags.renderProfile(option.id.key))
                        .captureClickable(shape = CaptureTextRippleShape, enabled = !profiles.locked) {
                            haptics.selection()
                            onSelect(option.id)
                        }.semantics {
                            contentDescription = "Render profile ${option.label}" +
                                if (isSelected) ", active" else ""
                            role = Role.RadioButton
                            selected = isSelected
                            onClick {
                                if (profiles.locked) return@onClick false
                                haptics.selection()
                                onSelect(option.id)
                                true
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        option.label,
                        color = if (isSelected) CaptureColors.Accent else Color.White.copy(alpha = .62f),
                        fontFamily = CaptureMono,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 10.sp,
                        maxLines = 1,
                        style = ViewfinderTextStyle
                    )
                }
            }
        }
        if (!profiles.log) StripParamsEntry(onOpenParams)
    }
}

@Composable
private fun TonemapParamsRow(
    state: CaptureUiState,
    profileLabel: String,
    modified: Boolean,
    onBack: () -> Unit,
    onOpenProfiles: () -> Unit,
    dispatch: CaptureDispatch
) {
    val haptics = LocalCaptureHaptics.current
    Row(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 2.dp, end = StripDimens.TrailingInset),
        verticalAlignment = Alignment.CenterVertically
    ) {
        StripBackButton(onBack)
        // Parked at the thumb: BLK starts at the right edge with a peek of
        // the next cell; the rest scrolls in from the right, empty space left.
        BoxWithConstraints(
            Modifier
                .weight(1f)
                .height(StripDimens.ParamRowHeight)
        ) {
            LazyRow(
                Modifier
                    .fillMaxSize()
                    .testTag(CaptureTestTags.TONEMAP_STRIP_PARAMS),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(StripDimens.ParamSpacing, Alignment.End),
                contentPadding = PaddingValues(start = parkedParamsPadding(maxWidth))
            ) {
                items(TonemapCatalog.tone, key = { it.jsonKey }) { descriptor ->
                    TonemapParamItem(descriptor, state, dispatch)
                }
            }
        }
        Box(
            Modifier
                .widthIn(max = 120.dp)
                .height(CaptureDimens.CompactParamRowHeight)
                .captureClickable(shape = CaptureTextRippleShape) {
                    haptics.selection()
                    onOpenProfiles()
                }.semantics {
                    contentDescription = "Render profile $profileLabel. Tap to choose profile."
                    onClick {
                        haptics.selection()
                        onOpenProfiles()
                        true
                    }
                },
            contentAlignment = Alignment.CenterEnd
        ) {
            Text(
                if (modified) "$profileLabel •" else profileLabel,
                color = if (modified) CaptureColors.AccentSoft else Color.White.copy(alpha = .95f),
                fontFamily = CaptureMono,
                fontWeight = FontWeight.SemiBold,
                fontSize = 10.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = ViewfinderTextStyle,
                modifier = Modifier.padding(start = 8.dp)
            )
        }
    }
}

@Composable
private fun TonemapParamItem(
    descriptor: TonemapParam,
    state: CaptureUiState,
    dispatch: CaptureDispatch
) {
    val parameter = requireNotNull(descriptor.param)
    val committed = TonemapCatalog.valueOf(state, parameter)
    val latestCommitted by rememberUpdatedState(committed)
    // Single source for the ScrubTone delta base. Re-seeded only at drag
    // start (mirrors ToneScrubSurface): re-seeding on every committed change
    // would reset the base mid-drag when the async persist/sync round-trip
    // lands, inflating/duplicating steps under fast scrub.
    var gesture by remember(descriptor.jsonKey) { mutableIntStateOf(committed) }

    StripNumericItem(
        key = descriptor.jsonKey,
        shortLabel = descriptor.shortLabel,
        value = committed.toFloat(),
        display = signedTone(committed),
        isDefault = committed == descriptor.defaultValue.roundToInt(),
        defaultValue = descriptor.defaultValue,
        min = descriptor.minimum,
        max = descriptor.maximum,
        step = descriptor.step,
        decimals = descriptor.decimals,
        testTag = CaptureTestTags.tonemapParam(descriptor.jsonKey),
        onDragStarted = { gesture = latestCommitted },
        onScrub = { nextFloat ->
            val next = nextFloat.roundToInt().coerceIn(descriptor.minimum.roundToInt(), descriptor.maximum.roundToInt())
            val delta = next - gesture
            if (delta != 0) {
                gesture = next
                dispatch(ScrubTone(parameter, delta))
            }
        },
        onReset = {
            val current = latestCommitted
            gesture = descriptor.defaultValue.roundToInt()
            if (current != descriptor.defaultValue.roundToInt()) {
                dispatch(ScrubTone(parameter, descriptor.defaultValue.roundToInt() - current))
            }
        },
        format = { signedTone(it.roundToInt()) }
    )
}

private fun signedTone(value: Int): String = if (value >= 0) "+$value" else "$value"
