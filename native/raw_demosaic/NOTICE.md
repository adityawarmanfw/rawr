# raw_demosaic — NOTICE

Part of Rawr and distributed with it under **GPL-3.0-only** (see the repository
root `LICENSE` and `NOTICE`).

This module is a GPU port / derivative of GPL-3.0-or-later upstream code.
Copyright in the original code remains with its authors:

- **RCD** demosaic: derived from **librtprocess**
  (<https://github.com/CarVac/librtprocess>), GPL-3.0-or-later, which
  incorporates code from **RawTherapee**
  (<https://github.com/RawTherapee/RawTherapee>). RCD algorithm by Luis Sanz
  Rodríguez; librtprocess and RawTherapee contributors.
- **VNG4** demosaic: derived from **librtprocess** (unclamped variant),
  GPL-3.0-or-later.
- **AMaZE** demosaic: derived from **librtprocess** / RawTherapee `amaze.cc`
  and **darktable** `src/iop/demosaicing/amaze.cc`, GPL-3.0-or-later. AMaZE
  algorithm Copyright (C) 2008-2010 Emil Martinec, optimizations by Ingo
  Weyrich; darktable Copyright (C) 2011-2024 darktable developers. The GPU pass
  structure was adapted from a privately shared GPL-3.0 WGSL implementation
  (2026), whose copyright remains with its author.
- **Dual** (RCD+VNG4 / AMaZE+VNG4) blend: derived from **RawTherapee**,
  GPL-3.0-or-later.
- **quadfix** pre-filter: original to Rawr.

Files here are modified ports; individual files carry provenance comments naming
the upstream function or pass they reproduce. Full third-party notices are in the
repository root `THIRD_PARTY_NOTICES.md`.
