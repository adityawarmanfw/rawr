package com.rawr.camera.model

import com.rawr.camera.settings.model.AgxCatalog
import com.rawr.camera.settings.model.AgxGroup
import com.rawr.camera.settings.model.AgxSection
import java.util.Locale

/** Strip key of the render exposure cell in the AgX RANGE section (not an AgX control). */
internal const val AGX_STRIP_EXPOSURE_KEY = "renderExposure"

/**
 * The AgX strip as a two-level FilmConfigBlock: darktable's sections
 * (RANGE, CURVE, LOOK, PRIM) at L1 and their groups at L2. Render exposure
 * leads the RANGE section, since AgX replaces the TONE controls.
 */
internal fun agxStripState(agx: AgxQuickState, renderExposureTenths: Int, presetLabel: String): FilmSimQuickState {
    val ev = TonemapCatalog.renderExposure
    val evValue = renderExposureTenths / 10f
    val params = buildMap {
        put(AGX_STRIP_EXPOSURE_KEY, FilmSimQuickParam(
            key = AGX_STRIP_EXPOSURE_KEY,
            shortLabel = ev.shortLabel,
            value = evValue,
            defaultValue = ev.defaultValue,
            minimum = ev.minimum,
            maximum = ev.maximum,
            step = ev.step,
            decimals = ev.decimals,
            displayValue = "%+.1f".format(Locale.US, evValue),
            isDefault = renderExposureTenths == 0
        ))
        AgxCatalog.all.forEach { spec ->
            val value = agx.values.valueOf(spec.parameter)
            val default = agx.defaults.valueOf(spec.parameter)
            put(spec.jsonKey, FilmSimQuickParam(
                key = spec.jsonKey,
                shortLabel = spec.shortLabel,
                value = value,
                defaultValue = default,
                minimum = spec.minimum,
                maximum = spec.maximum,
                step = spec.step,
                decimals = spec.valueDecimals,
                displayValue = spec.format(value, spec.stripUnit),
                isDefault = spec.isDefault(value, default),
                displayScale = spec.displayScale,
                displayDecimals = spec.decimals,
                unit = spec.stripUnit
            ))
        }
    }
    val sections = AgxSection.entries.map { section ->
        FilmSimQuickSection(section.name.lowercase(), section.label, AgxGroup.entries.filter { it.section == section }.map { group ->
            val exposure = if (group == AgxGroup.Range) listOf(FilmStripItem.Numeric(AGX_STRIP_EXPOSURE_KEY)) else emptyList()
            FilmSimQuickSubsection(group.name.lowercase(), group.label,
                exposure + AgxCatalog.group(group).map { FilmStripItem.Numeric(it.jsonKey) })
        })
    }
    return FilmSimQuickState(
        activePresetName = presetLabel,
        selectedPresetId = null,
        modified = agx.modified || renderExposureTenths != 0,
        sections = sections,
        params = params
    )
}
