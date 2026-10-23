package com.rawr.camera.renderer

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.window.Dialog
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.rotate
import com.rawr.camera.ui.icons.rounded.ChevronRight
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.sp
import com.rawr.camera.ui.CaptureMono
import com.rawr.camera.ui.CaptureColors
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.selection.selectable
import com.rawr.camera.ui.rememberCaptureHaptics
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.*
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.rawr.camera.model.valueFor
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*
import kotlin.math.roundToInt

// Clip both drawing and hit testing. A draw-only clip leaves scrolled sliders
// interactive outside the pane. All preview images use the same Compose compositor.
internal fun Modifier.clipEditorViewport(): Modifier = clipToBounds()

internal enum class RendererStage { RAW, LINEAR, RENDER, OUTPUT }

// Match the capture strip's instrument typography, with room for full parameter names.
internal val EditorLabelStyle = TextStyle(fontFamily = CaptureMono, fontSize = 13.sp, lineHeight = 18.sp)
internal val EditorValueStyle = EditorLabelStyle.copy(fontWeight = FontWeight.SemiBold)
internal val EditorHeadingStyle = EditorLabelStyle.copy(
    fontSize = 11.sp, fontWeight = FontWeight.Bold, letterSpacing = .6.sp
)
private val EditorMuted = Color.White.copy(alpha = .55f)

@Composable
internal fun EditorNumber(label: String, value: Float, min: Float, max: Float, step: Float = .01f,
                          reset: Float = value, change: (Float) -> Unit) {
    var entry by remember { mutableStateOf<String?>(null) }
    val latestChange by rememberUpdatedState(change)
    val latestValue by rememberUpdatedState(value)
    val haptics = rememberCaptureHaptics()
    var lastHaptic by remember { mutableFloatStateOf(value) }
    val hapticStep = maxOf(step, (max - min) / 40f)
    fun set(v: Float) {
        val next = (min + ((v.coerceIn(min, max) - min) / step).roundToInt() * step).coerceIn(min, max)
        if (kotlin.math.abs(next - lastHaptic) >= hapticStep ||
            (next != lastHaptic && (next == min || next == max))) {
            haptics.sliderTick()
            lastHaptic = next
        }
        latestChange(next)
    }
    val color = CaptureColors.AccentSoft
    val decimals = when { step >= 1f -> 0; step >= .1f -> 1; step >= .01f -> 2; else -> 3 }
    Column(Modifier.fillMaxWidth().padding(top = 6.dp, bottom = 2.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(label, style = EditorLabelStyle, color = EditorMuted,
                modifier = Modifier.weight(1f).clickable { entry = value.toString() }.padding(vertical = 4.dp))
            Text(String.format(java.util.Locale.US, "%.$decimals" + "f", value),
                style = EditorValueStyle, color = color, textAlign = TextAlign.End,
                modifier = Modifier.clickable { entry = value.toString() }.padding(vertical = 4.dp))
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Canvas(Modifier.weight(1f).height(32.dp).semantics {
                contentDescription = label
                progressBarRangeInfo = ProgressBarRangeInfo(value.coerceIn(min, max), min..max)
                setProgress { set(it); true }
            }.pointerInput(min, max, step) {
                detectTapGestures {
                    lastHaptic = latestValue
                    haptics.sliderTick()
                    val inset = 6.dp.toPx()
                    set(min + ((it.x - inset) / (size.width - inset * 2).coerceAtLeast(1f)) * (max - min))
                }
            }.pointerInput(min, max, step) {
                val inset = 6.dp.toPx()
                fun at(x: Float) = min + ((x - inset) / (size.width - inset * 2).coerceAtLeast(1f)) * (max - min)
                detectHorizontalDragGestures(onDragStart = {
                    lastHaptic = latestValue
                    haptics.sliderTick()
                    set(at(it.x))
                }) { event, _ -> event.consume(); set(at(event.position.x)) }
            }) {
                val inset = 6.dp.toPx()
                val x = inset + ((value - min) / (max - min)).coerceIn(0f, 1f) * (size.width - inset * 2)
                val y = size.height / 2
                drawLine(CaptureColors.StrongLine, Offset(inset, y), Offset(size.width - inset, y), 2.dp.toPx())
                drawLine(color.copy(alpha = .65f), Offset(inset, y), Offset(x, y), 2.dp.toPx())
                drawCircle(color, 5.dp.toPx(), Offset(x, y))
            }
            @Composable fun nudge(symbol: String, delta: Float) {
                val target = value + delta
                val enabled = if (delta < 0) value > min else value < max
                Box(Modifier.size(36.dp, 32.dp).clickable(enabled = enabled) { haptics.sliderTick(); lastHaptic = target; set(target) },
                    contentAlignment = Alignment.Center) {
                    Text(symbol, style = EditorValueStyle.copy(fontSize = 18.sp),
                        color = if (enabled) color else EditorMuted.copy(alpha = .3f))
                }
            }
            nudge("−", -step)
            nudge("+", step)
        }
    }
    entry?.let { text ->
        AlertDialog(onDismissRequest = { entry = null }, title = { Text(label) }, text = {
            OutlinedTextField(value = text, onValueChange = { entry = it }, singleLine = true)
        }, confirmButton = {
            TextButton(onClick = { text.toFloatOrNull()?.takeIf { it.isFinite() }?.let(::set); entry = null }) { Text("Set") }
        }, dismissButton = {
            TextButton(onClick = { set(reset); entry = null }) { Text("Reset") }
        })
    }
}

