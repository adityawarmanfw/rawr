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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.systemGestureExclusion
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
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
import com.rawr.camera.model.FilmSimQuickDiscrete
import com.rawr.camera.model.FilmSimQuickState
import com.rawr.camera.model.FilmStripItem

/**
 * Compact film-sim overlay. Bare viewfinder text with drop shadows, no cards.
 *
 * Modes: Collapsed (`{preset} + PARAMS`) -> Profiles (scrollable preset names)
 * or Config (L1 sections at the bottom, scrubbable two-line params above).
 * Sticky edges: `<` back stays left, the alternate entry (PARAMS / preset
 * name) stays right so modes switch without going back first. Everything is
 * gated on film ON by the caller; power itself lives on the FILM pill.
 */
private enum class FilmStripMode { Collapsed, Profiles, Config }

@Composable
internal fun FilmStrip(
    quick: FilmSimQuickState,
    onSelectPreset: (String) -> Unit,
    onScrubParam: (String, Float) -> Unit,
    onResetParam: (String) -> Unit,
    onScrubDiscrete: (String, Int) -> Unit,
    onToggleFlag: (String, Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    var mode by remember { mutableStateOf(FilmStripMode.Collapsed) }
    var sectionKey by remember { mutableStateOf(quick.sections.firstOrNull()?.key ?: "film") }
    var subsectionKey by remember { mutableStateOf<String?>(null) }
    // L4 drill into a discrete's option list. Cleared whenever the section
    // or subsection changes, or Config is left, so a stale drill never
    // greets the next open.
    var l4Key by remember { mutableStateOf<String?>(null) }
    // OFF hides the whole strip (caller gate); always restart collapsed so a
    // stale expanded mode never greets the next ON.
    LaunchedEffect(quick.presets.isEmpty()) {
        if (quick.presets.isEmpty()) mode = FilmStripMode.Collapsed
    }
    val section = quick.sections.firstOrNull { it.key == sectionKey }
        ?: quick.sections.firstOrNull()
    if (section != null && section.key != sectionKey) sectionKey = section.key
    val subsection = section?.subsections?.firstOrNull { it.key == subsectionKey }
        ?: section?.subsections?.firstOrNull()
    if (subsection != null && subsection.key != subsectionKey) subsectionKey = subsection.key
    Column(
        modifier.testTag(CaptureTestTags.FILM_STRIP),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        when (mode) {
            FilmStripMode.Collapsed -> {
                FilmCollapsedRow(
                    presetName = quick.activePresetName,
                    modified = quick.modified,
                    onOpenProfiles = { mode = FilmStripMode.Profiles },
                    onOpenParams = { mode = FilmStripMode.Config }
                )
            }
            FilmStripMode.Profiles -> {
                FilmProfilesRow(
                    quick = quick,
                    onBack = { mode = FilmStripMode.Collapsed },
                    onSelect = {
                        onSelectPreset(it)
                        // Stay in place (sticky) for rapid A/B comparison.
                    },
                    onOpenParams = { mode = FilmStripMode.Config }
                )
            }
            FilmStripMode.Config -> {
                FilmConfigBlock(
                    quick = quick,
                    sectionKey = section?.key ?: "film",
                    subsectionKey = subsection?.key,
                    onSection = {
                        sectionKey = it
                        subsectionKey = null
                        l4Key = null
                    },
                    onSubsection = {
                        subsectionKey = it
                        l4Key = null
                    },
                    onBack = {
                        l4Key = null
                        mode = FilmStripMode.Collapsed
                    },
                    onOpenProfiles = {
                        l4Key = null
                        mode = FilmStripMode.Profiles
                    },
                    l4Key = l4Key,
                    onOpenDiscrete = { l4Key = it },
                    onCloseL4 = { l4Key = null },
                    onScrubParam = onScrubParam,
                    onResetParam = onResetParam,
                    onScrubDiscrete = onScrubDiscrete,
                    onToggleFlag = onToggleFlag
                )
            }
        }
    }
}

@Composable
private fun FilmCollapsedRow(
    presetName: String,
    modified: Boolean,
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
                .testTag(CaptureTestTags.FILM_STRIP_PROFILE)
                .captureClickable(shape = CaptureTextRippleShape) {
                    haptics.selection()
                    onOpenProfiles()
                }.semantics {
                    contentDescription = "Film preset $presetName. Tap to choose preset."
                    onClick {
                        haptics.selection()
                        onOpenProfiles()
                        true
                    }
                },
            contentAlignment = Alignment.CenterEnd
        ) {
            Text(
                if (modified) "$presetName •" else presetName,
                color = if (modified) CaptureColors.AccentSoft else Color.White.copy(alpha = .95f),
                fontFamily = CaptureMono,
                fontWeight = FontWeight.SemiBold,
                fontSize = 10.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = ViewfinderTextStyle
            )
        }
        StripParamsEntry(onOpenParams)
    }
}

