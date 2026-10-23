package com.rawr.camera.model

import com.rawr.camera.settings.model.*

internal fun SettingsValues.toRenderProfileQuickState(): RenderProfileQuickState {
    if (isLogActive) return RenderProfileQuickState(
        options = VideoLogProfile.entries.map { RenderProfileQuickOption(RenderProfileSelection.Log(it), it.label) },
        selectedId = RenderProfileSelection.Log(videoLogProfile), log = true
    )
    val usable = userLutProfiles.filter { it.stages.isNotEmpty() }
    val builtIns = listOf(ColorRenderProfile.RawrBase, if (isVideo) ColorRenderProfile.Rec709 else ColorRenderProfile.SRgb)
    val selected = regularLutProfileId()?.takeIf { regularRenderProfile() == ColorRenderProfile.UserLut && usable.any { p -> p.id == it } }
    val agx = AgxPreset.entries.map { RenderProfileQuickOption(RenderProfileSelection.Agx(it), it.label) }
    val savedAgx = agxLooks.map { RenderProfileQuickOption(RenderProfileSelection.SavedAgx(it.id), it.name) }
    return RenderProfileQuickState(
        options = listOf(RenderProfileQuickOption(RenderProfileSelection.BuiltIn(builtIns[0]), builtIns[0].label)) + agx + savedAgx +
            builtIns.drop(1).map { RenderProfileQuickOption(RenderProfileSelection.BuiltIn(it), it.label) } +
            usable.map { RenderProfileQuickOption(RenderProfileSelection.Imported(it.id), it.name) },
        selectedId = selected?.let { RenderProfileSelection.Imported(it) }
            ?: matchingAgxLook()?.let { RenderProfileSelection.SavedAgx(it.id) }.takeIf { isAgxActive() }
            ?: RenderProfileSelection.Agx(regularAgxPreset()).takeIf { regularRenderProfile() == ColorRenderProfile.AgX }
            ?: RenderProfileSelection.BuiltIn(regularRenderProfile().takeIf { it in builtIns } ?: ColorRenderProfile.RawrBase),
        agx = if (isAgxActive()) AgxQuickState(activeAgxValues(), regularAgxPreset().defaults) else null
    )
}

// Compact viewfinder film strip: preset switching + numeric scrubs apply
// immediately to the preview (mirroring selectRenderProfileFromCapture).
// Origin-preset semantics match Settings: edits keep selectedFilmPresetId
// so the preset reads Modified instead of silently deselecting.
internal fun SettingsValues.toFilmSimQuickState(): FilmSimQuickState {
    val base = selectedFilmPreset()
    val look = filmSimLook
    fun param(control: FilmControl.Numeric): FilmSimQuickParam {
        val spec = FilmSimSpecs.forParameter(control.parameter)
        val clamped = look.numericValue(control.parameter).coerceIn(spec.minimum, spec.maximum)
        val epsilon = 0.5f / Math.pow(10.0, spec.decimals.coerceIn(0, 3).toDouble()).toFloat()
        return FilmSimQuickParam(
            key = control.parameter.name,
            shortLabel = FilmSimCatalog.shortLabel(control),
            value = clamped,
            defaultValue = spec.defaultValue,
            minimum = spec.minimum,
            maximum = spec.maximum,
            step = spec.step,
            decimals = spec.decimals,
            displayValue = formatFilmStripValue(clamped, spec.minimum, spec.decimals),
            isDefault = kotlin.math.abs(clamped - spec.defaultValue) <= epsilon
        )
    }
    fun discrete(control: FilmControl.Discrete): FilmSimQuickDiscrete? {
        val options = control.field.options()
        if (options.isEmpty()) return null
        return FilmSimQuickDiscrete(
            key = control.field.name,
            shortLabel = FilmSimCatalog.shortLabel(control),
            options = options,
            selectedIndex = look.discretePosition(control.field).coerceIn(options.indices)
        )
    }
    fun flag(control: FilmControl.Flag): FilmSimQuickFlag =
        FilmSimQuickFlag(
            key = control.flag.name,
            shortLabel = FilmSimCatalog.shortLabel(control),
            enabled = look.flagValue(control.flag)
        )
    return FilmSimQuickState(
        activePresetName = base?.name ?: "Custom",
        selectedPresetId = selectedFilmPresetId,
        modified = isFilmPresetModified(),
        presets = (FilmFactoryPresets.all + filmPresets).map { FilmSimQuickOption(it.id, it.name) },
        sections = CaptureFilmCatalog.sections,
        params = CaptureFilmCatalog.controls.filterIsInstance<FilmControl.Numeric>().associate { control ->
            control.parameter.name to param(control)
        },
        discretes = CaptureFilmCatalog.controls.filterIsInstance<FilmControl.Discrete>().mapNotNull { control ->
            discrete(control)?.let { control.field.name to it }
        }.toMap(),
        flags = CaptureFilmCatalog.controls.filterIsInstance<FilmControl.Flag>().associate { control ->
            control.flag.name to flag(control)
        },
        grainEnabled = look.grainEnabled
    )
}

/** Catalog membership and section geometry are constant across settings edits. */
private object CaptureFilmCatalog {
    // Strip L1/L2 grouping: 1:1 with catalog sections/subsections, except
    // diffusion which merges its camera/print triples into CAMERA / PRINT
    // rows. Membership still comes only from the catalog (see the
    // coverage test); keys/labels below stay stable for test tags.
    val stripSectionMeta = mapOf(
        FilmSimSection.Film to ("film" to "FILM"),
        FilmSimSection.DirCouplers to ("dir" to "DIR"),
        FilmSimSection.Print to ("print" to "PRINT"),
        FilmSimSection.Filters to ("filters" to "FILTERS"),
        FilmSimSection.Diffusion to ("diffusion" to "DIFFUSION"),
        FilmSimSection.Grain to ("grain" to "GRAIN"),
        FilmSimSection.Halation to ("halation" to "HALATION"),
        FilmSimSection.Scanner to ("scanner" to "SCANNER"),
        FilmSimSection.Output to ("output" to "OUTPUT")
    )
    fun stripItems(items: List<FilmControl>): List<FilmStripItem> = items.map { control ->
        when (control) {
            is FilmControl.Numeric -> FilmStripItem.Numeric(control.parameter.name)
            is FilmControl.Discrete -> FilmStripItem.Discrete(control.field.name)
            is FilmControl.Flag -> FilmStripItem.Flag(control.flag.name)
        }
    }
    val controls = FilmSimCatalog.sections.flatMap { section ->
        section.subsections.flatMap { it.controls }
    }
    val sections = FilmSimCatalog.sections.map { section ->
        val (key, label) = stripSectionMeta.getValue(section.section)
        val subsections = if (section.section == FilmSimSection.Diffusion) {
            listOf(
                FilmSimQuickSubsection(
                    "camera", "CAMERA",
                    stripItems(
                        section.subsections.filter { it.key.startsWith("camera-") }
                            .flatMap { it.controls }
                    )
                ),
                FilmSimQuickSubsection(
                    "print", "PRINT",
                    stripItems(
                        section.subsections.filter { it.key.startsWith("print-") }
                            .flatMap { it.controls }
                    )
                )
            )
        } else {
            section.subsections.map { sub ->
                FilmSimQuickSubsection(sub.key, sub.shortLabel, stripItems(sub.controls))
            }
        }
        FilmSimQuickSection(key, label, subsections)
    }
}
