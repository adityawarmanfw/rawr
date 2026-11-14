"""Ground-truth comparison of Hexadeca -> 50MP Bayer conversions.

A sharp RGB crop is treated as the ideal 200MP-scale scene. The mosaic is the
4x4 BGGR block CFA (10-bit, black 64, white 1023); the target is the 2x2 box
mean of the scene (the ideal 50MP image). Every method produces 50MP Bayer
that goes through the same RCD, so differences come from the conversion only.

Build the library first (see README):
  c++ -O3 -std=c++17 -shared -fPIC -Iapp/src/main/cpp \
      tools/hexadeca_reference/high_benchmark.cpp -o tmp/hexadeca-gt/lib.dylib
Run: uv run --with numpy --with numba --with scipy --with pillow python \
      tools/hexadeca_reference/gt_compare.py [--noise] SCENE.jpg [...]
"""
import argparse, ctypes, json, sys
from pathlib import Path
import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

Image.MAX_IMAGE_PIXELS = None
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'native/raw_demosaic/rcd/tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from rcd_numba_core import demosaic_rg  # noqa: E402

BLACK, RANGE = 64.0, 959.0
LIB = ctypes.CDLL(str(ROOT / 'tmp/hexadeca-gt/lib.dylib'))
U16 = np.ctypeslib.ndpointer(dtype=np.uint16, flags='C_CONTIGUOUS')
F32 = np.ctypeslib.ndpointer(dtype=np.float32, flags='C_CONTIGUOUS')
LIB.high_remosaic.argtypes = [U16] + [ctypes.c_uint] * 6
LIB.quad_remosaic_precise.argtypes = [U16] + [ctypes.c_uint] * 3
LIB.high_reduce.argtypes = [U16] + [ctypes.c_int] * 4 + [F32, F32]
LIB2 = ctypes.CDLL(str(ROOT / 'tmp/hexadeca-gt/lib2.dylib'))  # CfaRemosaic.h Detail
LIB2.detail_quad.argtypes = [U16] + [ctypes.c_uint] * 3


def srgb_to_lin(v):
    return np.where(v <= .04045, v / 12.92, ((v + .055) / 1.055) ** 2.4)


def lin_to_srgb(v):
    v = np.clip(v, 0, 1)
    return np.where(v <= .0031308, v * 12.92, 1.055 * v ** (1 / 2.4) - .055)


def pick_crops(path, count, side):
    """Highest-detail non-overlapping tiles (gradient energy of luma)."""
    im = np.asarray(Image.open(path).convert('RGB'), np.float32) / 255
    lum = im.mean(2)
    gy, gx = np.gradient(lum)
    energy = gx * gx + gy * gy
    h, w = lum.shape
    cands = []
    for y in range(side, h - 2 * side, side):
        for x in range(side, w - 2 * side, side):
            tile = energy[y:y + side, x:x + side]
            m = lum[y:y + side, x:x + side].mean()
            if .08 < m < .7:  # avoid blown/black tiles
                cands.append((float(tile.mean()), y, x))
    cands.sort(reverse=True)
    out = []
    for _, y, x in cands:
        if all(abs(y - yy) >= side or abs(x - xx) >= side for yy, xx in out):
            out.append((y, x))
        if len(out) == count:
            break
    return im, out


