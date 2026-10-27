"""Scores distortion_validate dumps against a float64 reference of the WB pass.

usage: uv run --with numpy python -I distortion_compare.py <out-dir> [<out-dir> ...]

Reference: the rational profile model evaluated per pixel in float64, bilinear
resample of the dumped fp16 source (clamped at the border), then the pass's
white balance gains. Reports the error of each dump against it, and of every
dump against the first one.
"""
import sys
import numpy as np

GAINS = np.array([1.0, 1.0, 1.0], dtype=np.float64)
FX, FY, CX, CY = 1698.182744, 1696.500137, 2042.360233, 1548.313918
K1, K2, P1, P2, K3, K4, K5, K6 = -0.051307, 0.015171, 0.000123, 0.000357, 0.0, 0.0, -0.023536, 0.014256
CASES = {"photo": (4096, 3072, 0.9758, 0.0, 0.0), "video": (3840, 2160, 0.98474, 128.0, 456.0),
         "samsung_3x": (3648, 2736, 1.0, 0.0, 0.0)}


def load(path, w, h):
    return np.fromfile(path, dtype=np.float16).reshape(h, w, 4)


def source_positions(w, h, scale, ox, oy):
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    xn = (ox + x - CX) / (FX * scale)
    yn = (oy + y - CY) / (FY * scale)
    r2 = xn * xn + yn * yn
    rad = (1 + r2 * (K1 + r2 * (K2 + r2 * K3))) / (1 + r2 * (K4 + r2 * (K5 + r2 * K6)))
    xd = xn * rad + 2 * P1 * xn * yn + P2 * (r2 + 2 * xn * xn)
    yd = yn * rad + P1 * (r2 + 2 * yn * yn) + 2 * P2 * xn * yn
    return CX + xd * FX - ox, CY + yd * FY - oy  # frameScale 1


def reference(src, sx, sy):
    h, w, _ = src.shape
    sx = np.clip(sx, 0, w - 1)
    sy = np.clip(sy, 0, h - 1)
    x0 = np.floor(sx).astype(int)
    y0 = np.floor(sy).astype(int)
    x1 = np.minimum(x0 + 1, w - 1)
    y1 = np.minimum(y0 + 1, h - 1)
    fx = (sx - x0)[..., None]
    fy = (sy - y0)[..., None]
    s = src.astype(np.float64)
    top = s[y0, x0] * (1 - fx) + s[y0, x1] * fx
    bot = s[y1, x0] * (1 - fx) + s[y1, x1] * fx
    return top * (1 - fy) + bot * fy


def stats(err, ref):
    peak = np.abs(ref).max()
    mse = float(np.mean(err ** 2))
    psnr = 10 * np.log10(peak * peak / mse) if mse > 0 else float("inf")
    return f"max {np.abs(err).max():.5f}  mean {np.abs(err).mean():.6f}  p99.9 {np.quantile(np.abs(err), 0.999):.5f}  PSNR {psnr:.1f} dB"


def main(dirs):
    for name, (w, h, scale, ox, oy) in CASES.items():
        src = load(f"{dirs[0]}/{name}_src.f16", w, h)
        if name == "samsung_3x":
            y, x = np.mgrid[0:h, 0:w].astype(np.float64)
            xn, yn = (x - 1813.4) / 6961.56, (y - 1445.27) / 6975.01
            r2 = xn * xn + yn * yn
            radial = 1 - 0.416185 * r2 + 8.34895 * r2**2 - 47.3359 * r2**3
            sx, sy = 1813.4 + xn * radial * 6961.56, 1445.27 + yn * radial * 6975.01
        else:
            sx, sy = source_positions(w, h, scale, ox, oy)
        ref = reference(src, sx, sy)[..., :3] * GAINS
        print(f"[{name}] {w}x{h}")
        outs = [load(f"{d}/{name}_out.f16", w, h)[..., :3].astype(np.float64) for d in dirs]
        for d, o in zip(dirs, outs):
            print(f"  vs float64 reference  {d:32s} {stats(o - ref, ref)}")
            assert np.isfinite(o).all(), f"{name}: nonfinite output"
            assert np.mean(np.abs(o - ref)) < 0.001, f"{name}: incorrect distortion mapping"
        for d, o in zip(dirs[1:], outs[1:]):
            print(f"  vs {dirs[0]} {d:20s} {stats(o - outs[0], ref)}")


if __name__ == "__main__":
    main(sys.argv[1:])
