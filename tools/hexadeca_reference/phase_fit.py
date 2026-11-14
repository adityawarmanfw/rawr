"""Per-phase response fit for a Hexadeca 4x4 BGGR block mosaic.

Model: for block type t (0..3 = (by&1)*2 + (bx&1)) and in-block position p
(0..15), x = a[t,p] * s + b[t,p] + noise, where s is the block mean. Fitted from
flat blocks over the whole frame, binned by block mean, then normalised so the
mean gain over the 16 positions is 1 and the mean offset 0 (the block mean is
preserved). Research prototype: estimates and applies the correction.
"""
import numpy as np

NBIN = 40


def fit(raw, black=64.0, band=1536, thr=0.03):
    """Returns (bin centres, ratio[4,16,NBIN], blocks used per type)."""
    H, W = raw.shape
    edges = np.geomspace(4.0, 900.0, NBIN + 1)
    n = np.zeros((4, NBIN)); sm = np.zeros((4, NBIN)); sx = np.zeros((4, NBIN, 16))
    for y0 in range(0, H - H % 8, band):
        r = raw[y0:min(H, y0 + band + 8)].astype(np.float32) - black
        h = (r.shape[0] // 8) * 8
        r = r[:h, :W // 8 * 8]
        hb, wb = h // 4, r.shape[1] // 4
        b = r.reshape(hb, 4, wb, 4).transpose(0, 2, 1, 3).reshape(hb, wb, 16)
        m = b.mean(2)
        dev = np.zeros_like(m)
        for dy, dx in [(0, 2), (0, -2), (2, 0), (-2, 0)]:
            sh = np.roll(np.roll(m, dy, 0), dx, 1)
            dev = np.maximum(dev, np.abs(sh - m))
        # noise allowance: ~ sqrt(2) * block-mean sigma for a*s + 9 variance
        ok = (m > 4) & (dev < thr * m + 2.5 * np.sqrt((0.5 * np.maximum(m, 0) + 9) / 16 * 2))
        ok[:3] = ok[-3:] = False; ok[:, :3] = ok[:, -3:] = False
        ty = (np.arange(hb)[:, None] & 1) * 2 + (np.arange(wb)[None, :] & 1)
        idx = np.clip(np.searchsorted(edges, m) - 1, 0, NBIN - 1)
        for t in range(4):
            sel = ok & (ty == t)
            if not sel.any():
                continue
            ii = idx[sel]; mm = m[sel]; bb = b[sel]
            np.add.at(n[t], ii, 1); np.add.at(sm[t], ii, mm)
            for p in range(16):
                np.add.at(sx[t, :, p], ii, bb[:, p])
    # Response ratio per (type, position, intensity bin): mean x_p / mean m.
    # The ratio is not constant with intensity (dual-conversion-gain style
    # steps appear around 30 DN), so it is a lookup curve, not a gain+offset.
    centres = np.sqrt(edges[:-1] * edges[1:])
    ratio = np.ones((4, 16, NBIN)); cnt = n.sum(1)
    for t in range(4):
        use = n[t] >= 300
        if use.sum() < 3 or cnt[t] < 2000:
            continue
        mean_m = sm[t, use] / n[t, use]
        r = (sx[t, use, :] / n[t, use, None]) / mean_m[:, None]       # [bins, 16]
        r /= r.mean(1, keepdims=True)                                 # block mean preserved
        for p in range(16):
            ratio[t, p] = np.interp(np.log(centres), np.log(mean_m), r[:, p])
        # light smoothing along intensity against bin noise
        k = np.array([.25, .5, .25])
        pad = np.pad(ratio[t], ((0, 0), (1, 1)), mode='edge')
        ratio[t] = k[0] * pad[:, :-2] + k[1] * pad[:, 1:-1] + k[2] * pad[:, 2:]
    return centres, ratio, cnt


def apply(raw, curve, black=64.0, band=1024):
    """Corrected float32 mosaic: each sample divided by its position's ratio at
    the block's mean intensity (log-linear interpolation of the fitted curve)."""
    centres, ratio = curve
    H, W = raw.shape
    out = np.empty((H, W), np.float32)
    lc = np.log(centres)
    for y0 in range(0, H, band):
        r = raw[y0:y0 + band].astype(np.float32) - black
        h = r.shape[0]
        hb, wb = h // 4, W // 4
        b = r.reshape(hb, 4, wb, 4).transpose(0, 2, 1, 3).reshape(hb, wb, 16)
        m = np.maximum(b.mean(2), 1.0)
        f = np.clip((np.log(m) - lc[0]) / (lc[1] - lc[0]), 0, len(lc) - 1.001)
        i0 = f.astype(np.int32); fr = (f - i0)[..., None]
        ty = (((y0 // 4) + np.arange(hb))[:, None] & 1) * 2 + (np.arange(wb)[None, :] & 1)
        rr = ratio[ty[..., None], np.arange(16)[None, None, :], i0[..., None]] * (1 - fr) + \
             ratio[ty[..., None], np.arange(16)[None, None, :], np.minimum(i0 + 1, len(lc) - 1)[..., None]] * fr
        c = b / rr
        out[y0:y0 + h] = c.reshape(hb, wb, 4, 4).transpose(0, 2, 1, 3).reshape(h, W) + black
    return out
