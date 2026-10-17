package com.rawr.camera.settings.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.border
import androidx.compose.foundation.selection.selectable
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.ArrowDropDown
import com.rawr.camera.ui.icons.rounded.Check
import com.rawr.camera.ui.icons.rounded.ChevronRight
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.draw.scale
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.rawr.camera.settings.model.NumericSettingSpec
import java.util.Locale
import kotlin.math.roundToInt

internal val SettingsContentWidth = 720.dp
internal val SettingsSectionSpacing = 8.dp
internal val SettingsCardShape = RoundedCornerShape(12.dp)

/**
 * Single rhythm for every settings row. Every item in a card sits on the
 * same 16dp side / 12dp top-bottom inset, so rows, sliders, segment bars
 * and chip groups stack with even gaps; titles share bodyMedium and values share labelMedium/primary.
 * Help text lives behind an ⓘ dialog so rows stay one or two lines.
 */
internal val SettingsRowMinHeight = 56.dp
internal val SettingsRowHorizontalPadding = 16.dp
internal val SettingsRowVerticalPadding = 12.dp
internal val SettingsSliderHorizontalPadding = 16.dp
internal val SettingsSliderVerticalPadding = 12.dp

/**
 * In-memory scroll offsets per settings destination. Plain remember +
 * DisposableEffect (no SavedStateRegistry): navigate away saves, returning
 * restores. Survives recomposition and navigation; rotation resets to top.
 */
internal class SettingsScrollMemory {
    private val offsets = mutableMapOf<String, Int>()

    fun offsetFor(key: String): Int = offsets[key] ?: 0

    fun save(key: String, offset: Int) {
        offsets[key] = offset
    }
}

internal data class SettingsScrollSession(val key: String, val memory: SettingsScrollMemory)

internal val LocalSettingsScrollSession = compositionLocalOf<SettingsScrollSession?> { null }

@Composable
internal fun SettingsPageContainer(
    modifier: Modifier = Modifier,
    testTag: String? = null,
    content: @Composable ColumnScope.() -> Unit
) {
    val session = LocalSettingsScrollSession.current
    val scrollState =
        if (session != null) {
            remember(session.key) {
                androidx.compose.foundation.ScrollState(session.memory.offsetFor(session.key))
            }.also { state ->
                androidx.compose.runtime.DisposableEffect(session.key) {
                    onDispose { session.memory.save(session.key, state.value) }
                }
            }
        } else {
            androidx.compose.foundation.rememberScrollState()
        }
    Column(
        modifier =
            modifier
                .fillMaxWidth()
                .then(if (testTag != null) Modifier.testTag(testTag) else Modifier)
                .widthIn(max = SettingsContentWidth)
                .verticalScroll(scrollState)
                .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(SettingsSectionSpacing),
        content = content
    )
}

@Composable
internal fun SettingsGroup(
    title: String? = null,
    description: String? = null,
    content: @Composable ColumnScope.() -> Unit
) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        if (title != null) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(start = SettingsRowHorizontalPadding)
            ) {
                Text(
                    title.uppercase(),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.primary,
                    fontWeight = FontWeight.SemiBold
                )
                if (description != null) SettingsInfoButton(title, description)
            }
        } else if (description != null) {
            Text(
                description,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = SettingsRowHorizontalPadding)
            )
        }
        Surface(
            shape = SettingsCardShape,
            color = MaterialTheme.colorScheme.surfaceContainerLow,
            tonalElevation = 0.dp
        ) {
            Column(content = content)
        }
    }
}

