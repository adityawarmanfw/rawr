package com.rawr.camera.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rawr.camera.architecture.CaptureDispatch
import com.rawr.camera.architecture.CaptureFilmEvent
import com.rawr.camera.architecture.CaptureTransitions
import com.rawr.camera.architecture.LockWhiteBalance
import com.rawr.camera.architecture.ScrubTone
import com.rawr.camera.architecture.SetExposureCandidate
import com.rawr.camera.architecture.SetExposureMode
import com.rawr.camera.architecture.SetFocusMode
import com.rawr.camera.architecture.SetManualFocus
import com.rawr.camera.architecture.SetWhiteBalanceMode
import com.rawr.camera.architecture.ToggleFilmSim
import com.rawr.camera.model.CaptureUiState
import com.rawr.camera.model.ExposureMode
import com.rawr.camera.model.ExposureParameter
import com.rawr.camera.model.FilmSimQuickState
import com.rawr.camera.model.FilmStripItem
import com.rawr.camera.model.FocusMode
import com.rawr.camera.model.RenderProfileQuickState
import com.rawr.camera.model.RenderProfileSelection
import com.rawr.camera.model.TonemapCatalog
import com.rawr.camera.model.WhiteBalanceMode
import com.rawr.camera.model.capabilityFor
import com.rawr.camera.model.chipLabel
import com.rawr.camera.model.mfNormalizedFromDiopters
import com.rawr.camera.model.nextExposureMode
import com.rawr.camera.model.requestedCandidateIdFor
import com.rawr.camera.model.zeroEvCandidateId
import com.rawr.camera.settings.model.FilmSimNumericParameter
import kotlin.math.roundToInt

/** Which value the Pro layout's ruler is currently editing. */
internal enum class ProTarget(val title: String) {
    Wb("WB"),
    Shutter("SHUTTER"),
    Iso("ISO"),
    Ev("EV"),
    Focus("FOCUS")
}

/** Full-screen sheets opened from the shutter bar. */
internal enum class ProSheet { None, Filters, Params }

private val PanelShape = RoundedCornerShape(22.dp)
private val ChipShape = RoundedCornerShape(14.dp)

/**
 * The Pro layout's viewfinder overlay: one ruler for whichever value is selected, and a single row of tiles that
 * show every live value. Tap a tile to edit it on the ruler, tap it again to close. Nothing else sits over the image.
 */
