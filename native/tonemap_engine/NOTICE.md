# Notices

TonemapEngine's analytical sRGB/OKLab gamut-boundary implementation is adapted from
Björn Ottosson's MIT-licensed gamut clipping reference implementation:
https://bottosson.github.io/posts/gamutclipping/

The formulas are retained as color-math implementation details; TonemapEngine's public API
does not expose OKLab as a working space.

The AgX render transform (`include/tonemap/Agx.h`, `shaders/agx.glsl`) is ported from
darktable's `src/iop/agx.c` and `src/common/custom_primaries.c`, Copyright (C) 2025-2026
darktable developers, GPL-3.0-or-later: https://github.com/darktable-org/darktable
Its Blender-like primaries reproduce the AgX matrices of EaryChow's AgX_LUT_Gen
(https://github.com/EaryChow/AgX_LUT_Gen), after Troy Sobotka's AgX.
