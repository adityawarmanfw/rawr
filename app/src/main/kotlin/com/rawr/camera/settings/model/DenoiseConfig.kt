package com.rawr.camera.settings.model

/**
 * Still-pipeline denoise configuration, single source of truth.
 *
 * Wire values stay identical to the legacy fields (master bool + wavelet
 * strengths), so recipe JSON and prefs keys are untouched. Only the
 * settings-layer ownership lives here.
 */

/** Profiled wavelet denoise (pre-WB linear, needs a sensor noise profile). */
data class WaveletDenoise(
    val strength: Float = 1f,
    val detail: Float = 1f,
    // Luma-band force (Y0 threshold scale, shipped tuning 0.25; higher
    // smooths luma grain harder, chroma force stays fixed at 0.75).
    val luma: Float = 0.25f,
    // Wavelet band count (coarsest bands dropped first: faster, but
    // large-scale chroma-blotch cleanup goes first).
    val scales: Int = 7
)

sealed interface DenoiseConfig {
    data object Off : DenoiseConfig

    data class Wavelet(val config: WaveletDenoise = WaveletDenoise()) : DenoiseConfig

    companion object {
        /**
         * Repairs legacy fields into a valid config. Master off parks
         * everything; anything else is wavelet. Out-of-range ints and
         * non-finite floats are clamped, never thrown.
         */
        fun fromLegacy(
            enabled: Boolean,
            waveletStrength: Float,
            waveletDetail: Float,
            waveletLuma: Float = 0.25f,
            waveletScales: Int = 7
        ): DenoiseConfig {
            if (!enabled) return Off
            return Wavelet(
                WaveletDenoise(
                    strength = waveletStrength.coerceIn(0f, 8f),
                    detail = waveletDetail.coerceIn(0f, 1.8f),
                    luma = waveletLuma.coerceIn(0f, 1f),
                    scales = waveletScales.coerceIn(1, 7)
                )
            )
        }
    }
}

/** Master-switch transition. ON keeps the current config; Off enables as wavelet. */
fun DenoiseConfig.withMaster(enabled: Boolean): DenoiseConfig =
    if (enabled) {
        if (this is DenoiseConfig.Off) DenoiseConfig.Wavelet() else this
    } else {
        DenoiseConfig.Off
    }

/** Strength sliders only exist while wavelet is on; otherwise no-op
 * (the UI disables them, so this is unreachable in practice). */
fun DenoiseConfig.withWaveletStrength(value: Float): DenoiseConfig =
    if (this is DenoiseConfig.Wavelet) {
        copy(config = config.copy(strength = value.coerceIn(0f, 8f)))
    } else {
        this
    }

fun DenoiseConfig.withWaveletDetail(value: Float): DenoiseConfig =
    if (this is DenoiseConfig.Wavelet) {
        copy(config = config.copy(detail = value.coerceIn(0f, 1.8f)))
    } else {
        this
    }

fun DenoiseConfig.withWaveletLuma(value: Float): DenoiseConfig =
    if (this is DenoiseConfig.Wavelet) {
        copy(config = config.copy(luma = value.coerceIn(0f, 1f)))
    } else {
        this
    }

fun DenoiseConfig.withWaveletScales(value: Int): DenoiseConfig =
    if (this is DenoiseConfig.Wavelet) {
        copy(config = config.copy(scales = value.coerceIn(1, 7)))
    } else {
        this
    }

/** Capture path capabilities the resolver decides on. Device gates (float16
 * compute, geometry size) stay native with SKIP emits; this covers the
 * path-level decisions that used to be re-derived at every tap site. */
enum class StillPath {
    SINGLE,
    MULTIFRAME
}

/**
 * Exactly what the still pipeline may run, after path arbitration. Every
 * method is off on the merged path: the multiframe merge does its own
 * denoising.
 */
data class ResolvedDenoise(
    val waveletStrength: Float = 0f,
    val waveletDetail: Float = 1f,
    val waveletLuma: Float = 0.25f,
    val waveletScales: Int = 7,
    val skipped: List<String> = emptyList()
)

/**
 * Single path arbitration point. Native keeps its device gates as backstop.
 * The dedicated denoiser is single-frame only: a multiframe merge already
 * denoises (burst-fitted noise model plus fallback cleanup of motion areas),
 * so it resolves to off on the merged path.
 */
fun DenoiseConfig.resolve(path: StillPath): ResolvedDenoise {
    if (path == StillPath.MULTIFRAME && this !is DenoiseConfig.Off) {
        return ResolvedDenoise(skipped = listOf("single_frame_only_multiframe_denoises"))
    }
    return when (this) {
        is DenoiseConfig.Off -> ResolvedDenoise()
        is DenoiseConfig.Wavelet ->
            ResolvedDenoise(
                waveletStrength = config.strength,
                waveletDetail = config.detail,
                waveletLuma = config.luma,
                waveletScales = config.scales
            )
    }
}

/**
 * JNI spec arrays: the collapsed form of the resolved intent at the
 * Kotlin->native boundary. Layout is fixed; native validates lengths:
 *
 * modes[2]     = [master, waveletScales]
 * strengths[3] = [waveletStrength, waveletDetail, waveletLuma]
 */
fun ResolvedDenoise.toModesArray(): IntArray =
    intArrayOf(if (waveletStrength > 0f) 1 else 0, waveletScales.coerceIn(1, 7))

fun ResolvedDenoise.toStrengthsArray(): FloatArray =
    floatArrayOf(waveletStrength, waveletDetail, waveletLuma.coerceIn(0f, 1f))

/**
 * Legacy field decomposition for the pass-through plumbing (prefs keys,
 * recipe JSON). Keys and wire values are unchanged.
 */
data class LegacyDenoise(
    val enabled: Boolean,
    val waveletStrength: Float,
    val waveletDetail: Float,
    val waveletLuma: Float,
    val waveletScales: Int
)

fun DenoiseConfig.toLegacy(): LegacyDenoise = when (this) {
    is DenoiseConfig.Off -> LegacyDenoise(
        enabled = false, waveletStrength = 1f, waveletDetail = 1f, waveletLuma = 0.25f, waveletScales = 7
    )
    is DenoiseConfig.Wavelet -> LegacyDenoise(
        enabled = true, waveletStrength = config.strength, waveletDetail = config.detail,
        waveletLuma = config.luma, waveletScales = config.scales
    )
}