@Composable
internal fun ProViewfinderOverlay(state: CaptureUiState, dispatch: CaptureDispatch, modifier: Modifier = Modifier) {
    var target by rememberSaveable { mutableStateOf<ProTarget?>(null) }
    val haptics = LocalCaptureHaptics.current
    // With a ruler open, Back closes it rather than leaving the app.
    BackHandler(enabled = target != null) { target = null }
    Column(
        modifier.pointerInput(Unit) {
            // Taps on the controls must not fall through to tap-to-focus.
            detectTapGestures(onTap = {})
        },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        target?.let { current ->
            Column(
                Modifier
                    .fillMaxWidth()
                    .clip(PanelShape)
                    .background(Color.Black.copy(alpha = .58f))
                    .padding(top = 8.dp, bottom = 6.dp)
            ) {
                when (current) {
                    ProTarget.Shutter -> ExposureDial(state, dispatch, ExposureParameter.Shutter)
                    ProTarget.Iso -> ExposureDial(state, dispatch, ExposureParameter.Iso)
                    ProTarget.Ev -> ExposureDial(state, dispatch, ExposureParameter.Ev)
                    ProTarget.Wb -> WhiteBalanceDial(state, dispatch)
                    ProTarget.Focus -> FocusDial(state, dispatch)
                }
            }
        }
        Row(
            Modifier
                .fillMaxWidth()
                .clip(PanelShape)
                .background(Color.Black.copy(alpha = .5f))
                .padding(horizontal = 4.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(2.dp)
        ) {
            val mode = state.exposureControl.mode
            ProTile(
                title = "MODE",
                value = mode.chipLabel,
                selected = false,
                modifier = Modifier.weight(1.25f)
            ) {
                haptics.selection()
                dispatch(SetExposureMode(nextExposureMode(mode, state.capabilities.supportedExposureModes)))
            }
            ProTile("WB", whiteBalanceTileValue(state), target == ProTarget.Wb, Modifier.weight(1f)) {
                haptics.selection()
                target = if (target == ProTarget.Wb) null else ProTarget.Wb
            }
            ProTile("SS", exposureTileValue(state, ExposureParameter.Shutter), target == ProTarget.Shutter, Modifier.weight(1f)) {
                haptics.selection()
                target = if (target == ProTarget.Shutter) null else ProTarget.Shutter
            }
            ProTile("ISO", exposureTileValue(state, ExposureParameter.Iso), target == ProTarget.Iso, Modifier.weight(1f)) {
                haptics.selection()
                target = if (target == ProTarget.Iso) null else ProTarget.Iso
            }
            ProTile("EV", exposureTileValue(state, ExposureParameter.Ev), target == ProTarget.Ev, Modifier.weight(1f)) {
                haptics.selection()
                target = if (target == ProTarget.Ev) null else ProTarget.Ev
            }
            ProTile("FOCUS", focusTileValue(state), target == ProTarget.Focus, Modifier.weight(1.1f)) {
                haptics.selection()
                target = if (target == ProTarget.Focus) null else ProTarget.Focus
            }
        }
    }
}

@Composable
private fun ProTile(
    title: String,
    value: String,
    selected: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit
) {
    Column(
        modifier
            .heightIn(min = 48.dp)
            .clip(ChipShape)
            .background(if (selected) CaptureColors.Accent.copy(alpha = .22f) else Color.Transparent)
            .clickable(onClick = onClick)
            .semantics { contentDescription = "$title $value" }
            .padding(vertical = 5.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            value,
            color = if (selected) CaptureColors.Accent else Color.White,
            fontFamily = CaptureMono,
            fontWeight = FontWeight.Bold,
            fontSize = 14.sp,
            maxLines = 1,
            softWrap = false,
            textAlign = TextAlign.Center
        )
        Text(
            title,
            color = if (selected) CaptureColors.Accent.copy(alpha = .8f) else CaptureColors.Muted,
            fontFamily = CaptureMono,
            fontWeight = FontWeight.Bold,
            fontSize = 9.sp,
            maxLines = 1
        )
    }
}

private fun whiteBalanceTileValue(state: CaptureUiState): String = when (state.whiteBalanceMode) {
    WhiteBalanceMode.Auto -> "AUTO"
    WhiteBalanceMode.ManualTempTint -> "${state.whiteBalanceTemperatureK}K"
    else -> state.whiteBalanceMode.shortLabel
}

private fun exposureTileValue(state: CaptureUiState, parameter: ExposureParameter): String {
    val capability = state.capabilities.capabilityFor(parameter)
    val requested = capability.candidate(state.requestedCandidateIdFor(parameter))?.displayLabel ?: "-"
    val applied = when (parameter) {
        ExposureParameter.Shutter -> state.exposureApplied.shutter
        ExposureParameter.Iso -> state.exposureApplied.iso
        ExposureParameter.Ev -> state.exposureApplied.evOrMeter
    }
    val locked = CaptureTransitions.isParameterLocked(state, parameter)
    return when {
        parameter == ExposureParameter.Ev && state.exposureControl.mode == ExposureMode.Manual ->
            applied?.displayLabel ?: "-"
        parameter == ExposureParameter.Ev -> requested
        locked -> requested
        else -> applied?.displayLabel ?: requested
    }
}

private fun focusTileValue(state: CaptureUiState): String = when (state.focus.mode) {
    FocusMode.Af -> "AF"
    FocusMode.AfLock -> "AF LOCK"
    FocusMode.Mf -> manualFocusLabel(state, state.focus.mfNormalized)
}

/** Distance label for a manual-focus position; the native mapping is diopters = (1 - normalized) * minimum. */
private fun manualFocusLabel(state: CaptureUiState, normalized: Float): String {
    val minimum = state.capabilities.manualFocus.minimumFocusDistance
    if (minimum <= 0f) return "${(normalized * 100).roundToInt()}%"
    val diopters = minimum * (1f - normalized).coerceIn(0f, 1f)
    if (diopters < .06f) return "INF"
    val meters = 1f / diopters
    return if (meters < 1f) "%.2fm".format(meters) else "%.1fm".format(meters)
}

@Composable
private fun DialHeader(title: String, value: String, trailing: @Composable () -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(Modifier.weight(1f).widthIn(min = 56.dp), contentAlignment = Alignment.CenterStart) {
            Text(
                title,
                color = CaptureColors.Muted,
                fontFamily = CaptureMono,
                fontWeight = FontWeight.Bold,
                fontSize = 11.sp
            )
        }
        Text(
            value,
            color = Color.White,
            fontFamily = CaptureMono,
            fontWeight = FontWeight.Bold,
            fontSize = 24.sp,
            maxLines = 1
        )
        Box(Modifier.weight(1f), contentAlignment = Alignment.CenterEnd) { trailing() }
    }
}