@Composable
internal fun SettingsRow(
    title: String,
    value: String? = null,
    supportingText: String? = null,
    enabled: Boolean = true,
    testTag: String? = null,
    onClick: (() -> Unit)? = null,
    leadingIcon: ImageVector? = null,
    trailing: @Composable (() -> Unit)? = null
) {
    Row(
        modifier =
            Modifier
                .fillMaxWidth()
                .then(if (testTag != null) Modifier.testTag(testTag) else Modifier)
                .then(if (onClick != null && enabled) Modifier.clickable(onClick = onClick) else Modifier)
                .heightIn(min = SettingsRowMinHeight)
                .padding(horizontal = SettingsRowHorizontalPadding, vertical = SettingsRowVerticalPadding),
        verticalAlignment = Alignment.CenterVertically
    ) {
        if (leadingIcon != null) {
            Icon(
                imageVector = leadingIcon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.padding(end = SettingsRowHorizontalPadding)
            )
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(1.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    title,
                    style = MaterialTheme.typography.bodyMedium,
                    color =
                        if (enabled) {
                            MaterialTheme.colorScheme.onSurface
                        } else {
                            MaterialTheme.colorScheme.onSurface.copy(
                                alpha = .38f
                            )
                        },
                    modifier = Modifier.weight(1f, fill = false)
                )
                if (supportingText != null) SettingsInfoButton(title, supportingText)
            }
            if (value != null) {
                Text(
                    value,
                    style = MaterialTheme.typography.labelMedium,
                    color =
                        if (enabled) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.onSurfaceVariant
                                .copy(
                                    alpha = .45f
                                )
                        },
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
        if (trailing != null) {
            Spacer(Modifier.width(8.dp))
            trailing()
        } else if (onClick != null) {
            Spacer(Modifier.width(12.dp))
            Icon(
                Icons.Rounded.ChevronRight,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) .65f else .3f)
            )
        }
    }
}

@Composable
internal fun SettingsSelectionRow(
    title: String,
    selected: Boolean,
    supportingText: String? = null,
    enabled: Boolean = true,
    testTag: String? = null,
    onClick: () -> Unit
) {
    SettingsRow(
        title = title,
        supportingText = supportingText,
        enabled = enabled,
        testTag = testTag,
        onClick = onClick,
        trailing = {
            SelectedCheck(selected, enabled)
        }
    )
}

@Composable
internal fun SettingDivider() {
    HorizontalDivider(
        modifier = Modifier.padding(start = SettingsRowHorizontalPadding),
        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = .36f)
    )
}

@Composable
internal fun SettingsSwitchRow(
    title: String,
    checked: Boolean,
    value: String? = null,
    supportingText: String? = null,
    enabled: Boolean = true,
    testTag: String? = null,
    onCheckedChange: (Boolean) -> Unit
) {
    SettingsRow(
        title = title,
        value = value,
        supportingText = supportingText,
        enabled = enabled,
        testTag = testTag,
        onClick = { onCheckedChange(!checked) },
        trailing = {
            CompactSwitch(checked, onCheckedChange, enabled)
        }
    )
}

@Composable
internal fun NumericSliderRow(
    spec: NumericSettingSpec,
    value: Float,
    enabled: Boolean = true,
    onValueChange: (Float) -> Unit
) {
    StandaloneNumericSliderRow(
        identity = spec.parameter.name,
        label = spec.label,
        minimum = spec.minimum,
        maximum = spec.maximum,
        step = spec.step,
        decimals = spec.decimals,
        value = value,
        enabled = enabled,
        defaultValue = spec.defaultValue,
        valueFormatter = { formatNumericValue(spec, it) },
        dimDisabledText = false,
        onValueChange = onValueChange
    )
}