@Composable
internal fun EditorChoice(label: String, selected: String, choices: List<String>, choose: (Int) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        Row(Modifier.fillMaxWidth().heightIn(min = 44.dp).clickable { open = true }.padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(label, style = EditorLabelStyle, color = EditorMuted, modifier = Modifier.weight(1f))
            Text(selected, style = EditorValueStyle, color = CaptureColors.AccentSoft,
                textAlign = TextAlign.End, maxLines = 2, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1.2f))
            Text("⌄", style = EditorLabelStyle, color = EditorMuted)
        }
        if (open) EditorDialog(label, onDismiss = { open = false }) {
            LazyColumn(Modifier.fillMaxWidth().heightIn(max = 360.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp)) {
                itemsIndexed(choices) { i, option ->
                    Row(Modifier.fillMaxWidth()
                        .background(if (option == selected) CaptureColors.Accent.copy(alpha = .12f) else Color.Transparent,
                            RoundedCornerShape(6.dp))
                        .selectable(selected = option == selected, role = Role.RadioButton,
                            onClick = { open = false; choose(i) })
                        .padding(horizontal = 12.dp, vertical = 14.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text(option, style = EditorLabelStyle, modifier = Modifier.weight(1f),
                            color = if (option == selected) CaptureColors.AccentSoft else Color.White)
                        Text(if (option == selected) "✓" else "", modifier = Modifier.width(18.dp),
                            color = CaptureColors.AccentSoft)
                    }
                }
            }
        }
    }
}

@Composable
internal fun EditorToggle(label: String, checked: Boolean, change: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().heightIn(min = 44.dp)
        .toggleable(value = checked, role = Role.Switch, onValueChange = change).padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(label, style = EditorLabelStyle, color = EditorMuted, modifier = Modifier.weight(1f))
        Box(Modifier.width(64.dp)
            .background(if (checked) CaptureColors.Accent.copy(alpha = .16f) else Color.Transparent, RoundedCornerShape(4.dp))
            .border(1.dp, if (checked) CaptureColors.Accent.copy(alpha = .55f) else CaptureColors.StrongLine, RoundedCornerShape(4.dp))
            .padding(vertical = 7.dp), contentAlignment = Alignment.Center) {
            Text(if (checked) "✓ ON" else "OFF", style = EditorHeadingStyle,
                color = if (checked) CaptureColors.AccentSoft else EditorMuted)
        }
    }
}

