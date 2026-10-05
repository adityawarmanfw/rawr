# Third-Party Notices

Rawr is distributed as **GPL-3.0-only** (see `LICENSE` and `NOTICE`). It links
and/or adapts the components below. Copyright in each component remains with its
respective authors.

Where a component's full license text is vendored in this repository, the path is
given. MIT, BSD-2-Clause and BSD-3-Clause texts are reproduced in full at the end
of this file. The GNU GPL v3 text is the repository `LICENSE`.

## 1. Components linked into the shipped application

| Component | Used in | Upstream | License |
|---|---|---|---|
| darktable (`denoiseprofile`) | `native/raw_denoise` | https://github.com/darktable-org/darktable | GPL-3.0-or-later |
| RawTherapee (dual demosaic, Coloropp highlight, false-colour correction) | `native/raw_demosaic/dual`, `native/raw_highlight`, `native/false_color_correction` | https://github.com/RawTherapee/RawTherapee | GPL-3.0-or-later |
| librtprocess (RCD, VNG4 demosaic) | `native/raw_demosaic/rcd`, `native/raw_demosaic/vng4` | https://github.com/CarVac/librtprocess | GPL-3.0-or-later |
| spektrafilm / spektrafilm-ofx | `native/spektrafilm`, `app/src/main/assets/spektrafilm` | https://github.com/chaert-s/spektrafilm-ofx , https://github.com/andreavolpato/spektrafilm | GPL-3.0-only |
| Handheld Multi-Frame Super-Resolution (Wronski et al.) | `native/multiframe` | https://github.com/Jamy-L/Handheld-Multi-Frame-Super-Resolution | MIT |
| hdr-plus-swift / Burst Photo (HDR+ spatial merge) | `native/multiframe/merge_hdrplus` | https://github.com/martin-marek/hdr-plus-swift | GPL-3.0 |
| GALOSH | `native/galosh` | https://github.com/luxgrain/GALOSH | Apache-2.0 |
| libultrahdr (Ultra HDR / JPEG_R mux) | `native/gainmap` | https://github.com/google/libultrahdr | Apache-2.0 |
| Oklab gamut clipping (Björn Ottosson) | `native/tonemap_engine` | https://bottosson.github.io/posts/gamutclipping/ | MIT |
| tinydng | `native/tinydng` | https://github.com/syoyo/tinydng | MIT |
| libjpeg-turbo | `native/libjpeg-turbo` | https://github.com/libjpeg-turbo/libjpeg-turbo | IJG + BSD-3-Clause |
| libadrenotools | build-time fetch (Adreno driver loading) | https://github.com/bylaws/libadrenotools | BSD-2-Clause |
| AndroidX, Jetpack Compose, Media3 | Gradle dependencies | https://developer.android.com/jetpack | Apache-2.0 |
| Kotlin standard library, kotlinx.coroutines | Gradle dependencies | https://kotlinlang.org | Apache-2.0 |
| Reorderable (Compose drag-to-reorder) | Gradle dependency | https://github.com/Calvin-LL/Reorderable | Apache-2.0 |

Vendored full license texts in this repository:

- libjpeg-turbo: `native/libjpeg-turbo/LICENSE.md` (+ `README.ijg`, which carries
  the IJG License text)
- GALOSH: `native/galosh/UPSTREAM_LICENSE` (Apache-2.0),
  `native/galosh/UPSTREAM_NOTICE`
- tinydng: `native/tinydng/LICENSE`
- Redistributed Gradle dependencies: license metadata is packaged in each
  dependency's own artifact.

Bundled into the app (`app/src/main/assets/licenses/`) and shown by the in-app
Legal notices screen: `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md`,
`Apache-2.0.txt`, `galosh-NOTICE.txt`, `libjpeg-turbo-LICENSE.md`,
`libjpeg-turbo-README.ijg`, `tinydng-LICENSE.txt`.

## 2. Copyright notices

- **darktable** — Copyright (C) the darktable developers. GPL-3.0-or-later.
- **RawTherapee** — Copyright (C) the RawTherapee contributors. GPL-3.0-or-later.
- **librtprocess** — Copyright (C) the librtprocess contributors; RCD algorithm
  by Luis Sanz Rodríguez. GPL-3.0-or-later.