@Composable
@OptIn(ExperimentalFoundationApi::class, ExperimentalMaterial3Api::class)
internal fun StandaloneNumericSliderRow(
    identity: String,
    label: String,
    supportingText: String? = null,
    minimum: Float,
    maximum: Float,
    step: Float,
    decimals: Int,
    value: Float,
    enabled: Boolean = true,
    defaultValue: Float? = null,
    unit: String = "",
    valueFormatter: ((Float) -> String)? = null,
    dimDisabledText: Boolean = true,
    onValueChange: (Float) -> Unit
) {
    require(maximum > minimum && step > 0f)
    val stepCount = ((maximum - minimum) / step).roundToInt().coerceAtLeast(1)
    val haptics = LocalSettingsHaptics.current

    fun stepIndex(v: Float): Int = ((v.coerceIn(minimum, maximum) - minimum) / step).roundToInt()
    val currentStepIndex = stepIndex(value)
    // Compare on the step grid so float noise never reads as "changed".
    val modified = defaultValue != null && currentStepIndex != stepIndex(defaultValue)
    var entry by remember(identity) { mutableStateOf<String?>(null) }
    fun snapped(v: Float) = (minimum + stepIndex(v) * step).coerceIn(minimum, maximum)
    var lastHapticStep by remember(identity) { mutableIntStateOf(currentStepIndex) }
    LaunchedEffect(currentStepIndex) { lastHapticStep = currentStepIndex }

    Column(
        Modifier.fillMaxWidth()
            .padding(
                start = SettingsSliderHorizontalPadding,
                end = SettingsSliderHorizontalPadding,
                top = SettingsSliderVerticalPadding,
                // The 20dp thumb sits centred in the 32dp track, so 6dp here
                // reads as the same 12dp gap as above the label.
                bottom = SettingsSliderVerticalPadding - 6.dp
            )
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    label,
                    style = MaterialTheme.typography.bodyMedium,
                    color =
                        if (enabled || !dimDisabledText) {
                            MaterialTheme.colorScheme.onSurface
                        } else {
                            MaterialTheme.colorScheme.onSurface.copy(
                                alpha = .38f
                            )
                        },
                    modifier =
                        Modifier.weight(1f, fill = false).combinedClickable(
                            enabled = enabled && defaultValue != null,
                            onClick = {},
                            onDoubleClick = { defaultValue?.let(onValueChange) }
                        )
                )
                if (supportingText != null) SettingsInfoButton(label, supportingText)
            }
            Spacer(Modifier.width(12.dp))
            if (modified) {
                Box(
                    Modifier.padding(end = 6.dp).size(6.dp)
                        .background(MaterialTheme.colorScheme.primary.copy(alpha = if (enabled) 1f else .38f), CircleShape)
                )
            }
            Text(
                valueFormatter?.invoke(value)
                    ?: (
                        (
                            if (decimals ==
                                0
                            ) {
                                value.roundToInt().toString()
                            } else {
                                String.format(Locale.US, "%.${decimals}f", value)
                            }
                            ) +
                            unit
                        ),
                style = MaterialTheme.typography.labelMedium,
                fontWeight = if (modified) FontWeight.SemiBold else null,
                modifier = Modifier
                    .clickable(enabled = enabled, onClickLabel = "Enter $label") {
                        entry = if (decimals == 0) value.roundToInt().toString()
                        else String.format(Locale.US, "%.${decimals}f", value)
                    },
                color =
                    if (!enabled && dimDisabledText) {
                        MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = .45f)
                    } else if (modified || defaultValue == null) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    }
            )
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            SettingsSliderTrack(
                label = label,
                value = value,
                minimum = minimum,
                maximum = maximum,
                defaultValue = defaultValue,
                enabled = enabled,
                modifier = Modifier.weight(1f).height(32.dp)
            ) { candidate ->
                val nextStep = stepIndex(candidate)
                if (nextStep != lastHapticStep) {
                    haptics.sliderTick()
                    lastHapticStep = nextStep
                }
                onValueChange((minimum + nextStep * step).coerceIn(minimum, maximum))
            }
            SliderNudge("−", enabled && currentStepIndex > 0) {
                haptics.sliderTick()
                onValueChange((minimum + (currentStepIndex - 1) * step).coerceIn(minimum, maximum))
            }
            SliderNudge("+", enabled && currentStepIndex < stepCount) {
                haptics.sliderTick()
                onValueChange((minimum + (currentStepIndex + 1) * step).coerceIn(minimum, maximum))
            }
        }
    }
    entry?.let { text ->
        val parsed = text.replace(',', '.').toFloatOrNull()?.takeIf { it.isFinite() }
        AlertDialog(
            onDismissRequest = { entry = null },
            title = { Text(label) },
            text = {
                OutlinedTextField(
                    value = text,
                    onValueChange = { entry = it },
                    singleLine = true,
                    isError = text.isNotEmpty() && parsed == null,
                    supportingText = {
                        Text(String.format(Locale.US, "%.${decimals}f – %.${decimals}f", minimum, maximum) + unit)
                    },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal)
                )
            },
            confirmButton = {
                TextButton(
                    enabled = parsed != null,
                    onClick = { parsed?.let { onValueChange(snapped(it)) }; entry = null }
                ) { Text("Set") }
            },
            dismissButton = {
                if (defaultValue != null) {
                    TextButton(onClick = { onValueChange(defaultValue); entry = null }) { Text("Reset") }
                }
            }
        )
    }
}

