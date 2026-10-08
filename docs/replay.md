# Offline replay

`uv run --no-project tools/replay.py --help` is the single launcher for the existing host
Vulkan tools. On macOS these use MoltenVK. Install a Vulkan SDK/loader, CMake,
Ninja, and `uv`; the launcher also finds shader tools from the configured
Android SDK. Builds and Python environments stay under `tmp/`.

```sh
uv run --no-project tools/replay.py dng input.dng tmp/render
uv run --no-project tools/replay.py burst --output-dir tmp/merge --dump-linear input.rzsl
uv run --no-project tools/replay.py burst --output-dir tmp/merge --dump-linear path/to/dng-burst
uv run --no-project tools/replay.py packed --input input.rgba16f --config input.txt --output-dir tmp/render
```

**These remain diagnostic stages, not a complete app-equivalent pipeline.**
The DNG helper currently requires RAWR provenance, RGGB, DualRcdVng4, neutral
tone controls, and no film simulation. The burst tool accepts RZSL or a
name-sorted DNG directory and exports merge intermediates. Packed replay has
separate render controls and experimental film presets.

Packed replay also supports `--rcd` for the production RCD demosaicer, using
the packed CFA input. It cannot be combined with quadfix or RGB input modes.
The DNG wrapper still validates DualRcdVng4 recipes; `--rcd` is a packed-runner
override for controlled comparisons.

Packed replay accepts `--technical-lut-dwg path/to/look.cube` to replace the
RAWR NTRL render with a technical LUT at full intensity. Input is converted
to DaVinci Wide Gamut / DaVinci Intermediate. Output uses **Use directly**:
the LUT must produce display-ready values treated as encoded sRGB, with no
additional output color-space conversion. This option cannot be combined with
`--film`. It does not change highlight recovery or its compression; use
`--app-coloropp --app-compression 0` to retain Inpaint Opposed reconstruction
while bypassing its separate SDR compression during a comparison.

The intended replacement is one production-backed replay pipeline: single DNG,
RZSL burst, or explicit DNG burst/batch; a versioned recipe plus parameter
overrides; the app's processing stages and output encoding. It must reject
unsupported settings explicitly rather than silently substitute defaults.