@Composable
private fun EditorNested(content: @Composable () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(start = 4.dp).drawBehind {
        drawLine(CaptureColors.StrongLine, Offset.Zero, Offset(0f, size.height), 1.dp.toPx())
    }.padding(start = 12.dp, bottom = 4.dp)) { content() }
}

@Composable
private fun EditorSection(title: String, content: @Composable () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(top = 12.dp, bottom = 4.dp)) {
        Text(title.uppercase(java.util.Locale.ROOT), style = EditorHeadingStyle, color = EditorMuted,
            modifier = Modifier.padding(bottom = 6.dp))
        content()
    }
}

@Composable
private fun EditorGroup(title: String, enabled: Boolean? = null, onEnabled: (Boolean) -> Unit = {}, content: @Composable () -> Unit) {
    var expanded by rememberSaveable(title) { mutableStateOf(false) }
    val stateHolder = rememberSaveableStateHolder()
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Row(Modifier.weight(1f).heightIn(min = 44.dp).clickable { expanded = !expanded }
                .semantics { stateDescription = if (expanded) "Expanded" else "Collapsed" }
                .padding(vertical = 8.dp, horizontal = 0.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(title.uppercase(java.util.Locale.ROOT), style = EditorHeadingStyle,
                    color = if (expanded) CaptureColors.AccentSoft else Color.White.copy(alpha = .8f),
                    modifier = Modifier.weight(1f))
                Icon(com.rawr.camera.ui.icons.Icons.Rounded.ChevronRight, null, tint = EditorMuted,
                    modifier = Modifier.padding(horizontal = 10.dp).size(18.dp).rotate(if (expanded) 90f else 0f))
            }
            if (enabled != null) Box(Modifier.width(52.dp).height(44.dp)
                .testTag("renderer_film_toggle_$title")
                .semantics { contentDescription = "$title enabled" }
                .toggleable(enabled, role = Role.Switch, onValueChange = onEnabled), contentAlignment = Alignment.Center) {
                Text(if (enabled) "ON" else "OFF", style = EditorHeadingStyle,
                    color = if (enabled) CaptureColors.AccentSoft else EditorMuted,
                    modifier = Modifier.background(if (enabled) CaptureColors.Accent.copy(alpha = .16f) else Color.Transparent,
                        RoundedCornerShape(4.dp)).border(1.dp, CaptureColors.StrongLine, RoundedCornerShape(4.dp))
                        .padding(horizontal = 8.dp, vertical = 5.dp))
            }
        }
        AnimatedVisibility(expanded) {
            stateHolder.SaveableStateProvider(title) { EditorNested(content) }
        }
    }
}