/**
 * Single-Canvas slider (same drawing as the Renderer editor). Material's
 * Slider lays out thumb, track and tick children per row, which makes
 * slider-heavy pages stutter while scrolling.
 */
@Composable
private fun SettingsSliderTrack(
    label: String,
    value: Float,
    minimum: Float,
    maximum: Float,
    defaultValue: Float?,
    enabled: Boolean,
    modifier: Modifier,
    onValueChange: (Float) -> Unit
) {
    val latestChange by rememberUpdatedState(onValueChange)
    val active = MaterialTheme.colorScheme.primary.copy(alpha = if (enabled) 1f else .38f)
    val inactive = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) .3f else .15f)
    val defaultMark = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) .6f else .25f)
    Canvas(
        modifier
            .semantics {
                contentDescription = label
                progressBarRangeInfo = ProgressBarRangeInfo(value.coerceIn(minimum, maximum), minimum..maximum)
                if (enabled) setProgress { latestChange(it); true } else disabled()
            }
            .pointerInput(enabled, minimum, maximum) {
                if (!enabled) return@pointerInput
                val inset = 2.dp.toPx()
                fun at(x: Float) =
                    minimum + ((x - inset) / (size.width - inset * 2).coerceAtLeast(1f)).coerceIn(0f, 1f) *
                        (maximum - minimum)
                detectTapGestures { latestChange(at(it.x)) }
            }
            .pointerInput(enabled, minimum, maximum) {
                if (!enabled) return@pointerInput
                val inset = 2.dp.toPx()
                fun at(x: Float) =
                    minimum + ((x - inset) / (size.width - inset * 2).coerceAtLeast(1f)).coerceIn(0f, 1f) *
                        (maximum - minimum)
                detectHorizontalDragGestures(onDragStart = { latestChange(at(it.x)) }) { change, _ ->
                    change.consume()
                    latestChange(at(change.position.x))
                }
            }
    ) {
        val inset = 2.dp.toPx()
        val y = size.height / 2
        val x = inset + ((value - minimum) / (maximum - minimum)).coerceIn(0f, 1f) * (size.width - inset * 2)
        val stroke = 4.dp.toPx()
        drawLine(inactive, Offset(inset, y), Offset(size.width - inset, y), stroke, StrokeCap.Round)
        drawLine(active, Offset(inset, y), Offset(x, y), stroke, StrokeCap.Round)
        if (defaultValue != null) {
            val dx = inset + ((defaultValue - minimum) / (maximum - minimum)).coerceIn(0f, 1f) * (size.width - inset * 2)
            drawLine(defaultMark, Offset(dx, y - 7.dp.toPx()), Offset(dx, y + 7.dp.toPx()), 1.5.dp.toPx())
        }
        drawRoundRect(
            active,
            topLeft = Offset(x - 2.dp.toPx(), y - 10.dp.toPx()),
            size = Size(4.dp.toPx(), 20.dp.toPx()),
            cornerRadius = CornerRadius(2.dp.toPx())
        )
    }
}

/** One-step fine adjustment beside a slider. */
@Composable
private fun SliderNudge(symbol: String, enabled: Boolean, onClick: () -> Unit) {
    TextButton(
        onClick = onClick,
        enabled = enabled,
        contentPadding = PaddingValues(0.dp),
        modifier = Modifier.size(36.dp, 32.dp)
    ) {
        Text(symbol, style = MaterialTheme.typography.titleSmall)
    }
}

/** Switch at 75% so it sits inside a compact row; the row keeps the touch target. */
@Composable
internal fun CompactSwitch(checked: Boolean, onCheckedChange: ((Boolean) -> Unit)?, enabled: Boolean = true) {
    CompositionLocalProvider(LocalMinimumInteractiveComponentSize provides Dp.Unspecified) {
        Box(Modifier.size(40.dp, 24.dp), contentAlignment = Alignment.Center) {
            Switch(
                checked = checked,
                onCheckedChange = onCheckedChange,
                enabled = enabled,
                modifier = Modifier.scale(.75f)
            )
        }
    }
}

/** Single-choice marker: a check on the selected row, nothing elsewhere. */
@Composable
internal fun SelectedCheck(selected: Boolean, enabled: Boolean = true) {
    Box(Modifier.size(24.dp), contentAlignment = Alignment.Center) {
        if (selected) {
            Icon(
                Icons.Rounded.Check,
                contentDescription = "Selected",
                tint = MaterialTheme.colorScheme.primary.copy(alpha = if (enabled) 1f else .38f),
                modifier = Modifier.size(20.dp)
            )
        }
    }
}

