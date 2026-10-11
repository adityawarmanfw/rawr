#!/usr/bin/env python3
"""Production shader checks for scene-key tone mapping (host Vulkan / MoltenVK).
Run: uv run --no-project --with numpy native/tonemap_engine/tests/adaptive_tone_gpu.py
Artifacts stay in tmp/adaptive-tone. Uses a grayscale ramp plus colored patches.
"""
from pathlib import Path
import subprocess
import numpy as np

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / 'tmp/adaptive-tone'
ENGINE = ROOT / 'native/tonemap_engine'
WORK.mkdir(parents=True, exist_ok=True)
build = WORK / 'build'
subprocess.run(['cmake', '-S', ENGINE, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release'], check=True)
subprocess.run(['cmake', '--build', build, '--target', 'tonemap_vulkan_test', '-j', '4'], check=True)
shader = WORK / 'adaptive-test.spv'
subprocess.run(['glslc', '-DRAWR_TONEMAP_OUTPUT_RGBA32F=1', '-DLOCAL_X=16', '-DLOCAL_Y=8',
                ENGINE / 'shaders/tonemap.comp', '-o', shader], check=True)
w, h = 1024, 8
x = np.exp2(np.linspace(-16, 3, w))
rgba = np.ones((h, w, 4), dtype=np.float16)
rgba[..., :3] = x[None, :, None]
for row, color in enumerate(([1.2, .8, .5], [.4, 1.2, .7], [.5, .8, 1.3]), start=1):
    rgba[row, :, :3] *= np.array(color)
luma = rgba[..., :3].astype(float) @ [0.2722287168, 0.6740817658, 0.0536895174]

def render(pixels, curve='0,-5,-1', profile=0, hal=1, exposure=0):
    source, output = WORK / 'input.rgba16f', WORK / 'output.f32'
    pixels.tofile(source)
    cmd = [build / 'tonemap_vulkan_test', '--shader', shader, '--bench-only', '--iterations', '1', '--warmup', '0',
           '--output-bits', '32', '--width', str(w), '--height', str(h), '--local-x', '16', '--local-y', '8',
           '--input-rgba16f', source, '--camera-matrix', '1,0,0,0,1,0,0,0,1', '--tone', '0,0,0,0,0,0,0,0',
           '--adaptive-tone', curve, '--render-transform', str(profile), '--ae-post-gain', str(hal),
           '--exposure-ev', str(exposure), '--dump-output', output]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return np.fromfile(output, dtype=np.float32).reshape(h, w, 4)[..., :3]

for profile in (0, 1, 2, 3):
    baseline = render(rgba, profile=profile)
    lifted = render(rgba, '2,-5,-1', profile)
    assert np.isfinite(lifted).all()
    assert np.array_equal(baseline[luma >= .5], lifted[luma >= .5]), 'highlight exposure changed'
    assert np.array_equal(baseline[luma <= 2**-11.6], lifted[luma <= 2**-11.6]), 'black toe changed'
    assert np.min(np.diff(lifted[0], axis=0)) >= -1e-6, 'tone reversal'
    assert np.array_equal(baseline, render(rgba, '0,-12,0', profile)), 'Off is not identity'
    if profile == 3:
        assert np.array_equal(baseline, lifted), 'LOG must bypass curve'
    else:
        at_key = np.argmin(abs(x - 2**-5))
        assert (lifted[0, at_key] > baseline[0, at_key] + .04).all(), 'subject not lifted'
    print(f'PASS profile={profile}: highlights, toe, monotonicity, Off, subject/LOG')

# Independent CPU warp, then production render: exercises curve values, RGB ratio
# preservation, and the ordering relative to HAL gain and manual exposure.
for hal in (1, 2):
    ev = np.log2(luma * hal)
    smooth = lambda t: np.clip(t, 0, 1)**2 * (3 - 2 * np.clip(t, 0, 1))
    # Key -5, highlight -1: peak at -6.5, toe 5 EV, lift limited to 0.25 * 5.5.
    gain = np.exp2(min(2, .25 * 5.5) * smooth((ev + 11.5) / 5) * (1 - smooth((ev + 6.5) / 5.5)))
    warped = rgba.copy()
    warped[..., :3] = rgba[..., :3].astype(float) * gain[..., None]
    for exposure in (0, 1):
        expected = render(warped, hal=hal, exposure=exposure)
        actual = render(rgba, '2,-5,-1', hal=hal, exposure=exposure)
        error = float(np.max(abs(actual - expected)))
        assert error < .001, error  # Half-float rounding of the reference input.
        print(f'PASS CPU/GPU curve parity HAL={hal} exposure={exposure}: max error={error:.7f}')
