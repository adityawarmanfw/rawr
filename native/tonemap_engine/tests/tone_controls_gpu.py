#!/usr/bin/env python3
"""Host GPU property tests for the photographic tone controls.

Runs the production tonemap.comp through tonemap_vulkan_test (MoltenVK on
macOS) on a synthetic scene-linear AP1 chart and checks invariants of the
current renderer: finite output, monotonic tone ramps, slider continuity,
stacking strength, LUT robustness, the vibrance shadow guard and the Blacks+
near-black tail. Pass --before SHADER.comp to print the same metrics for an
older shader (for review; only the current shader is asserted).

    uv run --no-project --with numpy native/tonemap_engine/tests/tone_controls_gpu.py
"""
from pathlib import Path
import argparse
import subprocess
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
ENGINE = ROOT / 'native/tonemap_engine'
WORK = ROOT / 'tmp/tone-host'
SRGB_LUMA = np.array([0.2126, 0.7152, 0.0722])
W, GRAY_ROWS, COLOR_ROWS, RANDOM_ROWS = 512, 8, 24, 64
H = GRAY_ROWS + COLOR_ROWS + RANDOM_ROWS
# Blacks, Shadows, Contrast, Midtones, Highlights, Whites, Saturation, Vibrance.
NAMES = ('B', 'S', 'C', 'M', 'H', 'W', 'SAT', 'VIB')


def tone(**values):
    return [float(values.get(n, 0.0)) for n in NAMES]


def chart():
    """Gray ramp, hue ramps and random colors over -12..+4 EV around 18% gray."""
    x = np.linspace(-12.0, 4.0, W)
    luma = 0.18 * np.exp2(x)
    rgb = np.zeros((H, W, 3))
    rgb[:GRAY_ROWS] = luma[None, :, None]
    hues = np.array([[1, .45, .3], [.35, 1, .4], [.3, .5, 1], [1, .9, .25], [1, .3, .8], [.25, .95, 1]])
    for i, h in enumerate(hues):
        for j, strength in enumerate((.25, .5, .75, 1.0)):
            c = 1.0 + strength * (h / h.mean() - 1.0)
            c /= c @ [0.2722287168, 0.6740817658, 0.0536895174]
            rgb[GRAY_ROWS + 4 * i + j] = luma[:, None] * c[None, :]
    rng = np.random.default_rng(7)
    rand = rng.uniform(0.05, 1.0, (RANDOM_ROWS, W, 3))
    rand *= (0.18 * np.exp2(rng.uniform(-10, 4, (RANDOM_ROWS, W, 1)))) / rand.mean(axis=2, keepdims=True)
    rgb[GRAY_ROWS + COLOR_ROWS:] = rand
    rgba = np.concatenate([rgb, np.ones((H, W, 1))], axis=2).astype(np.float16)
    path = WORK / 'chart.rgba16f'
    rgba.tofile(path)
    return path


def build_runner():
    build = WORK / 'build'
    subprocess.run(['cmake', '-S', ENGINE, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release'],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', build, '--target', 'tonemap_vulkan_test'], check=True, stdout=subprocess.DEVNULL)
    return build / 'tonemap_vulkan_test'


def compile_shader(source, name):
    out = WORK / f'{name}.spv'
    subprocess.run(['glslc', '-fshader-stage=compute', '-I', ENGINE / 'shaders', '-DRAWR_TONEMAP_OUTPUT_RGBA32F=1',
                    source, '-o', out], check=True)
    return out


def write_cube(path):
    """17^3 LUT whose output overshoots [0,1] (some creative LUTs do)."""
    n = 17
    g = np.linspace(0, 1, n)
    lines = [f'LUT_3D_SIZE {n}']
    for b in g:
        for gg in g:
            for r in g:
                v = np.array([r, gg, b]) * 1.35 - 0.12
                lines.append(' '.join(f'{c:.6f}' for c in v))
    path.write_text('\n'.join(lines) + '\n')
    return path


class Renderer:
    def __init__(self, runner, spv, chart_path):
        self.runner, self.spv, self.chart = runner, spv, chart_path
        self.cache = {}

    def __call__(self, values, lut=None):
        key = (tuple(values), lut)
        if key in self.cache:
            return self.cache[key]
        out = WORK / 'out.f32'
        cmd = [self.runner, '--shader', self.spv, '--bench-only', '--iterations', '1', '--warmup', '0',
               '--output-bits', '32', '--local-x', '8', '--local-y', '16', '--width', str(W), '--height', str(H), '--input-rgba16f', self.chart,
               '--camera-matrix', '1,0,0,0,1,0,0,0,1', '--tone', ','.join(f'{v:g}' for v in values),
               '--dump-output', out]
        if lut:
            cmd += ['--lut', lut, '--lut-after-action', '0']
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        img = np.fromfile(out, dtype=np.float32).reshape(H, W, 4)[..., :3].astype(np.float64)
        self.cache[key] = img
        return img


def luma(img):
    return img @ SRGB_LUMA