@Composable
private fun SmallPill(text: String, selected: Boolean, enabled: Boolean = true, onClick: () -> Unit) {
    Box(
        Modifier
            .clip(ChipShape)
            .background(if (selected) CaptureColors.Accent.copy(alpha = .3f) else Color.White.copy(alpha = .09f))
            .then(if (enabled) Modifier.clickable(onClick = onClick) else Modifier)
            .padding(horizontal = 12.dp, vertical = 6.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text,
            color = if (selected) CaptureColors.Accent else Color.White.copy(alpha = if (enabled) .85f else .35f),
            fontFamily = CaptureMono,
            fontWeight = FontWeight.Bold,
            fontSize = 11.sp,
            maxLines = 1
        )
    }
}

@Composable
private fun ExposureDial(state: CaptureUiState, dispatch: CaptureDispatch, parameter: ExposureParameter) {
    val capability = state.capabilities.capabilityFor(parameter)
    val haptics = LocalCaptureHaptics.current
    val mode = state.exposureControl.mode
    val locked = CaptureTransitions.isParameterLocked(state, parameter)
    val isEv = parameter == ExposureParameter.Ev
    val meterOnly = isEv && mode == ExposureMode.Manual
    val applied = when (parameter) {
        ExposureParameter.Shutter -> state.exposureApplied.shutter
        ExposureParameter.Iso -> state.exposureApplied.iso
        ExposureParameter.Ev -> state.exposureApplied.evOrMeter
    }
    // While the axis is automatic the ruler follows what the camera is doing; once locked it is the request.
    val requestedId = state.requestedCandidateIdFor(parameter)
    val seedId =
        if (locked || isEv) requestedId else applied?.sourceCandidateId?.takeIf { capability.candidate(it) != null } ?: requestedId
    val selectedIndex = capability.indexOf(seedId).coerceAtLeast(0)
    val label = capability.candidates.getOrNull(selectedIndex)?.displayLabel.orEmpty()
    val title = when (parameter) {
        ExposureParameter.Shutter -> "SHUTTER SPEED"
        ExposureParameter.Iso -> "ISO"
        ExposureParameter.Ev -> if (meterOnly) "EXPOSURE METER" else "EXPOSURE COMPENSATION"
    }
    DialHeader(title, if (meterOnly) applied?.displayLabel ?: "-" else label) {
        when {
            meterOnly -> Unit
            isEv -> SmallPill("RESET 0", selected = false) {
                haptics.selection()
                dispatch(SetExposureCandidate(parameter, capability.zeroEvCandidateId()))
            }
            else -> SmallPill(if (locked) "AUTO" else "AUTO ON", selected = !locked, enabled = locked) {
                CaptureTransitions.unlockModeFor(parameter, mode)?.let {
                    haptics.selection()
                    dispatch(SetExposureMode(it))
                }
            }
        }
    }
    if (!meterOnly) {
        ValueRuler(
            stepCount = capability.candidates.size,
            selectedIndex = selectedIndex,
            onIndexChange = { index ->
                if (!locked && !isEv) {
                    CaptureTransitions.lockModeFor(parameter, mode)?.let { dispatch(SetExposureMode(it)) }
                }
                dispatch(SetExposureCandidate(parameter, capability.candidates[index].id))
            },
            isMajor = { capability.candidates[it].id in capability.fullStopAnchorIds },
            labelAt = { capability.candidates[it].displayLabel },
            modifier = Modifier.padding(top = 4.dp)
        )
    }
}

