package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.*
import java.net.URLDecoder
import java.net.URLEncoder
import java.nio.charset.StandardCharsets

internal object LutProfileCodec {
    private fun enc(s: String) = URLEncoder.encode(s, StandardCharsets.UTF_8.name())

    private fun dec(s: String) = URLDecoder.decode(s, StandardCharsets.UTF_8.name())

    fun encode(profiles: List<ImportedLutProfile>): String = profiles.joinToString("\n") { p ->
        val stages =
            p.stages.joinToString(
                ","
            ) { s -> listOf(s.id, s.fileName, s.relativePath).joinToString("~") { enc(it) } }
        listOf(
            enc(p.id),
            enc(p.name),
            p.inputGamut.name,
            p.inputTransfer.name,
            p.outputGamut.name,
            p.outputTransfer.name,
            p.afterLut.name,
            stages,
            encodeTone(p.tone)
        ).joinToString("|")
    }

    fun decode(raw: String?): List<ImportedLutProfile> = raw
        .orEmpty()
        .lineSequence()
        .filter { it.isNotBlank() }
        .mapNotNull { line ->
            runCatching {
                // v23 adds a 9th pipe field carrying the per-profile tone.
                // 8-field lines are pre-per-profile-tone payloads → neutral tone.
                val a = line.split('|', limit = 9)
                require(a.size == 8 || a.size == 9)
                val stages =
                    if (a[7].isBlank()) {
                        emptyList()
                    } else {
                        a[7].split(',').map { token ->
                            val s = token.split('~', limit = 3)
                            require(s.size == 3)
                            ImportedLutStage(dec(s[0]), dec(s[1]), dec(s[2]))
                        }
                    }
                ImportedLutProfile(
                    dec(a[0]),
                    dec(a[1]),
                    stages,
                    LutGamut.valueOf(a[2]),
                    LutTransfer.valueOf(a[3]),
                    LutGamut.valueOf(a[4]),
                    LutTransfer.valueOf(a[5]),
                    AfterLutAction.valueOf(a[6]),
                    if (a.size == 9) decodeTone(a[8]) else ProfileTone.Neutral
                )
            }.getOrNull()
        }.toList()

    /** A backup must not silently lose records that the normal preference decoder tolerates. */
    fun decodeBackup(raw: String): List<ImportedLutProfile> {
        val lines = raw.lineSequence().filter { it.isNotBlank() }.toList()
        val profiles = decode(raw)
        require(profiles.size == lines.size) { "Backup contains malformed LUT profiles" }
        require(profiles.map { it.id }.distinct().size == profiles.size) { "Backup contains duplicate LUT profiles" }
        lines.forEach { line ->
            val fields = line.split('|', limit = 9)
            if (fields.size == 9) {
                val tone = fields[8].split(',').map { it.toFloatOrNull() }
                require(tone.size == 9 && tone.withIndex().all { (index, value) ->
                    value != null && value.isFinite() &&
                        value in (if (index == 0) -5f..5f else -100f..100f)
                }) { "Backup contains malformed LUT tone parameters" }
            }
        }
        return profiles
    }

    internal fun encodeTone(tone: ProfileTone): String = listOf(
        tone.renderExposure,
        tone.blacks,
        tone.shadows,
        tone.contrast,
        tone.midtones,
        tone.highlights,
        tone.whites,
        tone.saturation,
        tone.vibrance
    ).joinToString(",")

    internal fun decodeTone(raw: String): ProfileTone {
        if (raw.isBlank()) return ProfileTone.Neutral
        val parts = raw.split(',').mapNotNull { it.toFloatOrNull() }
        if (parts.size != 9) return ProfileTone.Neutral
        fun finite(v: Float, min: Float, max: Float, fallback: Float) =
            if (v.isFinite() && v in min..max) v else fallback
        val n = ProfileTone.Neutral
        return ProfileTone(
            renderExposure = finite(parts[0], -5f, 5f, n.renderExposure),
            blacks = finite(parts[1], -100f, 100f, n.blacks),
            shadows = finite(parts[2], -100f, 100f, n.shadows),
            contrast = finite(parts[3], -100f, 100f, n.contrast),
            midtones = finite(parts[4], -100f, 100f, n.midtones),
            highlights = finite(parts[5], -100f, 100f, n.highlights),
            whites = finite(parts[6], -100f, 100f, n.whites),
            saturation = finite(parts[7], -100f, 100f, n.saturation),
            vibrance = finite(parts[8], -100f, 100f, n.vibrance)
        )
    }
}