@Composable
internal fun RendererControls(stage: RendererStage, v: SettingsValues, dispatch: SettingsDispatch,
                              job: RenderJob, resolution: (Double)->Unit, importLut: (String?)->Unit,
                              shadowLift: (AutoKeyExposure)->Unit = {}, localTone: (Float)->Unit = {}) {
    fun send(a: SettingsAction)=dispatch.invoke(a)
    @Composable fun tone(p: ImageToneNumericParameter) {
        val s=p.reviewSpec();val t=v.activeImageTone()
        val value=when(p) {
            ImageToneNumericParameter.WbTemperature->t.wbTemperature
            ImageToneNumericParameter.WbTint->t.wbTint
            ImageToneNumericParameter.JpegQuality->t.jpegQuality
            ImageToneNumericParameter.ColorRenderingStrength->t.colorRenderingStrength
            else->t.valueFor(p.tonemapDescriptor())
        }
        EditorNumber(s.label,value,s.minimum,s.maximum,s.step,s.defaultValue){send(SetNumericValue(p,it))}
    }
    val scroll = rememberScrollState()
    // Padding is inside the scroll surface so the empty gutter also accepts scroll gestures.
    Column(Modifier.fillMaxSize().testTag("renderer_controls_${stage.name}").verticalScroll(scroll).padding(start = 16.dp, end = 64.dp, bottom = 16.dp)) {
        when(stage) {
            RendererStage.RAW -> {
                EditorChoice("Demosaic",v.demosaicAlgorithm.label,DemosaicAlgorithm.entries.map{it.label}){send(SetDemosaicAlgorithm(DemosaicAlgorithm.entries[it]))}
                if(v.demosaicAlgorithm.name.startsWith("Dual")) EditorNested {
                    EditorToggle("Auto contrast",v.dualAutoContrast){send(SetDualAutoContrast(it))}
                    if(!v.dualAutoContrast)EditorNumber("Dual contrast",v.dualContrastPercent,0f,100f,1f,20f){send(SetDualContrastPercent(it))}
                }
                EditorToggle("Lens shading",v.photoLensShadingEnabled){send(SetPhotoLensShadingEnabled(it))}
                EditorToggle("Distortion correction",v.distortionCorrectionEnabled){send(SetDistortionCorrectionEnabled(it))}
                EditorNumber("False color passes",v.photoFccSteps.toFloat(),1f,8f,1f,1f){send(SetPhotoFccSteps(it.toInt()))}
                EditorSection("Sharpening") {
                    EditorToggle("Capture sharpening",v.photoCaptureSharpeningEnabled){send(SetPhotoCaptureSharpeningEnabled(it))}
                    if(v.photoCaptureSharpeningEnabled) EditorNested {
                        EditorChoice("Strength",v.photoCaptureSharpeningLevel.label,CaptureSharpeningLevel.entries.map{it.label}){
                            send(SetPhotoCaptureSharpeningLevel(CaptureSharpeningLevel.entries[it]))
                        }
                    }
                }
            }
            RendererStage.LINEAR -> {
                // Scene-linear gain ahead of tone: RAWR, AgX, LUT and Film Sim all apply it.
                EditorSection("Exposure") { tone(ImageToneNumericParameter.RenderExposure) }
                EditorSection("White balance") {
                    tone(ImageToneNumericParameter.WbTemperature);tone(ImageToneNumericParameter.WbTint)
                }
                EditorSection("Dynamic range") {
                // Dynamic Range, as the camera's: local (exposure fusion), the global
                // curve or off. It runs before the Tone Mapping below (RAWR, AgX, film).
                val shadows = RendererRecipe.localToneShadows(job.draft)
                val lift = RendererRecipe.adaptiveToneMode(job.draft)  // null: older capture, "as captured"
                val method = when {
                    shadows > 0f -> DynamicRangeMethod.Local
                    lift == AutoKeyExposure.Off -> DynamicRangeMethod.Off
                    else -> DynamicRangeMethod.Global
                }
                // Preserve historical local recipes without exposing experimental controls.
                val methodLabel = if (method == DynamicRangeMethod.Local && !LOCAL_TONE_CONTROLS_VISIBLE)
                    "As captured" else method.label
                EditorChoice("Dynamic Range", methodLabel, visibleDynamicRangeMethods.map { it.label }) {
                    when (visibleDynamicRangeMethods[it]) {
                        DynamicRangeMethod.Local -> localTone(LocalToneLimits.DEFAULT_SHADOWS_EV)
                        DynamicRangeMethod.Global -> {
                            localTone(0f)
                            if (lift == AutoKeyExposure.Off) shadowLift(AutoKeyExposure.Medium)
                        }
                        DynamicRangeMethod.Off -> {
                            localTone(0f)
                            shadowLift(AutoKeyExposure.Off)
                        }
                    }
                }
                when (method) {
                    DynamicRangeMethod.Local -> if (LOCAL_TONE_CONTROLS_VISIBLE) EditorNested {
                        EditorNumber("Strength", shadows, LocalToneLimits.MIN_SHADOWS_EV, LocalToneLimits.MAX_SHADOWS_EV,
                            .1f, LocalToneLimits.DEFAULT_SHADOWS_EV) { localTone(it) }
                        val tuning = v.localToneTuning
                        EditorNumber("Highlight protection (EV)", tuning.highlightsEv, .5f, 2f, .1f, 2f) {
                            send(SetLocalTone(tuning = tuning.copy(highlightsEv = it)))
                        }
                        EditorNumber("Exposure selectivity", tuning.sigma, 3f, 5f, .1f, 5f) {
                            send(SetLocalTone(tuning = tuning.copy(sigma = it)))
                        }
                        EditorNumber("Blend scale", (tuning.coarsestMip - 5).toFloat(), 1f, 3f, 1f, 1f) {
                            send(SetLocalTone(tuning = tuning.copy(coarsestMip = it.toInt() + 5)))
                        }
                        EditorToggle("Local contrast boost", tuning.boostLocalContrast) {
                            send(SetLocalTone(tuning = tuning.copy(boostLocalContrast = it)))
                        }
                        TextButton(onClick = {
                            localTone(LocalToneLimits.DEFAULT_SHADOWS_EV)
                            send(SetLocalTone(tuning = LocalToneTuning()))
                        }) { Text("Reset local tone") }
                    }
                    DynamicRangeMethod.Global -> EditorNested {
                        EditorChoice("Strength", lift?.label ?: "As captured", globalDynamicRangeStrengths.map { it.label }) {
                            shadowLift(globalDynamicRangeStrengths[it])
                        }
                    }
                    DynamicRangeMethod.Off -> Unit
                }
                }
                EditorSection("Highlight recovery") {
                EditorToggle("Highlight reconstruction",v.photoHighlightEnabled){send(SetPhotoHighlightEnabled(it))}
                if(v.photoHighlightEnabled) EditorNested {
                    val methods=listOf("Color propagation","Inpaint Opposed")
                    EditorChoice("Method",methods[v.photoHighlightMethod.coerceIn(0,1)],methods){send(SetPhotoHighlightMethod(it))}
                    if(v.photoHighlightMethod==1) {
                        EditorNumber("Threshold",v.photoHighlightThreshold,.5f,2f,.05f,1f){send(SetPhotoHighlightThreshold(it))}
                        EditorNumber("Compression",v.photoHighlightCompression,0f,300f,1f,45f){send(SetPhotoHighlightCompression(it))}
                    }
                }
                }
                EditorSection("Defringe") {
                EditorToggle("Defringe",v.photoDefringeEnabled){send(SetPhotoDefringeEnabled(it))}
                if(v.photoDefringeEnabled) EditorNested {
                    EditorNumber("Strength",v.photoDefringeStrength,0f,1f,.01f){send(SetPhotoDefringeStrength(it))}
                    // Renderer supplies a color matrix and uses Lab defringe. The legacy
                    // camera-RGB edge/luma controls are not consumed by that algorithm.
                }
                }
                EditorSection("Noise reduction") {
                val denoise=v.photoDenoise
                EditorToggle("Denoise",denoise is DenoiseConfig.Wavelet){send(SetPhotoDenoiseEnabled(it))}
                if(denoise is DenoiseConfig.Wavelet) EditorNested {
                    EditorNumber("Strength",denoise.config.strength,0f,8f,.05f,1f){send(SetPhotoDenoiseStrength(it))}
                    EditorNumber("Detail",denoise.config.detail,0f,1.8f,.05f,1f){send(SetPhotoDenoiseDetail(it))}
                    EditorNumber("Luma",denoise.config.luma,0f,1f,.01f,.25f){send(SetPhotoDenoiseLuma(it))}
                    EditorNumber("Scales",denoise.config.scales.toFloat(),1f,7f,1f,7f){send(SetPhotoDenoiseScales(it.toInt()))}
                }
                }
            }
            RendererStage.RENDER -> {
                val mode=if(v.filmSimEnabled)"Film Sim" else if(v.colorRenderProfile==ColorRenderProfile.AgX)"AgX" else "RAWR"
                EditorChoice("Tone Mapping",mode,listOf("RAWR","AgX","Film Sim")){
                    send(SetFilmSimEnabled(it==2))
                    if(it==0)send(SetColorRenderProfile(ColorRenderProfile.RawrBase))
                    if(it==1)send(SetAgxPreset(v.agxPreset))
                }
                val modeState = rememberSaveableStateHolder()
                key(mode) {
                modeState.SaveableStateProvider(mode) {
                if(v.filmSimEnabled) {
                    val presets = (FilmFactoryPresets.all + v.filmPresets).distinctBy { it.id }
                    val presetLabel = v.selectedFilmPreset()?.name?.let {
                        if (v.isFilmPresetModified()) "$it · Modified" else it
                    } ?: "Custom"
                    EditorChoice("Preset", presetLabel, presets.map { it.name }) {
                        send(SelectFilmPreset(presets[it].id))
                    }
                    RendererFilmPresetActions(v, dispatch)
                    FilmSimCatalog.sections.filter { it.section != FilmSimSection.Output }.forEach { section -> RendererFilmGroup(section.section, v.filmSimLook, dispatch) {
                        section.subsections.forEach { sub ->
                          // The group header toggle already owns these enables.
                          val controls = sub.controls.filterNot { it is FilmControl.Flag && it.flag in headerOwnedFilmFlags }
                          if (controls.isEmpty()) return@forEach
                          val title = if (sub.title == "Setup" && section.section == FilmSimSection.Diffusion)
                              if (sub.key.startsWith("camera")) "Camera setup" else "Print setup" else sub.title
                          EditorGroup(title) {
                            controls.forEach { control -> when(control) {
                                is FilmControl.Numeric -> { val s=FilmSimSpecs.forParameter(control.parameter)
                                    EditorNumber(s.label,v.filmSimLook.numericValue(s.parameter),s.minimum,s.maximum,s.step,s.defaultValue){send(SetFilmSimNumericValue(s.parameter,it))} }
                                is FilmControl.Discrete -> { val options=control.field.options()
                                    EditorChoice(editorDisplayName(control.field.name),options.getOrElse(v.filmSimLook.discretePosition(control.field)){"Custom"},options){send(SetFilmSimDiscreteValue(control.field,control.field.storedValue(it)))} }
                                is FilmControl.Flag -> EditorToggle(editorDisplayName(control.flag.name),v.filmSimLook.flagValue(control.flag)){send(SetFilmSimFlag(control.flag,it))}
                            } }
                          }
                        }
                    } }
                } else if(v.colorRenderProfile==ColorRenderProfile.AgX) {
                    RendererAgxLookControls(v, dispatch)
                    AgxSection.entries.forEach { section ->
                        val groups = AgxGroup.entries.filter { it.section == section }
                        val title = when (section) {
                            AgxSection.Range -> "Input range"
                            AgxSection.Curve -> "Tone curve"
                            AgxSection.Look -> "Look"
                            AgxSection.Primaries -> "Color primaries"
                        }
                        EditorGroup(title) {
                            groups.forEach { group ->
                                @Composable fun parameters() {
                                    AgxCatalog.all.filter { it.group == group }.forEach { spec ->
                                        EditorNumber(spec.shortLabel,
                                            (v.agxValues[v.agxPreset] ?: v.agxPreset.defaults).valueOf(spec.parameter),
                                            spec.minimum, spec.maximum, spec.step,
                                            v.agxPreset.defaults.valueOf(spec.parameter)) { send(SetAgxValue(spec.parameter, it)) }
                                    }
                                }
                                if (groups.size == 1) parameters()
                                else EditorGroup(when (group) {
                                    AgxGroup.Curve -> "Basic"
                                    AgxGroup.AdvancedCurve -> "Advanced"
                                    AgxGroup.Before -> "Before tone mapping"
                                    AgxGroup.After -> "After tone mapping"
                                    else -> group.title
                                }) { parameters() }
                            }
                        }
                    }
                } else {
                    val profiles=listOf("RAWR","sRGB")+v.userLutProfiles.map{it.name}
                    EditorChoice("Profile",v.activeProfileLabel(),profiles){when(it){0->send(SetColorRenderProfile(ColorRenderProfile.RawrBase));1->send(SetColorRenderProfile(ColorRenderProfile.SRgb));else->send(SelectUserLutProfile(v.userLutProfiles[it-2].id))}}
                    EditorSection("Contrast") {
                        listOf(ImageToneNumericParameter.Contrast, ImageToneNumericParameter.MidtonePivot).forEach { tone(it) }
                    }
                    EditorGroup("Tonal range") {
                        listOf(ImageToneNumericParameter.BlackToe, ImageToneNumericParameter.Shadows,
                            ImageToneNumericParameter.Highlights, ImageToneNumericParameter.ShoulderWhitePoint).forEach { tone(it) }
                    }
                    EditorGroup("Color") {
                        tone(ImageToneNumericParameter.Saturation)
                        tone(ImageToneNumericParameter.Vibrance)
                        tone(ImageToneNumericParameter.ColorRenderingStrength)
                    }
                    v.userLutProfiles.firstOrNull { it.id == v.selectedUserLutProfileId }?.let { profile ->
                        EditorGroup("LUT profile") {
                            EditorChoice("Input gamut",profile.inputGamut.label,LutGamut.entries.map{it.label}){send(SetUserLutInputGamut(profile.id,LutGamut.entries[it]))}
                            EditorChoice("Input transfer",profile.inputTransfer.label,LutTransfer.entries.map{it.label}){send(SetUserLutInputTransfer(profile.id,LutTransfer.entries[it]))}
                            EditorChoice("Output gamut",profile.outputGamut.label,LutGamut.entries.map{it.label}){send(SetUserLutOutputGamut(profile.id,LutGamut.entries[it]))}
                            EditorChoice("Output transfer",profile.outputTransfer.label,LutTransfer.entries.map{it.label}){send(SetUserLutOutputTransfer(profile.id,LutTransfer.entries[it]))}
                            EditorChoice("After LUT",profile.afterLut.label,AfterLutAction.entries.map{it.label}){send(SetUserLutAfterAction(profile.id,AfterLutAction.entries[it]))}
                            profile.stages.forEach { lut ->
                                EditorChoice(lut.fileName,"Edit",listOf("Move up","Move down","Remove")){when(it){
                                    0->send(MoveUserLutStage(profile.id,lut.id,-1))
                                    1->send(MoveUserLutStage(profile.id,lut.id,1))
                                    else->send(RemoveUserLutStage(profile.id,lut.id))
                                }}
                            }
                        }
                    }
                    TextButton(onClick={importLut(v.selectedUserLutProfileId)}){Text("+ Import LUT",style=EditorHeadingStyle,color=CaptureColors.AccentSoft)}
                }
                }
            }
            }
            RendererStage.OUTPUT -> {
                val choices=RenderResolution.choices(job.width,job.height,job.rawr)
                val dims=RenderResolution.dimensions(job.width,job.height,job.megapixels)
                EditorSection("Image size") {
                    EditorChoice("Resolution","${dims.first} × ${dims.second}",choices.map{if(it==0.0)"Native" else "$it MP"}){resolution(choices[it])}
                }
                EditorSection("JPEG encoding") {
                tone(ImageToneNumericParameter.JpegQuality)
                listOf(ChoiceSelectorKind.JpegChromaSubsampling).forEach { kind ->
                    val caps=SettingsCatalog.initialState().capabilities
                    val options=when(kind) {
                        ChoiceSelectorKind.JpegChromaSubsampling->caps.jpegChromaSubsamplingChoices
                        ChoiceSelectorKind.OutputColorSpace->caps.outputColorSpaces
                        else->caps.transferFunctions
                    }
                    val selected=when(kind){ChoiceSelectorKind.JpegChromaSubsampling->v.imageTone.jpegChromaSubsamplingId;ChoiceSelectorKind.OutputColorSpace->v.imageTone.outputColorSpaceId;else->v.imageTone.transferFunctionId}
                    EditorChoice(when(kind) { ChoiceSelectorKind.JpegChromaSubsampling -> "Chroma subsampling"; ChoiceSelectorKind.OutputColorSpace -> "Color space"; else -> "Transfer function" },options.firstOrNull{it.id==selected}?.label?:selected,options.map{it.label}){send(SetChoice(kind,options[it].id))}
                }
                EditorToggle("UltraHDR",v.ultraHdrEnabled){send(SetUltraHdrEnabled(it))}
                }
            }
        }
    }
}