@Composable
private fun WhiteBalanceDial(state: CaptureUiState, dispatch: CaptureDispatch) {
    val haptics = LocalCaptureHaptics.current
    val manual = state.whiteBalanceMode == WhiteBalanceMode.ManualTempTint
    val liveK = state.autoWhiteBalanceTemperatureK ?: state.whiteBalanceTemperatureK
    val liveTint = state.autoWhiteBalanceTint ?: state.whiteBalanceTint
    val kelvin = if (manual) state.whiteBalanceTemperatureK else liveK
    val tint = if (manual) state.whiteBalanceTint else liveTint
    DialHeader("WHITE BALANCE", if (manual) "${kelvin}K" else whiteBalanceTileValue(state)) {
        Text(
            "tint ${if (tint > 0) "+" else ""}$tint",
            color = CaptureColors.Muted,
            fontFamily = CaptureMono,
            fontSize = 11.sp
        )
    }
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 10.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        val supported = state.capabilities.supportedWhiteBalanceModes
        WhiteBalanceMode.entries
            .filter { it != WhiteBalanceMode.ManualTempTint && it in supported }
            .forEach { mode ->
                SmallPill(mode.shortLabel, selected = state.whiteBalanceMode == mode) {
                    haptics.selection()
                    dispatch(SetWhiteBalanceMode(mode))
                }
            }
        if (state.capabilities.manualWhiteBalanceSupported) {
            SmallPill("MANUAL", selected = manual) {
                haptics.selection()
                dispatch(LockWhiteBalance(kelvin, tint))
            }
        }
    }
    if (state.capabilities.manualWhiteBalanceSupported) {
        val steps = (WhiteBalanceMode.TEMP_MAX_K - WhiteBalanceMode.TEMP_MIN_K) / WB_STEP_K + 1
        ValueRuler(
            stepCount = steps,
            selectedIndex = ((kelvin - WhiteBalanceMode.TEMP_MIN_K) / WB_STEP_K.toFloat()).roundToInt(),
            onIndexChange = { index ->
                dispatch(LockWhiteBalance(WhiteBalanceMode.TEMP_MIN_K + index * WB_STEP_K, tint))
            },
            isMajor = { (WhiteBalanceMode.TEMP_MIN_K + it * WB_STEP_K) % 1000 == 0 },
            labelAt = { "${(WhiteBalanceMode.TEMP_MIN_K + it * WB_STEP_K) / 1000}k" }
        )
        val tintSteps = WhiteBalanceMode.TINT_MAX - WhiteBalanceMode.TINT_MIN + 1
        ValueRuler(
            stepCount = tintSteps,
            selectedIndex = tint - WhiteBalanceMode.TINT_MIN,
            onIndexChange = { index ->
                dispatch(LockWhiteBalance(kelvin, WhiteBalanceMode.TINT_MIN + index))
            },
            isMajor = { (WhiteBalanceMode.TINT_MIN + it) % 10 == 0 },
            labelAt = {
                val value = WhiteBalanceMode.TINT_MIN + it
                if (value > 0) "+$value" else "$value"
            },
            modifier = Modifier.padding(top = 2.dp)
        )
    }
}

private const val WB_STEP_K = 100

@Composable
private fun FocusDial(state: CaptureUiState, dispatch: CaptureDispatch) {
    val haptics = LocalCaptureHaptics.current
    val capability = state.capabilities.manualFocus
    val mf = state.focus.mode == FocusMode.Mf
    // In MF the ruler is the request; otherwise it shows where the lens actually is.
    val position =
        if (mf) {
            state.focus.mfNormalized
        } else {
            state.focus.appliedFocusDiopters
                ?.let { mfNormalizedFromDiopters(it, capability.minimumFocusDistance) }
                ?: state.focus.mfNormalized
        }
    DialHeader("FOCUS", if (mf) manualFocusLabel(state, position) else focusTileValue(state)) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            SmallPill("AF", selected = state.focus.mode == FocusMode.Af) {
                haptics.selection()
                dispatch(SetFocusMode(FocusMode.Af))
            }
            SmallPill("LOCK", selected = state.focus.mode == FocusMode.AfLock) {
                haptics.selection()
                dispatch(SetFocusMode(FocusMode.AfLock))
            }
            SmallPill("MF", selected = mf, enabled = capability.supported) {
                haptics.selection()
                dispatch(SetFocusMode(FocusMode.Mf))
            }
        }
    }
    if (capability.supported) {
        ValueRuler(
            stepCount = FOCUS_STEPS + 1,
            selectedIndex = (position * FOCUS_STEPS).roundToInt(),
            onIndexChange = { index -> dispatch(SetManualFocus(index / FOCUS_STEPS.toFloat())) },
            isMajor = { it % 10 == 0 },
            labelAt = { manualFocusLabel(state, it / FOCUS_STEPS.toFloat()) },
            modifier = Modifier.padding(top = 4.dp)
        )
    }
}

private const val FOCUS_STEPS = 100