- **spektrafilm / spektrafilm-ofx** — Copyright (C) Aedan Diez; profiles
  Copyright (C) 2026 Andrea Volpato. GPL-3.0-only (code); CC BY-SA 4.0
  (profiles).
- **Handheld Multi-Frame Super-Resolution** — Copyright (c) 2023 Jamy Lafenetre
  (jamy.lafenetre@ens-paris-saclay.fr). MIT.
- **GALOSH** — Copyright 2026 luxgrain. Apache-2.0.
- **libultrahdr** — Copyright (c) Google LLC. Apache-2.0.
- **Oklab gamut clipping** — Copyright (c) Björn Ottosson. MIT.
- **tinydng** — Copyright (c) 2016-Present Syoyo Fujita and many contributors.
  MIT.
- **libjpeg-turbo** — IJG License, Copyright (C) 1991-2020 Thomas G. Lane and
  Guido Vollbeding; Modified BSD License, Copyright (C) 2009-2026 D. R. Commander
  and others. This software is based in part on the work of the Independent JPEG
  Group.
- **libadrenotools** — Copyright (c) 2021 Billy Laws. BSD-2-Clause.
- **AndroidX, Jetpack Compose, Media3** — Copyright (c) Google LLC. Apache-2.0.
- **Kotlin standard library, kotlinx.coroutines** — Copyright (c) JetBrains
  s.r.o. and contributors. Apache-2.0.

## 3. Validation tooling (in-repo, not linked into the app)

- `native/raw_denoise/tools/denoiseprofile_oracle.py` — port of darktable's
  `denoiseprofile.c` / `eaw.c`. GPL-3.0-or-later.
- `native/false_color_correction/tools/fcc_reference.py` — port of RawTherapee's
  false-colour suppression. GPL-3.0-or-later.
- `tools/defringe_reference.py` — original Rawr reference for the in-house
  defringe stage.
- `tools/video_demosaic_moltenvk/` uses LibRaw/rawpy (LGPL-2.1/CDDL) for AHD
  reference generation; not distributed.

## 4. Build-time-only dependencies (not linked into the app)