/** Small ⓘ that shows a row's help text in a dialog instead of inline. */
@Composable
internal fun SettingsInfoButton(title: String, text: String) {
    var open by remember { mutableStateOf(false) }
    Text(
        "ⓘ",
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = .7f),
        modifier = Modifier
            .clickable(onClickLabel = "About $title") { open = true }
            .padding(horizontal = 6.dp, vertical = 2.dp)
    )
    if (open) {
        AlertDialog(
            onDismissRequest = { open = false },
            title = { Text(title) },
            text = { Text(text) },
            confirmButton = { TextButton(onClick = { open = false }) { Text("OK") } }
        )
    }
}

internal fun numericStepIndex(spec: NumericSettingSpec, value: Float): Int =
    ((value.coerceIn(spec.minimum, spec.maximum) - spec.minimum) / spec.step).roundToInt()

internal fun formatNumericValue(spec: NumericSettingSpec, value: Float): String {
    val number =
        if (spec.decimals == 0) {
            value.roundToInt().toString()
        } else {
            String.format(Locale.US, "%.${spec.decimals}f", value)
        }
    val prefix = if (spec.minimum < 0f && value > 0f) "+" else ""
    return prefix + number + spec.unit
}

@Composable
internal fun <T> CompactChoiceRow(values: List<T>, selected: T, label: (T) -> String, onSelected: (T) -> Unit) {
    CompactSegments(
        labels = values.map(label),
        isSelected = { values[it] == selected },
        onClick = { onSelected(values[it]) }
    )
}

/**
 * 28dp joined segment bar in the Renderer's boxed style: thin outline,
 * tinted fill + accent text on selected segments. Stands in for Material
 * segmented buttons, chips and button rows, which run 36-40dp tall.
 */
