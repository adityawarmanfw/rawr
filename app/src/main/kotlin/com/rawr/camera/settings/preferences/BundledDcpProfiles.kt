package com.rawr.camera.settings.preferences

import android.content.Context
import com.rawr.camera.settings.model.LensDcp
import com.rawr.camera.settings.model.LensProfile
import java.io.File

/** MaZe's original, unmodified X300 Ultra profiles. Permission: docs/permissions/maze-x300u.md. */
internal object BundledDcpProfiles {
    const val VERSION = 1
    const val DEVICE = "vivo_x300_ultra"
    const val ASSET_DIR = "dcp_profiles/maze_x300u"
    data class Entry(val cameraId: String, val asset: String, val profile: LensDcp)
    val entries = listOf(
        Entry("4", "X300U_RAWR14_5600_Sun.dcp", LensDcp("4944ca8ec2622bab0d29b2e2ed003910.dcp", "5600 Sun · MaZe")),
        Entry("4", "X300U_RAWR14_2700_Bulb.dcp", LensDcp("a9cac7e4ca57a4ce2d9bb8606c9f926d.dcp", "2700 Bulb · MaZe")),
        Entry("4", "X300U_RAWR14_2700_LED.dcp", LensDcp("48712ce873bbb023424348df6afeb9dd.dcp", "2700 LED · MaZe")),
        Entry("4", "X300U_RAWR14_Sun-Bulb.dcp", LensDcp("c18f320104d9c357ac8d65edd1d3f16d.dcp", "Sun-Bulb · MaZe")),
        Entry("4", "X300U_RAWR14_Sun-LED.dcp", LensDcp("2f886bb93e3f275c3967f9552c1bc9dc.dcp", "Sun-LED · MaZe")),
        Entry("3", "X300U_RAWR35_5600_Sun.dcp", LensDcp("fb6997220daf6bbfc7970c501181d3a2.dcp", "5600 Sun · MaZe")),
        Entry("3", "X300U_RAWR35_2700_Bulb.dcp", LensDcp("5ca63fb441fa5f6b8e0ab734f8da74df.dcp", "2700 Bulb · MaZe")),
        Entry("3", "X300U_RAWR35_2700_LED.dcp", LensDcp("911b32e97e6dfdcd22171bbff8ba9e86.dcp", "2700 LED · MaZe")),
        Entry("3", "X300U_RAWR35_Sun-Bulb.dcp", LensDcp("dfb78b0f4bee8d17d3cb8d2ff6d69f14.dcp", "Sun-Bulb · MaZe")),
        Entry("3", "X300U_RAWR35_Sun-LED.dcp", LensDcp("dbb0118c7232a95090c9f760553e8c6e.dcp", "Sun-LED · MaZe")),
        Entry("5", "X300U_RAWR85_5600_Sun.dcp", LensDcp("b4f227dc588d8d7f9913f0be99eaaddb.dcp", "5600 Sun · MaZe")),
        Entry("5", "X300U_RAWR85_2700_Bulb.dcp", LensDcp("7cbe94e10b67ec2df435392fed2c882d.dcp", "2700 Bulb · MaZe")),
        Entry("5", "X300U_RAWR85_2700_LED.dcp", LensDcp("12937ac6d6ce03e8f68615f04ea05473.dcp", "2700 LED · MaZe")),
        Entry("5", "X300U_RAWR85_Sun-Bulb.dcp", LensDcp("9aee8be4642df15d40f605c9af8ecfd6.dcp", "Sun-Bulb · MaZe")),
        Entry("5", "X300U_RAWR85_Sun-LED.dcp", LensDcp("2c66fe7676f5e148020b1667b30956af.dcp", "Sun-LED · MaZe"))
    )

    /** Known original files get the same author-suffix UI label on import and backup restore too. */
    fun reference(file: String, fallbackName: String): LensDcp =
        entries.firstOrNull { it.profile.file == file }?.profile ?: LensDcp(file, fallbackName)

    fun defaults(deviceId: String, lenses: List<LensProfile>): List<LensProfile> =
        if (deviceId != DEVICE) lenses else lenses.map { lens ->
            val profiles = entries.filter { it.cameraId == lens.camera.id && lens.camera.physical.isEmpty() }.map { it.profile }
            if (profiles.isEmpty()) lens else lens.image { copy(dcpProfiles = profiles) }
        }

    /** Installs before settings reach backend consumers, using the same content names as imports/backups. */
    fun install(context: Context, defaults: List<LensProfile>) {
        install(DcpFileStore(context), defaults) { asset ->
            context.assets.open("$ASSET_DIR/$asset").use { it.readBytes() }
        }
    }

    internal fun install(store: DcpFileStore, defaults: List<LensProfile>, readAsset: (String) -> ByteArray) {
        val needed = defaults.flatMap { it.image.availableDcps }.map { it.file }.toSet()
        if (needed.isEmpty()) return
        entries.filter { it.profile.file in needed }.forEach { entry ->
            val target = File(store.dir, entry.profile.file)
            if (!target.isFile || DcpFileStore.fileName(target.readBytes()) != entry.profile.file) {
                val bytes = readAsset(entry.asset)
                check(DcpFileStore.fileName(bytes) == entry.profile.file) { "Damaged bundled DCP: ${entry.asset}" }
                check(store.add(bytes).file == entry.profile.file)
            }
        }
    }

    /** Adds choices without replacing user lens settings, imported profiles, or the active selection. */
    fun merge(lenses: List<LensProfile>, defaults: List<LensProfile>): List<LensProfile> {
        val choices = defaults.groupBy { it.camera }.mapValues { (_, routes) ->
            routes.flatMap { it.image.availableDcps }.distinctBy { it.file }
        }
        return lenses.map { lens ->
            val profiles = choices[lens.camera].orEmpty()
            if (profiles.isEmpty()) lens else lens.image {
                copy(dcpProfiles = (availableDcps + profiles).distinctBy { it.file })
            }
        }
    }
}