/** Enables the RendererFilmGroup header switch already drives. */
private val headerOwnedFilmFlags = setOf(FilmSimFlag.GrainEnabled, FilmSimFlag.HalationEnabled, FilmSimFlag.ScannerEnabled)

private fun editorDisplayName(name: String): String =
    name.replace(Regex("([a-z])([A-Z])"), "$1 $2").replace('_', ' ')

@Composable
internal fun EditorDialog(title: String, onDismiss: () -> Unit, content: @Composable ColumnScope.() -> Unit) {
    Dialog(onDismissRequest = onDismiss) {
        Column(Modifier.fillMaxWidth().background(CaptureColors.Panel, RoundedCornerShape(12.dp))
            .border(1.dp, CaptureColors.StrongLine, RoundedCornerShape(12.dp)).padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Text(title.uppercase(java.util.Locale.ROOT), style = EditorHeadingStyle, color = CaptureColors.AccentSoft)
            HorizontalDivider(color = CaptureColors.Line)
            content()
            HorizontalDivider(color = CaptureColors.Line)
            TextButton(onClick = onDismiss, modifier = Modifier.align(Alignment.End), contentPadding = PaddingValues(horizontal = 12.dp)) {
                Text("CLOSE", style = EditorHeadingStyle)
            }
        }
    }
}