@Composable
internal fun CompactSegments(
    labels: List<String>,
    isSelected: (Int) -> Boolean,
    onClick: (Int) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {
    val shape = RoundedCornerShape(6.dp)
    val outline = MaterialTheme.colorScheme.outlineVariant
    Row(
        modifier
            .fillMaxWidth()
            .padding(horizontal = SettingsSliderHorizontalPadding, vertical = SettingsSliderVerticalPadding)
            .height(28.dp)
            .clip(shape)
            .border(1.dp, outline, shape)
    ) {
        labels.forEachIndexed { index, text ->
            if (index > 0) Box(Modifier.width(1.dp).fillMaxHeight().background(outline))
            CompactSegmentCell(text, isSelected(index), enabled, Modifier.weight(1f)) { onClick(index) }
        }
    }
}

/** Standalone 28dp pill for wrapped chip groups (film stocks, presets). */
@Composable
internal fun CompactPill(text: String, selected: Boolean, modifier: Modifier = Modifier, onClick: () -> Unit) {
    val shape = RoundedCornerShape(6.dp)
    CompactSegmentCell(
        text,
        selected,
        true,
        modifier
            .height(28.dp)
            .clip(shape)
            .border(
                1.dp,
                if (selected) MaterialTheme.colorScheme.primary.copy(alpha = .6f) else MaterialTheme.colorScheme.outlineVariant,
                shape
            ),
        horizontalPadding = 10.dp,
        onClick = onClick
    )
}

@Composable
private fun CompactSegmentCell(
    text: String,
    selected: Boolean,
    enabled: Boolean,
    modifier: Modifier,
    horizontalPadding: Dp = 4.dp,
    onClick: () -> Unit
) {
    val primary = MaterialTheme.colorScheme.primary
    Box(
        modifier
            .fillMaxHeight()
            .background(if (selected) primary.copy(alpha = .16f) else androidx.compose.ui.graphics.Color.Transparent)
            .selectable(selected = selected, enabled = enabled, role = Role.Tab, onClick = onClick)
            .padding(horizontal = horizontalPadding),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text,
            style = MaterialTheme.typography.labelMedium,
            fontWeight = if (selected) FontWeight.SemiBold else null,
            color = when {
                !enabled -> MaterialTheme.colorScheme.onSurface.copy(alpha = .38f)
                selected -> primary
                else -> MaterialTheme.colorScheme.onSurfaceVariant
            },
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

@Composable
internal fun ToggleSubmenuRow(
    title: String,
    checked: Boolean,
    supportingText: String? = null,
    value: String? = null,
    enabled: Boolean = true,
    testTag: String? = null,
    onOpen: () -> Unit,
    onCheckedChange: (Boolean) -> Unit
) {
    // Android Wireless-debugging pattern: row tap navigates, switch tap toggles.
    // Switch consumes its own gesture so the row navigation does not fire.
    SettingsRow(
        title = title,
        value = value,
        supportingText = supportingText,
        enabled = enabled,
        testTag = testTag,
        onClick = onOpen,
        trailing = {
            CompactSwitch(checked, onCheckedChange, enabled)
        }
    )
}

@Composable
internal fun CapabilityUnavailableText(feature: String) {
    Text(
        "$feature: not on this camera (setting kept)",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = SettingsRowHorizontalPadding, vertical = SettingsSliderVerticalPadding)
    )
}

/**
 * MD3 multi-select segmented on/off toggle for one processing stage.
 * Photo and Video check independently: either, both, or neither can be on.
 * Strength sliders live in separate Photo/Video groups below; each group is
 * enabled only while its segment is checked.
 */
@Composable
internal fun PhotoVideoTargetSelector(
    photoOn: Boolean,
    videoOn: Boolean,
    onPhotoChange: (Boolean) -> Unit,
    onVideoChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    CompactSegments(
        labels = listOf(if (photoOn) "✓ Photo" else "Photo", if (videoOn) "✓ Video" else "Video"),
        isSelected = { if (it == 0) photoOn else videoOn },
        onClick = { if (it == 0) onPhotoChange(!photoOn) else onVideoChange(!videoOn) },
        modifier = modifier
    )
}

/** One-line summary of a Photo/Video enable pair for hub rows. */
internal fun photoVideoSummary(photoOn: Boolean, videoOn: Boolean): String = when {
    photoOn && videoOn -> "Photo + Video"
    photoOn -> "Photo only"
    videoOn -> "Video only"
    else -> "Off"
}

/**
 * MD3 single-choice segmented row for tiny discrete lists (2-4 short
 * options, inline). Replaces radio rows where the options fit side by side.
 */
@Composable
internal fun SegmentedOptionRow(
    options: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {
    CompactSegments(options, { it == selected }, onSelect, modifier, enabled)
}

/**
 * One-line choice: title left, current value + ⌄ right; tap opens a menu.
 * [optionText] puts a one-line explanation under each menu option.
 */
@Composable
internal fun <T> DropdownChoiceRow(
    title: String,
    selected: T,
    values: List<T>,
    label: (T) -> String,
    enabled: Boolean = true,
    supportingText: String? = null,
    optionText: ((T) -> String?)? = null,
    onSelect: (T) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    SettingsRow(
        title,
        supportingText = supportingText,
        enabled = enabled,
        onClick = { expanded = true },
        trailing = {
            // Menu anchors to the value, so it opens under the arrow at the row's end.
            Box {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        label(selected),
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.primary.copy(alpha = if (enabled) 1f else .45f),
                        maxLines = 1
                    )
                    Icon(
                        Icons.Rounded.ArrowDropDown,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) .8f else .3f)
                    )
                }
                DropdownMenu(expanded = expanded && enabled, onDismissRequest = { expanded = false }) {
                    values.forEach { value ->
                        val isSelected = value == selected
                        val detail = optionText?.invoke(value)
                        DropdownMenuItem(
                            text = {
                                Column(Modifier.widthIn(max = 280.dp)) {
                                    Text(
                                        label(value),
                                        style = MaterialTheme.typography.bodyMedium,
                                        color =
                                            if (isSelected) {
                                                MaterialTheme.colorScheme.primary
                                            } else {
                                                MaterialTheme.colorScheme.onSurface
                                            }
                                    )
                                    if (detail != null) {
                                        Text(
                                            detail,
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    }
                                }
                            },
                            trailingIcon = { SelectedCheck(isSelected) },
                            onClick = {
                                onSelect(value)
                                expanded = false
                            }
                        )
                    }
                }
            }
        }
    )
}