@Composable
private fun FilmProfilesRow(
    quick: FilmSimQuickState,
    onBack: () -> Unit,
    onSelect: (String) -> Unit,
    onOpenParams: () -> Unit
) {
    val haptics = LocalCaptureHaptics.current
    StripNavigationRow(onBack, Modifier.padding(bottom = 2.dp)) {
        LazyRow(
            Modifier
                .weight(1f)
                .height(CaptureDimens.CompactParamRowHeight),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(StripDimens.ParamSpacing, Alignment.End)
        ) {
            items(quick.presets, key = { it.id }) { preset ->
                val isSelected = preset.id == quick.selectedPresetId
                Box(
                    Modifier
                        .height(CaptureDimens.CompactParamRowHeight)
                        .testTag(CaptureTestTags.filmPreset(preset.id))
                        .captureClickable(shape = CaptureTextRippleShape) {
                            haptics.selection()
                            onSelect(preset.id)
                        }.semantics {
                            contentDescription = "Film preset ${preset.label}" +
                                if (isSelected) ", active" else ""
                            role = Role.RadioButton
                            selected = isSelected
                            onClick {
                                haptics.selection()
                                onSelect(preset.id)
                                true
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        preset.label,
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
        StripParamsEntry(onOpenParams, Modifier.padding(start = StripDimens.ParamSpacing))
    }
}

@Composable
private fun FilmConfigBlock(
    quick: FilmSimQuickState,
    sectionKey: String,
    subsectionKey: String?,
    onSection: (String) -> Unit,
    onSubsection: (String) -> Unit,
    onBack: () -> Unit,
    onOpenProfiles: () -> Unit,
    l4Key: String?,
    onOpenDiscrete: (String) -> Unit,
    onCloseL4: () -> Unit,
    onScrubParam: (String, Float) -> Unit,
    onResetParam: (String) -> Unit,
    onScrubDiscrete: (String, Int) -> Unit,
    onToggleFlag: (String, Boolean) -> Unit
) {
    val haptics = LocalCaptureHaptics.current
    val section = quick.sections.firstOrNull { it.key == sectionKey }
        ?: quick.sections.firstOrNull()
    val subsection = section?.subsections?.firstOrNull { it.key == subsectionKey }
        ?: section?.subsections?.firstOrNull()
    val drill = l4Key?.let { key -> quick.discretes[key]?.let { key to it } }
    Column(Modifier.fillMaxWidth()) {
        // L4 option list sits as its own row above L3, keeping the
        // subsection items visible below. Back collapses just the L4 row.
        if (drill != null) {
            FilmDiscreteOptionsRow(
                dropdownKey = drill.first,
                entry = drill.second,
                onBack = onCloseL4,
                onSelect = { onScrubDiscrete(drill.first, it) }
            )
        }
        // L3: editors of the active subsection. Items scrub VERTICALLY only
        // (up = increase), so scrubbing never fights row scrolling and never
        // runs out of room at the screen edges. Discretes drill to L4 on tap
        // instead of cycling: one-by-one stepping through 20 stocks is not
        // viable. Five parameter cells fit in the available row width.
        BoxWithConstraints(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = CaptureDimens.CompactParamRowEdgeInset)
                .height(StripDimens.ParamRowHeight)
        ) {
            val cellWidths = paramsCellWidths(maxWidth)
            val hasDirToggle = section?.key == "dir" && subsection?.key == "couplers"
            LazyRow(
                Modifier.fillMaxSize(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(CaptureDimens.ControlGap, Alignment.End),
                contentPadding = PaddingValues(
                    start = parkedParamsPadding(maxWidth, cellWidths[0], CaptureDimens.ControlGap)
                )
            ) {
            // DIR has no dedicated flag: amount > 0 is the enable. Offer an
            // explicit ON/OFF cell so disabling never means scrubbing to
            // exactly 0 (mirrors the Settings switch, which writes 1/0).
            if (hasDirToggle) {
                val dirAmount = quick.params["DirCouplersAmount"]?.value ?: 0f
                item(key = "dir-enable") {
                    FilmFlagItem(
                        key = "DirEnabled",
                        modifier = Modifier.width(cellWidths[0]),
                        shortLabel = "DIR",
                        enabled = dirAmount > 0f,
                        onToggle = { onScrubParam("DirCouplersAmount", if (it) 1f else 0f) }
                    )
                }
            }
            subsection?.items?.forEachIndexed { index, item ->
                val cellWidth = cellWidths[(index + if (hasDirToggle) 1 else 0) % cellWidths.size]
                when (item) {
                    is FilmStripItem.Numeric -> {
                        val param = quick.params[item.key] ?: return@forEachIndexed
                        item(key = item.key) {
                            StripNumericItem(
                                key = item.key,
                                modifier = Modifier.width(cellWidth),
                                shortLabel = param.shortLabel,
                                value = param.value,
                                display = param.displayValue,
                                isDefault = param.isDefault,
                                defaultValue = param.defaultValue,
                                min = param.minimum,
                                max = param.maximum,
                                step = param.step,
                                decimals = param.decimals,
                                onScrub = { onScrubParam(item.key, it) },
                                onReset = { onResetParam(item.key) }
                            )
                        }
                    }
                    is FilmStripItem.Discrete -> {
                        val entry = quick.discretes[item.key] ?: return@forEachIndexed
                        item(key = item.key) {
                            FilmDiscreteItem(
                                key = item.key,
                                modifier = Modifier.width(cellWidth),
                                shortLabel = entry.shortLabel,
                                display = entry.displayValue,
                                drilled = drill?.first == item.key,
                                onOpen = { onOpenDiscrete(item.key) }
                            )
                        }
                    }
                    is FilmStripItem.Flag -> {
                        val entry = quick.flags[item.key] ?: return@forEachIndexed
                        item(key = item.key) {
                            FilmFlagItem(
                                key = item.key,
                                modifier = Modifier.width(cellWidth),
                                shortLabel = entry.shortLabel,
                                enabled = entry.enabled,
                                onToggle = { onToggleFlag(item.key, it) }
                            )
                        }
                    }
                }
            }
            }
        }
        // L2: subsections of the active section, parked at the thumb like L3.
        BoxWithConstraints(
            Modifier
                .fillMaxWidth()
                .height(CaptureDimens.CompactParamRowHeight)
        ) {
            LazyRow(
                Modifier
                    .fillMaxSize()
                    .testTag(CaptureTestTags.filmSubsectionRow(section?.key ?: "")),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(StripDimens.ParamSpacing, Alignment.End),
                contentPadding = PaddingValues(start = parkedParamsPadding(maxWidth))
            ) {
            section?.subsections?.forEach { sub ->
                val active = sub.key == subsection?.key
                item(key = section.key + "/" + sub.key) {
                    Box(
                        Modifier
                            .height(CaptureDimens.CompactParamRowHeight)
                            .testTag(CaptureTestTags.filmSubsection(section.key, sub.key))
                            .captureClickable(shape = CaptureTextRippleShape) {
                                haptics.selection()
                                onSubsection(sub.key)
                            }.semantics {
                                contentDescription = "Film subsection ${sub.label}" +
                                    if (active) ", active" else ""
                                role = Role.Tab
                                selected = active
                                onClick {
                                    haptics.selection()
                                    onSubsection(sub.key)
                                    true
                                }
                            },
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            sub.label,
                            color = if (active) CaptureColors.Accent else Color.White.copy(alpha = .62f),
                            fontFamily = CaptureMono,
                            fontWeight = FontWeight.Bold,
                            fontSize = 8.sp,
                            letterSpacing = .4.sp,
                            maxLines = 1,
                            style = ViewfinderTextStyle
                        )
                    }
                }
            }
            }
        }
        // Lower (L1): collapse in the first column, scrollable sections, and preset picker.
        StripNavigationRow(onBack, Modifier.padding(bottom = 2.dp)) {
            // L1 sections parked at the thumb like L2/L3.
            BoxWithConstraints(
                Modifier
                    .weight(1f)
                    .height(CaptureDimens.CompactParamRowHeight)
            ) {
                LazyRow(
                    Modifier
                        .fillMaxSize()
                        .testTag(CaptureTestTags.FILM_STRIP_SECTION_ROW),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(StripDimens.ParamSpacing, Alignment.End),
                    contentPadding = PaddingValues(start = parkedParamsPadding(maxWidth))
                ) {
                    items(quick.sections, key = { it.key }) { item ->
                    val active = item.key == section?.key
                    Box(
                        Modifier
                            .height(CaptureDimens.CompactParamRowHeight)
                            .testTag(CaptureTestTags.filmSection(item.key))
                            .captureClickable(shape = CaptureTextRippleShape) {
                                haptics.selection()
                                onSection(item.key)
                            }.semantics {
                                contentDescription = "Film section ${item.label}" +
                                    if (active) ", active" else ""
                                role = Role.Tab
                                selected = active
                                onClick {
                                    haptics.selection()
                                    onSection(item.key)
                                    true
                                }
                            },
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            item.label,
                            color = if (active) CaptureColors.Accent else Color.White.copy(alpha = .62f),
                            fontFamily = CaptureMono,
                            fontWeight = FontWeight.Bold,
                            fontSize = 8.sp,
                            letterSpacing = .4.sp,
                            maxLines = 1,
                            style = ViewfinderTextStyle
                        )
                    }
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
                        contentDescription = "Film preset ${quick.activePresetName}. Tap to choose preset."
                        onClick {
                            haptics.selection()
                            onOpenProfiles()
                            true
                        }
                    },
                contentAlignment = Alignment.CenterEnd
            ) {
                Text(
                    if (quick.modified) "${quick.activePresetName} •" else quick.activePresetName,
                    color = if (quick.modified) CaptureColors.AccentSoft else Color.White.copy(alpha = .95f),
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
}

/** Two-line viewfinder item geometry: value above, short label below. */
internal object StripDimens {
    val ParamRowHeight = 36.dp
    val ParamMinWidth = 68.dp
    /** Gap between scrollable strip cells (params, profiles, sections). */
    val ParamSpacing = 18.dp
    // Right-sticky entries (PARAMS, preset) sit one inset off the physical
    // edge: flush-right targets are hard to hit and trigger edge touches.
    val TrailingInset = 12.dp
    /**
     * Parked params show this many leading cells at the thumb edge, e.g. 1.5
     * reads as one full cell plus a peek of the next so the row affords scroll.
     */
    const val ParkedVisibleCells = 1.5f
}

/** Flag on/off as a two-line viewfinder text. Tap toggles. */
@Composable
private fun FilmFlagItem(
    key: String,
    shortLabel: String,
    enabled: Boolean,
    onToggle: (Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    val haptics = LocalCaptureHaptics.current
    Box(
        modifier
            .widthIn(min = StripDimens.ParamMinWidth)
            .height(StripDimens.ParamRowHeight)
            .testTag(CaptureTestTags.filmParam(key))
            .systemGestureExclusion()
            .captureClickable(shape = CaptureTextRippleShape) {
                haptics.selection()
                onToggle(!enabled)
            }.semantics {
                contentDescription = "$shortLabel ${if (enabled) "on" else "off"}. Tap to toggle."
                role = Role.Switch
                onClick {
                    haptics.selection()
                    onToggle(!enabled)
                    true
                }
            },
        contentAlignment = Alignment.Center
    ) {
        StripTwoLineItem(
            valueText = if (enabled) "ON" else "OFF",
            valueColor = if (enabled) CaptureColors.Accent else Color.White.copy(alpha = .62f),
            labelText = shortLabel
        )
    }
}

/**
 * Two-line discrete cell (current option above, short label below). Tap
 * drills into the L3 option list; option-by-option cycling is intentionally
 * not offered (stepping through 20 stocks one at a time is not viable).
 */
@Composable
private fun FilmDiscreteItem(
    key: String,
    shortLabel: String,
    display: String,
    onOpen: () -> Unit,
    modifier: Modifier = Modifier,
    drilled: Boolean = false
) {
    val haptics = LocalCaptureHaptics.current
    Box(
        modifier
            .widthIn(min = StripDimens.ParamMinWidth)
            .height(StripDimens.ParamRowHeight)
            .testTag(CaptureTestTags.filmParam(key))
            .captureClickable(shape = CaptureTextRippleShape) {
                haptics.selection()
                onOpen()
            }.semantics {
                contentDescription = "$shortLabel $display. Tap to choose option."
                onClick {
                    haptics.selection()
                    onOpen()
                    true
                }
            },
        contentAlignment = Alignment.Center
    ) {
        StripTwoLineItem(
            valueText = display,
            valueColor = if (drilled) CaptureColors.Accent else Color.White.copy(alpha = .95f),
            labelText = shortLabel
        )
    }
}

/**
 * L3 option list for a drilled-in discrete: its own row above L2 with
 * sticky back left and scrollable option texts. Tap applies immediately and
 * stays for rapid comparison; back collapses just this row.
 */
@Composable
private fun FilmDiscreteOptionsRow(
    dropdownKey: String,
    entry: FilmSimQuickDiscrete,
    onBack: () -> Unit,
    onSelect: (Int) -> Unit
) {
    val haptics = LocalCaptureHaptics.current
    StripNavigationRow(onBack, Modifier.height(StripDimens.ParamRowHeight)) {
        LazyRow(
            Modifier
                .weight(1f)
                .height(StripDimens.ParamRowHeight),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(StripDimens.ParamSpacing, Alignment.End)
        ) {
            items(entry.options.size) { index ->
                val option = entry.options[index]
                val isSelected = index == entry.selectedIndex
                Box(
                    Modifier
                        .height(StripDimens.ParamRowHeight)
                        .testTag(CaptureTestTags.filmDiscreteOption(dropdownKey, index))
                        .captureClickable(shape = CaptureTextRippleShape) {
                            haptics.selection()
                            onSelect(index)
                        }.semantics {
                            contentDescription = "$option" + if (isSelected) ", active" else ""
                            role = Role.RadioButton
                            selected = isSelected
                            onClick {
                                haptics.selection()
                                onSelect(index)
                                true
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        option,
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
    }
}