- **colour-science** (https://www.colour-science.org) — BSD-3-Clause, used by
  `native/spektrafilm/tools`-derived profile generation.
- **NumPy** (https://numpy.org) — BSD-3-Clause, validation tooling.
- **glslc / Shaderc** — Apache-2.0, shader compilation.
- **CMake**, **Gradle**, **Android NDK**, **Ninja** — their respective licenses.

## 5. spektrafilm provenance and data attribution

The film-simulation module and its build-time-generated data are derived from
Aedan Diez's `spektrafilm-ofx`, which builds on Andrea Volpato's `spektrafilm`:

- **Film profile data** (shipped as `app/src/main/assets/spektrafilm/*.f32` and
  compiled into `native/spektrafilm/generated/SpektraGeneratedProfileCurves.cpp`)
  descends from Andrea Volpato's spektrafilm profiles, licensed
  **CC BY-SA 4.0**. Copyright (c) 2026 Andrea Volpato. Derivative profile data
  remains under CC BY-SA 4.0. CC BY-SA 4.0 is one-way compatible with GPLv3.
  Full text: https://github.com/andreavolpato/spektrafilm/blob/main/SPEKTRAFILM_LICENSE.txt
- **Spectral upsampling direction**: Johannes Hanika's Hanatos 2025 models and
  the hanatos/vkdt project — https://github.com/hanatos/vkdt
- **Alternate spectral reconstruction**: Mallett & Yuksel 2019 —
  https://diglib.eg.org/items/bbffa865-e99c-4c1f-bd33-70102dc8af78
- **Pinned upstream revision: `chaert-s/spektrafilm-ofx` @ `86476af` (2026-07-06).**
  Seven of the ten vendored shaders are byte-identical to that revision; the
  other three (`SpektraDiffusion.comp`, `SpektraPrintScan.comp`,
  `SpektraGrain.comp`) are locally modified. The generated data was reproduced
  exactly from that revision's `tools/generate_profile_curves.py` (hashes below).

### Provenance record (content hashes)

| Artifact | SHA-256 |
|---|---|
| `app/src/main/assets/spektrafilm/SpektraHanatos2025Spectra.f32` | `d615aaedaf269d07b14a351c0c9d54dc7c1df55e9da5f96246e4a737e0fe320f` |
| `app/src/main/assets/spektrafilm/SpektraOutputGamutCompression.f32` | `d633700344a0e47f6729e14d20418b7048695ea22fecc59dc30f23f66f3956f6` |
| `native/spektrafilm/generated/SpektraGeneratedProfileCurves.cpp` | `4e500d952b2b586da3862b7b32802075a77bdf81722f8ce58dea73a558fcc3f4` |
| `native/spektrafilm/generated/SpektraGeneratedProfileCounts.h` | `d3fa7033fe4b506bef4c8075a1498c1d62490d35c5e0e397972e42a123806da2` |

These hashes were reproduced by running `tools/generate_profile_curves.py` at
commit `86476af`, consuming `Resources/data/profiles/*.json` and
`Resources/data/luts/spectral_upsampling/irradiance_xy_tc.npy`. To regenerate:
check out `86476af`, install `numpy`/`scipy`/`colour-science`, and run the CLI
declared in the upstream `CMakeLists.txt` (`--output`, `--counts-output`,
`--hanatos-output`, `--output-gamut-compression-output`).

The upstream "licensed standards data" (SMPTE ST 2065-2 CSVs, used only for the
Academy Printer Density path) is **not** present in this repository; the Rawr
generated profile counts keep that path disabled.

## 6. Algorithm credits

Independent Rawr implementations of published methods, credited for provenance:

- **MHC 5×5 demosaic** (`native/video_pipeline`, `tools/video_demosaic_moltenvk`):
  Malvar, He, Cutler (2004).
- **Handheld MFSR** (`native/multiframe`): Wronski et al., SIGGRAPH 2019.
- **Ultra HDR gain map** (`native/gainmap`): ISO 21496-1.

## 7. Outstanding items before release

Done:

- In-app "Legal notices" screen (About → Open-source licenses) showing this file,
  `NOTICE`, and the GNU GPL v3 text; texts bundled at
  `app/src/main/assets/licenses/`.
- Copyright holder set in `NOTICE` ("adityawarmanfw").
- spektrafilm source and generated data pinned to spektrafilm-ofx `86476af`
  (see §5).

Still open:

- Publish the Corresponding Source and tag the release revision it was built from
  (GPL §6d); link the app to it.
- Release signing: `app/build.gradle.kts` still uses debug signing with
  `isDebuggable = true`.
- Lint: pre-existing findings are grandfathered in `app/lint-baseline.xml`; burn
  them down over time.

---

## MIT License

Copyright held by the authors listed in section 2 above.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## BSD 2-Clause License

Copyright held by the authors listed in section 2 above.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## BSD 3-Clause License

Copyright held by the authors listed in section 2 above.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## Apache License 2.0

The full text is bundled with the app at `assets/licenses/Apache-2.0.txt` and is
shown by the in-app Legal notices screen. It applies to GALOSH, libultrahdr, and
the Gradle dependencies (AndroidX, Compose, Media3, Kotlin). A copy is also kept
in the APK's dependency license metadata.

## IJG License

libjpeg-turbo's IJG License text is bundled with the app at
`assets/licenses/libjpeg-turbo-README.ijg` (from upstream `README.ijg`), along
with `assets/licenses/libjpeg-turbo-LICENSE.md`. This software is based in part
on the work of the Independent JPEG Group.

## Selected AndroidX Material icons

`app/src/main/kotlin/com/rawr/camera/ui/icons` contains a subset of AndroidX
Compose Material icons 1.7.8, Copyright The Android Open Source Project, under
the Apache License 2.0. Sources retain their copyright/license headers; package
names were relocated. Source: https://android.googlesource.com/platform/frameworks/support/