/** Right side of the shutter row in the Pro layout: the two dedicated entry points. */
@Composable
internal fun ProSideButtons(
    state: CaptureUiState,
    renderProfiles: RenderProfileQuickState,
    onOpenFilters: () -> Unit,
    onOpenParams: () -> Unit,
    modifier: Modifier = Modifier
) {
    val lookActive =
        state.filmSimEnabled ||
            renderProfiles.selectedId != RenderProfileSelection.BuiltIn(com.rawr.camera.settings.model.ColorRenderProfile.RawrBase)
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        SideButton("FILTERS", active = lookActive, onClick = onOpenFilters)
        SideButton("PARAMS", active = false, onClick = onOpenParams)
    }
}

/** Multiframe (MERGED) toggle for the lens row: tap to switch, long press for its settings. */
@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
@Composable
internal fun ProMergedToggle(state: CaptureUiState, onToggle: () -> Unit, onOpenSettings: () -> Unit, modifier: Modifier = Modifier) {
    val haptics = LocalCaptureHaptics.current
    val on = state.experimentalMultiframeEnabled
    Box(
        modifier
            .clip(ChipShape)
            .border(1.dp, if (on) CaptureColors.Accent else Color.White.copy(alpha = .22f), ChipShape)
            .combinedClickable(
                onClick = {
                    haptics.selection()
                    onToggle()
                },
                onLongClick = {
                    haptics.longPress()
                    onOpenSettings()
                },
                onLongClickLabel = "Multiframe settings"
            )
            .semantics {
                contentDescription = if (on) "Multiframe on. Tap to turn off. Long press for settings." else
                    "Multiframe off. Tap to turn on. Long press for settings."
            }
            .padding(horizontal = 10.dp, vertical = 4.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            "MERGED",
            color = if (on) CaptureColors.Accent else Color.White.copy(alpha = .7f),
            fontFamily = CaptureMono,
            fontWeight = FontWeight.Bold,
            fontSize = 10.sp
        )
    }
}

@Composable
private fun SideButton(text: String, active: Boolean, onClick: () -> Unit) {
    val haptics = LocalCaptureHaptics.current
    Box(
        Modifier
            .widthIn(min = 84.dp)
            .heightIn(min = 34.dp)
            .clip(ChipShape)
            .border(1.dp, if (active) CaptureColors.Accent else Color.White.copy(alpha = .25f), ChipShape)
            .clickable {
                haptics.selection()
                onClick()
            }
            .padding(horizontal = 10.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text,
            color = if (active) CaptureColors.Accent else Color.White.copy(alpha = .9f),
            fontFamily = CaptureMono,
            fontWeight = FontWeight.Bold,
            fontSize = 11.sp
        )
    }
}

/** Dimmed backdrop plus a bottom sheet; tapping the backdrop closes it. */
@Composable
internal fun ProSheetFrame(title: String, onClose: () -> Unit, content: @Composable () -> Unit) {
    // The system back gesture closes the sheet instead of leaving the app.
    BackHandler(onBack = onClose)
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = .55f))
            .clickable(onClick = onClose),
        contentAlignment = Alignment.BottomCenter
    ) {
        Column(
            Modifier
                .fillMaxWidth()
                .fillMaxHeight(.72f)
                .clip(RoundedCornerShape(topStart = 26.dp, topEnd = 26.dp))
                .background(CaptureColors.Panel)
                // Swallow taps so only the backdrop closes the sheet.
                .pointerInput(Unit) { detectTapGestures(onTap = {}) }
        ) {
            Row(
                Modifier.fillMaxWidth().padding(start = 20.dp, end = 12.dp, top = 14.dp, bottom = 6.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    title,
                    color = Color.White,
                    fontFamily = CaptureMono,
                    fontWeight = FontWeight.Bold,
                    fontSize = 16.sp,
                    modifier = Modifier.weight(1f)
                )
                SmallPill("DONE", selected = true, onClick = onClose)
            }
            Column(
                Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 6.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp)
            ) { content() }
        }
    }
}

@Composable
internal fun SheetLabel(text: String) {
    Text(
        text,
        color = CaptureColors.Muted,
        fontFamily = CaptureMono,
        fontWeight = FontWeight.Bold,
        fontSize = 11.sp,
        modifier = Modifier.padding(top = 4.dp)
    )
}