/** Quick effect switches remain available when a section is collapsed. Off/on
 * restores the last DIR amount and multi-effect selection during this edit. */
@Composable
internal fun RendererFilmGroup(section: FilmSimSection, look: FilmSimLook, dispatch: SettingsDispatch,
    content: @Composable () -> Unit) {
    var dirAmount by rememberSaveable { mutableFloatStateOf(1f) }
    var flagMask by rememberSaveable { mutableIntStateOf(0) }
    val flags = rendererFilmSectionFlags(section)
    val enabled = when (section) {
        FilmSimSection.DirCouplers -> look.dirCouplersAmount > 0f
        FilmSimSection.Print -> look.process == 0
        else -> flags.takeIf { it.isNotEmpty() }?.any { look.flagValue(it) }
    }
    EditorGroup(section.title, enabled, { on ->
        when (section) {
            FilmSimSection.DirCouplers -> {
                if (!on) dirAmount = look.dirCouplersAmount
                dispatch.invoke(SetFilmSimNumericValue(FilmSimNumericParameter.DirCouplersAmount,
                    if (on) dirAmount.takeIf { it > 0f } ?: 1f else 0f))
            }
            FilmSimSection.Print -> dispatch.invoke(SetFilmSimDiscreteValue(FilmSimDiscreteField.Process, if (on) 0 else 1))
            else -> {
                if (!on) flagMask = flags.foldIndexed(0) { index, mask, flag ->
                    if (look.flagValue(flag)) mask or (1 shl index) else mask
                }
                flags.forEachIndexed { index, flag ->
                    dispatch.invoke(SetFilmSimFlag(flag, on && (flagMask == 0 || flagMask and (1 shl index) != 0)))
                }
            }
        }
    }, content)
}

private fun rendererFilmSectionFlags(section: FilmSimSection): List<FilmSimFlag> = when (section) {
    FilmSimSection.Filters -> listOf(FilmSimFlag.CameraUvEnabled, FilmSimFlag.CameraIrEnabled)
    FilmSimSection.Diffusion -> listOf(FilmSimFlag.CameraDiffusionEnabled, FilmSimFlag.PrintDiffusionEnabled)
    FilmSimSection.Grain -> listOf(FilmSimFlag.GrainEnabled)
    FilmSimSection.Halation -> listOf(FilmSimFlag.HalationEnabled)
    FilmSimSection.Scanner -> listOf(FilmSimFlag.ScannerEnabled)
    else -> emptyList()
}