def metrics(render, lut):
    m = {}
    stacks = [tone(S=100, W=100), tone(B=100, S=100, C=100, M=100, H=100, W=100),
              tone(B=-100, S=-100, C=-100, M=-100, W=-100), tone(B=100, S=100, H=-100, W=100, C=-50),
              tone(B=-60, S=80, C=60, M=-40, H=70, W=-30, SAT=50, VIB=80)]
    worst_gray, worst_color, bad = 0.0, 0.0, 0
    for t in stacks + [tone(**{n: s * 100}) for n in NAMES[:6] for s in (-1, 1)]:
        img = render(t)
        bad += int((~np.isfinite(img)).sum() + (img < -1e-6).sum() + (img > 1 + 1e-6).sum())
        rows = luma(img[:GRAY_ROWS + COLOR_ROWS])
        drop = rows[:, :-1] - rows[:, 1:]
        worst_gray = max(worst_gray, float(drop[:GRAY_ROWS].max()))
        if t[6] == 0 and t[7] == 0:
            worst_color = max(worst_color, float(drop[GRAY_ROWS:].max()))
    m['invalid_pixels'] = bad
    m['gray_ramp_inversion'] = worst_gray
    # Saturated highlights clip per channel; Whites+ chroma can dip one
    # clipped channel by a few codes (pre-existing, bounded here).
    m['color_ramp_inversion'] = worst_color
    # Whites leaving 0 must not change how other sliders render: a 0.01 step
    # with Shadows active has to be a tiny change, not a method switch.
    m['whites_tiny_step_with_shadows'] = float(np.abs(render(tone(S=50, W=.01)) - render(tone(S=50))).max())
    # Stacking: Shadows+100 keeps its lift where it acts most when Whites-100
    # is added (the old convex budget halved it).
    def lift(t_on, t_off):
        return luma(render(t_on))[0] - luma(render(t_off))[0]
    alone = lift(tone(S=100), tone())
    region = alone > 0.5 * alone.max()
    m['shadows_strength_when_stacked'] = float(np.median(lift(tone(S=100, W=-100), tone(W=-100))[region] / alone[region]))
    # NaN from pow() of a negative base is driver-defined: Apple clamps it to
    # black, Adreno may keep NaN. Count both.
    img = render(tone(S=40, W=60, C=30), lut=lut)
    lit = render(tone(), lut=lut).max(axis=2) > 0
    m['lut_bad_pixels'] = int((~np.isfinite(img)).any(axis=2).sum() + (lit & (img.max(axis=2) <= 0)).sum())
    # Vibrance+ in deep shadows (linear Y < 2%) vs midtones.
    def chroma(i):
        lin = np.where(i <= 0.04045, i / 12.92, ((i + 0.055) / 1.055) ** 2.4)
        return lin.max(axis=2) - lin.min(axis=2), lin @ SRGB_LUMA
    c0, y0 = chroma(render(tone())[GRAY_ROWS:GRAY_ROWS + COLOR_ROWS])
    c1, _ = chroma(render(tone(VIB=100))[GRAY_ROWS:GRAY_ROWS + COLOR_ROWS])
    deep, mid = (y0 > 1e-4) & (y0 < 0.015), (y0 > 0.15) & (y0 < 0.4)
    m['vibrance_gain_deep_shadow'] = float(np.median(c1[deep] / np.maximum(c0[deep], 1e-9)))
    m['vibrance_gain_midtone'] = float(np.median(c1[mid] / np.maximum(c0[mid], 1e-9)))
    # Blacks+100 slope (noise gain) below one output code; the fitted y^.51
    # lift is unbounded at black.
    g0, g1 = luma(render(tone()))[0], luma(render(tone(B=100)))[0]
    near = (g0[1:] > 1e-6) & (g0[1:] < 1 / 255)
    slope = np.diff(g1) / np.maximum(np.diff(g0), 1e-12)
    m['blacks_plus_subcode_slope'] = float(slope[near].max()) if near.any() else float('nan')
    # Single sliders are unchanged except where intended (gray + moderate color rows).
    rows = slice(0, GRAY_ROWS + COLOR_ROWS)
    for n in ('S', 'H', 'C', 'M'):
        for s in (-100, 50, 100):
            m[f'single_{n}{s:+d}'] = render(tone(**{n: s}))[rows]
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--before', type=Path, help='older tonemap.comp to compare against (not asserted)')
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    runner = build_runner()
    chart_path = chart()
    lut = str(write_cube(WORK / 'overshoot.cube'))
    after = metrics(Renderer(runner, compile_shader(ENGINE / 'shaders/tonemap.comp', 'after'), chart_path), lut)
    before = None
    if args.before:
        before = metrics(Renderer(runner, compile_shader(args.before, 'before'), chart_path), lut)

    scalar = [k for k in after if not k.startswith('single_')]
    print(f'{"metric":34s} {"before":>12s} {"after":>12s}')
    for k in scalar:
        b = f'{before[k]:12.5g}' if before else ' ' * 12
        print(f'{k:34s} {b} {after[k]:12.5g}')
    if before:
        diffs = {k: float(np.abs(after[k] - before[k]).max()) for k in after if k.startswith('single_')}
        print('single-slider max |after - before|:', ', '.join(f'{k[7:]}={v:.2g}' for k, v in diffs.items()))

    checks = [
        ('no invalid pixels', after['invalid_pixels'] == 0),
        ('gray tone ramps monotonic', after['gray_ramp_inversion'] < 1e-5),
        ('color ramps within clip tolerance', after['color_ramp_inversion'] < 0.02),
        ('Whites continuous at 0', after['whites_tiny_step_with_shadows'] < 0.002),
        ('Shadows keeps strength when stacked', 0.8 < after['shadows_strength_when_stacked'] < 1.25),
        ('LUT overshoot stays finite', after['lut_bad_pixels'] == 0),
        ('Vibrance+ spares deep shadows', after['vibrance_gain_deep_shadow'] < 1.05 < after['vibrance_gain_midtone']),
        ('Blacks+ does not amplify sub-code noise', after['blacks_plus_subcode_slope'] < 8.0),
    ]
    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(('PASS ' if ok else 'FAIL ') + name)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