/** Everything that changes the look, in one place: render profile (LUT), film simulation and its presets. */
@Composable
internal fun FiltersSheet(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    renderProfiles: RenderProfileQuickState,
    onSelectRenderProfile: (RenderProfileSelection) -> Unit,
    filmQuick: FilmSimQuickState?,
    onFilmEvent: (CaptureFilmEvent) -> Unit,
    onClose: () -> Unit
) {
    val haptics = LocalCaptureHaptics.current
    ProSheetFrame("FILTERS", onClose) {
        SheetLabel("LOOK")
        ChipFlow {
            renderProfiles.options.forEach { option ->
                SmallPill(
                    option.label,
                    selected = !state.filmSimEnabled && option.id == renderProfiles.selectedId,
                    enabled = !renderProfiles.locked
                ) {
                    haptics.selection()
                    // A look and film simulation never render together: picking a look hands the render back to it.
                    if (state.filmSimEnabled) dispatch(ToggleFilmSim)
                    onSelectRenderProfile(option.id)
                }
            }
        }
        if (filmQuick != null) {
            SheetLabel("FILM SIMULATION")
            ChipFlow {
                SmallPill("OFF", selected = !state.filmSimEnabled) {
                    haptics.selection()
                    if (state.filmSimEnabled) dispatch(ToggleFilmSim)
                }
                filmQuick.presets.forEach { preset ->
                    SmallPill(
                        preset.label,
                        selected = state.filmSimEnabled && filmQuick.selectedPresetId == preset.id
                    ) {
                        haptics.selection()
                        if (!state.filmSimEnabled) dispatch(ToggleFilmSim)
                        onFilmEvent(CaptureFilmEvent.SelectPreset(preset.id))
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ChipFlow(content: @Composable () -> Unit) {
    FlowRow(
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) { content() }
}

/** Every adjustable look value as a large slider with a reset: tone controls, or the film simulation's when it is on. */
@Composable
internal fun ParamsSheet(
    state: CaptureUiState,
    dispatch: CaptureDispatch,
    filmQuick: FilmSimQuickState?,
    onFilmEvent: (CaptureFilmEvent) -> Unit,
    onClose: () -> Unit
) {
    ProSheetFrame("PARAMS", onClose) {
        if (state.filmSimEnabled && filmQuick != null) {
            filmQuick.sections.forEach { section ->
                SheetLabel(section.label.uppercase())
                section.items.filterIsInstance<FilmStripItem.Numeric>().forEach { item ->
                    val param = filmQuick.params[item.key] ?: return@forEach
                    ParamSlider(
                        label = param.shortLabel,
                        display = param.displayValue,
                        value = param.value,
                        range = param.minimum..param.maximum,
                        isDefault = param.isDefault,
                        onChange = { value ->
                            onFilmEvent(
                                CaptureFilmEvent.ScrubNumeric(FilmSimNumericParameter.valueOf(item.key), value)
                            )
                        },
                        onReset = {
                            onFilmEvent(CaptureFilmEvent.ResetNumeric(FilmSimNumericParameter.valueOf(item.key)))
                        }
                    )
                }
            }
        } else {
            SheetLabel("TONE")
            TonemapCatalog.tone.forEach { entry ->
                val parameter = entry.param ?: return@forEach
                val current = TonemapCatalog.valueOf(state, parameter)
                ParamSlider(
                    label = entry.longLabel,
                    display = if (current > 0) "+$current" else "$current",
                    value = current.toFloat(),
                    range = entry.minimum..entry.maximum,
                    isDefault = TonemapCatalog.isDefault(state, parameter),
                    onChange = { value -> dispatch(ScrubTone(parameter, value.roundToInt() - current)) },
                    onReset = { dispatch(ScrubTone(parameter, entry.defaultValue.roundToInt() - current)) }
                )
            }
        }
    }
}

@Composable
private fun ParamSlider(
    label: String,
    display: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    isDefault: Boolean,
    onChange: (Float) -> Unit,
    onReset: () -> Unit
) {
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                label,
                color = Color.White.copy(alpha = .9f),
                fontFamily = CaptureMono,
                fontWeight = FontWeight.Bold,
                fontSize = 12.sp,
                modifier = Modifier.weight(1f)
            )
            Text(
                display,
                color = if (isDefault) CaptureColors.Muted else CaptureColors.Accent,
                fontFamily = CaptureMono,
                fontWeight = FontWeight.Bold,
                fontSize = 13.sp
            )
            Box(Modifier.padding(start = 10.dp)) { SmallPill("RESET", selected = false, enabled = !isDefault, onClick = onReset) }
        }
        Slider(
            value = value.coerceIn(range.start, range.endInclusive),
            onValueChange = onChange,
            valueRange = range,
            colors = SliderDefaults.colors(
                thumbColor = CaptureColors.Accent,
                activeTrackColor = CaptureColors.Accent,
                inactiveTrackColor = Color.White.copy(alpha = .18f)
            )
        )
    }
}
