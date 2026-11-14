"""Side-by-side crops of real Hexadeca DNGs: WIP vs Detail vs Detail + phase fit.

Columns: previous WIP (2x2 sum + Quad High), Detail, Detail with the per-frame
phase-response correction applied before the sum. White balance and the DNG
forward matrix are applied; exposure is fixed per scene from the WIP image.
Needs tmp/hexadeca-gt/lib.dylib and lib2.dylib (see README.md).
"""
import sys
from pathlib import Path
import numpy as np, rawpy, tifffile
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter, uniform_filter
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gt_compare as g, phase_fit as pf

OUT = Path(__file__).resolve().parents[2] / 'tmp/hexadeca-gt/visual'
OUT.mkdir(parents=True, exist_ok=True)
D = Path.home() / 'Downloads'
BRAD = np.array([[.8951, .2664, -.1614], [-.7502, 1.7135, .0367], [.0389, -.0685, 1.0296]])
ADAPT = np.linalg.inv(BRAD) @ np.diag((BRAD @ [.95047, 1, 1.08883]) / (BRAD @ [.96422, 1, .82521])) @ BRAD
XYZRGB = np.array([[3.2404542, -1.5371385, -.4985314], [-.969266, 1.8760108, .041556], [.0556434, -.2040259, 1.0572252]])


def camera_matrix(path):
    with tifffile.TiffFile(path) as f:
        t = f.pages[0].tags
        rat = lambda c: (lambda v: v[::2] / v[1::2])(np.array(t[c].value, float))
        wb = 1 / rat(50728); wb /= wb[1]
        fm = rat(50964).reshape(3, 3)
    return (XYZRGB @ ADAPT @ fm @ np.diag(wb)).astype(np.float32)


def to_srgb(rgb, matrix, exposure, stretch=None):
    v = rgb @ matrix.T * exposure
    if stretch:  # local-contrast stretch around the patch mean, to expose grids
        m = v.mean((0, 1), keepdims=True); v = m + (v - m) * stretch
    return np.uint8(np.rint(g.lin_to_srgb(np.clip(v, 0, 1)) * 255))


def methods(raw, curve):
    r16 = np.clip(np.rint(raw), 0, 1023).astype(np.uint16)
    wip = g.method_current(r16)
    det = g.method_detail_cpp(r16)
    fit = g.method_detail_cpp(np.clip(np.rint(pf.apply(raw, curve)), 0, 1023).astype(np.uint16))
    return {'WIP: sum + Quad High': wip, 'Detail': det, 'Detail + phase fit': fit}


def sheet(tiles, labels, zoom, path):
    h, w = tiles[0].shape[:2]
    S = Image.new('RGB', (w * zoom * len(tiles), h * zoom))
    for i, (t, l) in enumerate(zip(tiles, labels)):
        im = Image.fromarray(t).resize((w * zoom, h * zoom), Image.NEAREST)
        ImageDraw.Draw(im).text((6, 4), l, fill=(255, 255, 0))
        S.paste(im, (i * w * zoom, 0))
    S.save(path)


def main():
    for n in ['165945_230', '165856_902', '170047_675']:
        p = D / f'RAWR_20261112_{n}.dng'
        with rawpy.imread(str(p)) as f:
            raw = f.raw_image_visible.copy()
        matrix = camera_matrix(p)
        curve = pf.fit(raw)[:2]
        # detail crop: highest-gradient 256x192 window (50MP px) on a coarse grid; sky/smooth crop: lowest
        lum = raw[::8, ::8].astype(np.float32) - 64
        gy, gx = np.gradient(gaussian_filter(lum, 1)); gm = np.hypot(gx, gy)
        win = (192 // 4, 256 // 4)  # in 1/8-subsampled 200MP px: 50MP px*2/8
        score = uniform_filter(gm, size=(24, 32))
        mean = uniform_filter(lum, size=(24, 32))
        valid = (mean > 25)
        picks = {}
        s2 = np.where(valid, score, -1); y, x = np.unravel_index(np.argmax(s2), s2.shape); picks['detail'] = (y, x)
        s3 = np.where(valid & (mean > 60), -score, -1e9); y, x = np.unravel_index(np.argmax(s3), s3.shape); picks['smooth'] = (y, x)
        for tag, (cy, cx) in picks.items():
            y0 = max(0, (cy * 8 - 192) // 8 * 8); x0 = max(0, (cx * 8 - 256) // 8 * 8)
            raw_crop = np.ascontiguousarray(raw[y0:y0 + 384, x0:x0 + 512])
            outs = methods(raw_crop, curve)
            rgbs = {k: g.rcd(np.clip(v, 0, 1).astype(np.float32)) for k, v in outs.items()}
            exposure = 0.8 / max(float(np.quantile(rgbs['WIP: sum + Quad High'] @ matrix.T, .995)), .01)
            stretch = 6.0 if tag == 'smooth' else None
            tiles = [to_srgb(r, matrix, exposure, stretch) for r in rgbs.values()]
            sheet(tiles, [k + (' (contrast x6)' if stretch else '') for k in rgbs], 2 if tag == 'detail' else 2,
                  OUT / f'{n}_{tag}.png')
            print(n, tag, (y0, x0), flush=True)


if __name__ == '__main__':
    main()