def cfa_bggr_block(h, w, n):
    """Channel index (0 R,1 G,2 B) of a BGGR image whose blocks are n x n."""
    ch = np.array([[2, 1], [1, 0]])
    by, bx = np.mgrid[0:h, 0:w]
    return ch[(by // n) & 1, (bx // n) & 1]


def mosaic(lin, rng, noise):
    h, w, _ = lin.shape
    ch = cfa_bggr_block(h, w, 4)
    sig = np.take_along_axis(lin, ch[..., None], 2)[..., 0] * RANGE
    if noise:  # Poisson-Gaussian, DN units (a*s + b)
        sig = sig + rng.standard_normal(sig.shape) * np.sqrt(noise[0] * np.maximum(sig, 0) + noise[1])
    return np.clip(np.rint(sig + BLACK), 0, 1023).astype(np.uint16)


def rcd(bggr):  # float Bayer 0..1 (BGGR) -> RGB
    return demosaic_rg(np.ascontiguousarray(bggr[::-1, ::-1]))[::-1, ::-1].copy()


def bayer_of(rgb):
    h, w, _ = rgb.shape
    return np.take_along_axis(rgb, cfa_bggr_block(h, w, 1)[..., None], 2)[..., 0]


def method_current(raw):
    q = raw[0::2, 0::2].astype(np.uint32)
    q += raw[0::2, 1::2]; q += raw[1::2, 0::2]; q += raw[1::2, 1::2]
    q = np.ascontiguousarray(q.astype(np.uint16))
    h, w = q.shape
    LIB.quad_remosaic_precise(q, w, h, 8)
    return (q.astype(np.float32) / 4 - BLACK) / RANGE


def method_detail_cpp(raw):
    q = raw[0::2, 0::2].astype(np.uint32)
    q += raw[0::2, 1::2]; q += raw[1::2, 0::2]; q += raw[1::2, 1::2]
    q = np.ascontiguousarray(q.astype(np.uint16))
    h, w = q.shape
    LIB2.detail_quad(q, w, h, 8)
    return (q.astype(np.float32) / 4 - BLACK) / RANGE


def method_quad_rounded(quality):
    def run(raw):
        q = raw[0::2, 0::2].astype(np.uint32)
        q += raw[0::2, 1::2]; q += raw[1::2, 0::2]; q += raw[1::2, 1::2]
        q = np.ascontiguousarray(((q + 2) // 4).astype(np.uint16))
        h, w = q.shape
        LIB.high_remosaic(q, w, h, 2, 3, 8, quality)
        return (q.astype(np.float32) - BLACK) / RANGE
    return run


def method_baseline(raw):
    h, w = raw.shape
    out = np.zeros((h // 2, w // 2), np.float32)
    LIB.high_reduce(np.ascontiguousarray(raw), w, h, 256, 8, np.ones(4, np.float32), out)
    return out


def method_bin_demosaic(raw):
    """Upper-bound style: 200MP High remosaic, RCD at 200MP, 2x2 box, CFA sample."""
    r = raw.copy()
    h, w = r.shape
    LIB.high_remosaic(r, w, h, 4, 3, 8, 2)
    rgb = rcd((r.astype(np.float32) - BLACK) / RANGE)
    rgb = rgb.reshape(h // 2, 2, w // 2, 2, 3).mean((1, 3))
    return bayer_of(rgb)


def method_hybrid(**kw):
    import hybrid_proto

    def run(raw):
        return (hybrid_proto.hybrid(raw, **kw) - BLACK) / RANGE
    return run


def method_quadproto(**kw):
    import hybrid_proto

    def run(raw):
        r = raw.astype(np.float32)
        q = (r[0::2, 0::2] + r[0::2, 1::2] + r[1::2, 0::2] + r[1::2, 1::2]) / 4
        return (hybrid_proto.quad(q, **kw) - BLACK) / RANGE
    return run


METHODS = {
    'baseline_high200+natural': method_baseline,
    'current_sum+quadHigh': method_current,
    'sum_rounded+quadHigh': method_quad_rounded(2),
    'sum_rounded+quadStd': method_quad_rounded(1),
    'remosaic200+rcd200+bin': method_bin_demosaic,
    'detail (CfaRemosaic.h Detail)': method_detail_cpp,
    'hybrid': method_hybrid(),
    'hybrid_lap0.25': method_hybrid(lap=0.25),
    'hybrid_lap0.5': method_hybrid(lap=0.5),
}


def psnr(a, b, border=24):
    a, b = a[border:-border, border:-border], b[border:-border, border:-border]
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse == 0 else -10 * np.log10(mse)


def hf_ratio(a, ref, border=24):
    """Detail kept: high-pass (sigma 1.5) energy of result vs reference, luma."""
    la, lr = a.mean(2), ref.mean(2)
    ha, hr = la - gaussian_filter(la, 1.5), lr - gaussian_filter(lr, 1.5)
    s = (slice(border, -border),) * 2
    return float(np.sqrt(np.mean(ha[s] ** 2) / np.mean(hr[s] ** 2)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scenes', nargs='+', type=Path)
    ap.add_argument('--noise', action='store_true', help='Poisson-Gaussian noise, a=0.5 b=9 DN')
    ap.add_argument('--blur', type=float, default=0.0, help='lens blur sigma at 200MP scale')
    ap.add_argument('--crops', type=int, default=2)
    ap.add_argument('--side', type=int, default=1536)
    ap.add_argument('--save', type=Path)
    args = ap.parse_args()
    rng = np.random.default_rng(1)
    noise = (0.5, 9.0) if args.noise else None
    rows = []
    for scene in args.scenes:
        im, crops = pick_crops(scene, args.crops, args.side)
        for (y, x) in crops:
            tile = srgb_to_lin(im[y:y + args.side, x:x + args.side])
            if args.blur:
                tile = gaussian_filter(tile, (args.blur, args.blur, 0))
            target = tile.reshape(args.side // 2, 2, args.side // 2, 2, 3).mean((1, 3))
            raw = mosaic(tile, rng, noise)
            ideal_bayer = bayer_of(target)
            ceiling = rcd(ideal_bayer.astype(np.float32))
            row = {'scene': scene.name, 'y': y, 'x': x}
            for name, fn in {'ideal_bayer+rcd (ceiling)': None, **METHODS}.items():
                if fn is None:
                    bayer, rgb = ideal_bayer, ceiling
                else:
                    bayer = fn(raw)
                    rgb = rcd(np.clip(bayer, 0, 1).astype(np.float32))
                row[name] = {
                    'bayer_psnr': psnr(bayer, ideal_bayer),
                    'rgb_psnr_lin': psnr(rgb, target),
                    'rgb_psnr_srgb': psnr(lin_to_srgb(rgb), lin_to_srgb(target)),
                    'detail_ratio': hf_ratio(lin_to_srgb(rgb), lin_to_srgb(target)),
                }
                if args.save:
                    args.save.mkdir(parents=True, exist_ok=True)
                    tag = f"{scene.stem}_{y}_{x}_{name.split(' ')[0].replace('+','_')}.png"
                    Image.fromarray(np.uint8(np.rint(lin_to_srgb(rgb) * 255))).save(args.save / tag)
            if args.save:
                Image.fromarray(np.uint8(np.rint(lin_to_srgb(target) * 255))).save(
                    args.save / f"{scene.stem}_{y}_{x}_target.png")
            rows.append(row)
            print(json.dumps({k: (v if not isinstance(v, dict) else {m: round(s, 2) for m, s in v.items()})
                              for k, v in row.items()}), flush=True)
    names = [k for k in rows[0] if isinstance(rows[0][k], dict)]
    print('\nMean over', len(rows), 'crops  (bayer dB / rgb-lin dB / rgb-sRGB dB / detail ratio)')
    for n in names:
        m = {k: np.mean([r[n][k] for r in rows]) for k in rows[0][n]}
        print(f"{n:32s} {m['bayer_psnr']:6.2f} {m['rgb_psnr_lin']:6.2f} {m['rgb_psnr_srgb']:6.2f} {m['detail_ratio']:6.3f}")


if __name__ == '__main__':
    main()
