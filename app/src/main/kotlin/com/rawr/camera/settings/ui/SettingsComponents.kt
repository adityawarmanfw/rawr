package com.rawr.camera.settings.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.ChevronRight
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rawr.camera.settings.model.NumericSettingSpec
import java.util.Locale
import kotlin.math.roundToInt

internal val SettingsContentWidth = 720.dp
internal val SettingsSectionSpacing = 12.dp
internal val SettingsCardShape = RoundedCornerShape(12.dp)

/**
 * Single rhythm for every settings row. All interactive rows share the same
 * minimum height and gutters so title-only, value, switch and radio rows
 * line up; titles share bodyLarge and values share labelLarge/primary.
 */
internal val SettingsRowMinHeight = 64.dp
internal val SettingsRowHorizontalPadding = 16.dp
internal val SettingsRowVerticalPadding = 12.dp
internal val SettingsSliderHorizontalPadding = 16.dp
internal val SettingsSliderVerticalPadding = 8.dp

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
        verticalArrangement = Arrangement.spacedBy(12.dp),
        content = content
    )
}

@Composable
internal fun SettingsGroup(
    title: String? = null,
    description: String? = null,
    content: @Composable ColumnScope.() -> Unit
) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        if (title != null) {
            Text(
                title.uppercase(),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.padding(start = SettingsRowHorizontalPadding)
            )
        }
        if (description != null) {
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
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                title,
                style = MaterialTheme.typography.bodyLarge,
                color =
                    if (enabled) {
                        MaterialTheme.colorScheme.onSurface
                    } else {
                        MaterialTheme.colorScheme.onSurface.copy(
                            alpha = .38f
                        )
                    }
            )
            if (value != null) {
                Text(
                    value,
                    style = MaterialTheme.typography.labelLarge,
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
            if (supportingText != null) {
                Text(
                    supportingText,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) 1f else .5f),
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
        if (trailing != null) {
            Spacer(Modifier.width(12.dp))
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
            RadioButton(
                selected = selected,
                onClick = null,
                enabled = enabled
            )
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
            Switch(
                checked = checked,
                onCheckedChange = onCheckedChange,
                enabled = enabled
            )
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
@OptIn(ExperimentalFoundationApi::class)
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
    supportingTextBelowSlider: Boolean = false,
    dimDisabledText: Boolean = true,
    onValueChange: (Float) -> Unit
) {
    require(maximum > minimum && step > 0f)
    val stepCount = ((maximum - minimum) / step).roundToInt().coerceAtLeast(1)
    val sliderSteps = (stepCount - 1).coerceAtLeast(0)
    val haptics = LocalSettingsHaptics.current

    fun stepIndex(v: Float): Int = ((v.coerceIn(minimum, maximum) - minimum) / step).roundToInt()
    val currentStepIndex = stepIndex(value)
    var lastHapticStep by remember(identity) { mutableIntStateOf(currentStepIndex) }
    LaunchedEffect(currentStepIndex) { lastHapticStep = currentStepIndex }

    Column(
        Modifier.fillMaxWidth()
            .padding(horizontal = SettingsSliderHorizontalPadding, vertical = SettingsSliderVerticalPadding)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                label,
                style = MaterialTheme.typography.bodyLarge,
                color =
                    if (enabled || !dimDisabledText) {
                        MaterialTheme.colorScheme.onSurface
                    } else {
                        MaterialTheme.colorScheme.onSurface.copy(
                            alpha = .38f
                        )
                    },
                modifier =
                    Modifier.weight(1f).combinedClickable(
                        enabled = enabled && defaultValue != null,
                        onClick = {},
                        onDoubleClick = { defaultValue?.let(onValueChange) }
                    )
            )
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
                style = MaterialTheme.typography.labelLarge,
                color =
                    if (enabled || !dimDisabledText) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                            .copy(
                                alpha = .45f
                            )
                    }
            )
        }
        if (supportingText != null && !supportingTextBelowSlider) {
            Text(
                supportingText,
                style = MaterialTheme.typography.bodySmall,
                color =
                    if (enabled || !dimDisabledText) {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = .38f)
                    },
                modifier = Modifier.padding(top = 2.dp)
            )
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Slider(
                value = value.coerceIn(minimum, maximum),
                onValueChange = { candidate ->
                    val nextStep = stepIndex(candidate)
                    if (nextStep != lastHapticStep) {
                        haptics.sliderTick()
                        lastHapticStep = nextStep
                    }
                    onValueChange(candidate)
                },
                onValueChangeFinished = { lastHapticStep = stepIndex(value) },
                valueRange = minimum..maximum,
                steps = sliderSteps,
                modifier = Modifier.weight(1f),
                enabled = enabled
            )
            SliderNudge("−", enabled && currentStepIndex > 0) {
                haptics.sliderTick()
                onValueChange((minimum + (currentStepIndex - 1) * step).coerceIn(minimum, maximum))
            }
            SliderNudge("+", enabled && currentStepIndex < stepCount) {
                haptics.sliderTick()
                onValueChange((minimum + (currentStepIndex + 1) * step).coerceIn(minimum, maximum))
            }
        }
        if (supportingText != null && supportingTextBelowSlider) {
            Text(
                supportingText,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

/** One-step fine adjustment beside a slider. */
@Composable
private fun SliderNudge(symbol: String, enabled: Boolean, onClick: () -> Unit) {
    TextButton(
        onClick = onClick,
        enabled = enabled,
        contentPadding = PaddingValues(0.dp),
        modifier = Modifier.size(36.dp)
    ) {
        Text(symbol, style = MaterialTheme.typography.titleMedium)
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
    Row(
        modifier = Modifier.fillMaxWidth()
            .padding(horizontal = SettingsSliderHorizontalPadding, vertical = SettingsSliderVerticalPadding),
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        values.forEach { value ->
            val active = value == selected
            if (active) {
                FilledTonalButton(
                    onClick = { onSelected(value) },
                    modifier = Modifier.weight(1f).heightIn(min = 40.dp),
                    contentPadding = PaddingValues(horizontal = 8.dp)
                ) { Text(label(value), maxLines = 1) }
            } else {
                OutlinedButton(
                    onClick = { onSelected(value) },
                    modifier = Modifier.weight(1f).heightIn(min = 40.dp),
                    contentPadding = PaddingValues(horizontal = 8.dp)
                ) { Text(label(value), maxLines = 1) }
            }
        }
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
            androidx.compose.material3.Switch(
                checked = checked,
                onCheckedChange = onCheckedChange,
                enabled = enabled
            )
        }
    )
}

@Composable
internal fun CapabilityUnavailableText(feature: String) {
    Text(
        "$feature is not available on the currently selected camera. Your global preference is preserved.",
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
    MultiChoiceSegmentedButtonRow(
        modifier = modifier.fillMaxWidth()
            .padding(horizontal = SettingsSliderHorizontalPadding, vertical = SettingsSliderVerticalPadding)
    ) {
        SegmentedButton(
            checked = photoOn,
            onCheckedChange = onPhotoChange,
            shape = SegmentedButtonDefaults.itemShape(index = 0, count = 2),
            icon = { SegmentedButtonDefaults.Icon(active = photoOn) },
            label = { Text("Photo") }
        )
        SegmentedButton(
            checked = videoOn,
            onCheckedChange = onVideoChange,
            shape = SegmentedButtonDefaults.itemShape(index = 1, count = 2),
            icon = { SegmentedButtonDefaults.Icon(active = videoOn) },
            label = { Text("Video") }
        )
    }
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
    SingleChoiceSegmentedButtonRow(
        modifier = modifier.fillMaxWidth()
            .padding(horizontal = SettingsSliderHorizontalPadding, vertical = SettingsSliderVerticalPadding)
    ) {
        options.forEachIndexed { index, label ->
            SegmentedButton(
                selected = index == selected,
                onClick = { onSelect(index) },
                enabled = enabled,
                shape = SegmentedButtonDefaults.itemShape(index = index, count = options.size),
                // No check icon: with 3+ segments the icon crowds longer
                // labels (e.g. "Chroma only") against the segment border.
                icon = {},
                label = { Text(label, maxLines = 1) }
            )
        }
    }
}

/** Settings row that opens a dropdown of [values]; the row shows the selected label. */
@Composable
internal fun <T> DropdownChoiceRow(
    title: String,
    selected: T,
    values: List<T>,
    label: (T) -> String,
    enabled: Boolean = true,
    supportingText: String? = null,
    onSelect: (T) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    Box {
        SettingsRow(title, label(selected), supportingText = supportingText, enabled = enabled, onClick = { expanded = true })
        DropdownMenu(expanded = expanded && enabled, onDismissRequest = { expanded = false }) {
            values.forEach { value ->
                DropdownMenuItem(text = { Text(label(value)) }, onClick = {
                    onSelect(value)
                    expanded = false
                })
            }
        }
    }
}
