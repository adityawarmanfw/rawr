"""Full 50MP renders of real Hexadeca DNGs: WIP vs Detail vs Detail + phase fit.

Same RCD, white balance, forward matrix and exposure for every variant (exposure
is fixed from the WIP). Writes JPEGs to tmp/hexadeca-gt/50mp/<scene>_<variant>.jpg
and matching 100% crops of one detail window per scene.
"""
import sys
from pathlib import Path
import numpy as np, rawpy, tifffile
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gt_compare as g, phase_fit as pf, visual_compare as vc

OUT = Path(__file__).resolve().parents[2] / 'tmp/hexadeca-gt/50mp'
OUT.mkdir(parents=True, exist_ok=True)
D = Path.home() / 'Downloads'


def render(bayer, matrix, exposure):
    h, w = bayer.shape
    out = np.empty((h, w, 3), np.uint8)
    for y in range(0, h, 256):
        y0, y1 = max(0, y - 32), min(h, y + 288)
        rgb = g.rcd(np.clip(bayer[y0:y1], 0, 1).astype(np.float32))[y - y0:min(y + 256, h) - y0]
        out[y:y + len(rgb)] = vc.to_srgb(rgb, matrix, exposure)
    return out


def main():
    scenes = [Path(a).expanduser() for a in sys.argv[1:]] or [D / f'RAWR_20261112_{n}.dng' for n in ['165945_230', '165856_902', '170047_675']]
    for p in scenes:
        n = p.stem
        with tifffile.TiffFile(p) as f:
            orient = int(f.pages[0].tags[274].value)
        with rawpy.imread(str(p)) as f:
            raw = f.raw_image_visible.copy()
        matrix = vc.camera_matrix(p)
        curve = pf.fit(raw)[:2]
        r16 = np.clip(np.rint(raw), 0, 1023).astype(np.uint16)
        variants = {'1_wip': g.method_current(r16), '2_detail': g.method_detail_cpp(r16)}
        del r16
        fixed = np.clip(np.rint(pf.apply(raw, curve)), 0, 1023).astype(np.uint16)
        del raw
        variants['3_detail_fit'] = g.method_detail_cpp(fixed)
        del fixed
        # exposure from the WIP: 99.5th percentile of a subsampled RGB estimate
        sample = np.concatenate([g.rcd(np.clip(variants['1_wip'][y:y + 320], 0, 1).astype(np.float32))[32:-32].reshape(-1, 3)[::7]
                                 for y in range(0, 6144 - 320, 640)])
        exposure = 0.9 / max(float(np.quantile(sample @ matrix.T, .995)), .01)
        crops = {}
        for tag, b in variants.items():
            img = render(b, matrix, exposure)
            im = Image.fromarray(img)
            if orient == 6:
                im = im.transpose(Image.Transpose.ROTATE_270)
            im.save(OUT / f'{n}_{tag}.jpg', quality=96, subsampling=0)
            print(n, tag, im.size, flush=True)
            del img, im
        del variants


if __name__ == '__main__':
    main()
