"""Prototype: direct 50MP Bayer from a Hexadeca 4x4 BGGR mosaic.

Own-colour output sites are the exact mean of the four 200MP samples. Foreign
sites are the mean over the 2x2 footprint of a 200MP estimate of the wanted
channel: pixel-level green (directional, optional Laplacian guidance), plus a
colour difference interpolated from same-colour 50MP pixels with
green-similarity weights. Numba reference only; not the production code.
"""
import numpy as np
from numba import njit, prange

# BGGR block parity: channel of block (bx, by).
CH = np.array([[2, 1], [1, 0]], np.int64)


@njit(cache=True, inline='always')
def clampi(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


@njit(cache=True, parallel=True)
def green200(raw, ch, lap, eps, cubic, N=4):
    h, w = raw.shape
    out = np.empty((h, w), np.float32)
    for y in prange(h):
        for x in range(w):
            if ch[(y // N) & 1, (x // N) & 1] == 1:
                out[y, x] = raw[y, x]
                continue
            x0 = x // N * N
            y0 = y // N * N
            xl = x0 - 1 if x0 - 1 >= 0 else x0 + N
            xr = x0 + N if x0 + N < w else xl
            yt = y0 - 1 if y0 - 1 >= 0 else y0 + N
            yb = y0 + N if y0 + N < h else yt
            gl = np.float32(raw[y, xl]); gr = np.float32(raw[y, xr])
            gt = np.float32(raw[yt, x]); gb = np.float32(raw[yb, x])
            th = np.float32(x - xl) / np.float32(xr - xl) if xr != xl else np.float32(0.5)
            tv = np.float32(y - yt) / np.float32(yb - yt) if yb != yt else np.float32(0.5)
            gh = gl + (gr - gl) * th
            gv = gt + (gb - gt) * tv
            if cubic > 0.0:
                # Hermite with end slopes from the neighbouring green pixels,
                # clamped to the end values so edges cannot overshoot.
                xl2 = clampi(xl - 1 if xl < x else xl + 1, 0, w - 1); xr2 = clampi(xr + 1 if xr > x else xr - 1, 0, w - 1)
                yt2 = clampi(yt - 1 if yt < y else yt + 1, 0, h - 1); yb3 = clampi(yb + 1 if yb > y else yb - 1, 0, h - 1)
                if xr != xl:
                    L = np.float32(xr - xl)
                    m0 = (gl - np.float32(raw[y, xl2])) * (1.0 if xl2 < xl else -1.0) * L
                    m1 = (np.float32(raw[y, xr2]) - gr) * (1.0 if xr2 > xr else -1.0) * L
                    t = th
                    hh = (2*t**3 - 3*t**2 + 1) * gl + (t**3 - 2*t**2 + t) * m0 + (-2*t**3 + 3*t**2) * gr + (t**3 - t**2) * m1
                    hh = min(max(hh, min(gl, gr)), max(gl, gr))
                    gh = gh + cubic * (hh - gh)
                if yb != yt:
                    L = np.float32(yb - yt)
                    m0 = (gt - np.float32(raw[yt2, x])) * (1.0 if yt2 < yt else -1.0) * L
                    m1 = (np.float32(raw[yb3, x]) - gb) * (1.0 if yb3 > yb else -1.0) * L
                    t = tv
                    hv = (2*t**3 - 3*t**2 + 1) * gt + (t**3 - 2*t**2 + t) * m0 + (-2*t**3 + 3*t**2) * gb + (t**3 - t**2) * m1
                    hv = min(max(hv, min(gt, gb)), max(gt, gb))
                    gv = gv + cubic * (hv - gv)
            xa = clampi(x0 - 2 * N + N - 1, 0, w - 1); xb = clampi(x0 + 2 * N, 0, w - 1)
            ya = clampi(y0 - 2 * N + N - 1, 0, h - 1); yb2 = clampi(y0 + 2 * N, 0, h - 1)
            ah = abs(gl - gr) + abs(np.float32(raw[y, x0]) - np.float32(raw[y, x0 + N - 1])) + \
                0.5 * abs(np.float32(raw[y, xa]) - np.float32(raw[y, xb]))
            av = abs(gt - gb) + abs(np.float32(raw[y0, x]) - np.float32(raw[y0 + N - 1, x])) + \
                0.5 * abs(np.float32(raw[ya, x]) - np.float32(raw[yb2, x]))
            wh = 1.0 / ((ah + eps) * (ah + eps))
            wv = 1.0 / ((av + eps) * (av + eps))
            v = (wh * gh + wv * gv) / (wh + wv)
            if lap != 0.0:
                # Laplacian guidance from this block's own colour: its second
                # difference across the block, along the chosen axis, tells how
                # much the linear green estimate misses (cf. Hamilton-Adams).
                # Local curvature of the own-colour samples at (x, y):
                xm = clampi(x - 1, x0, x0 + N - 1); xp = clampi(x + 1, x0, x0 + N - 1)
                ym = clampi(y - 1, y0, y0 + N - 1); yp = clampi(y + 1, y0, y0 + N - 1)
                c = np.float32(raw[y, x])
                lh = 2 * c - np.float32(raw[y, xm]) - np.float32(raw[y, xp])
                lv = 2 * c - np.float32(raw[ym, x]) - np.float32(raw[yp, x])
                v = v + lap * (wh * lh + wv * lv) / (wh + wv)
            out[y, x] = v
    return out


@njit(cache=True, parallel=True)
def convert(raw, ch, green, sub_px, radius, spatial, tmode, lap_unused):
    """sub_px: 1 -> per-50MP-pixel colour differences (200MP High style)."""
    h, w = raw.shape
    oh, ow = h // 2, w // 2
    out = np.empty((oh, ow), np.float32)
    # Per 50MP pixel: mean raw-green difference and mean green.
    dd = np.zeros((oh, ow), np.float32)
    gg = np.zeros((oh, ow), np.float32)
    for oy in prange(oh):
        for ox in range(ow):
            s = 0.0; g = 0.0
            for j in range(2):
                for i in range(2):
                    gv = green[2 * oy + j, 2 * ox + i]
                    s += raw[2 * oy + j, 2 * ox + i] - gv
                    g += gv
            dd[oy, ox] = s / 4; gg[oy, ox] = g / 4
    for oy in prange(oh):
        for ox in range(ow):
            want = ch[oy & 1, ox & 1] if False else CH[oy & 1, ox & 1]
            own = CH[(oy // 2) & 1, (ox // 2) & 1]
            if want == own:
                s = 0.0
                for j in range(2):
                    for i in range(2):
                        s += raw[2 * oy + j, 2 * ox + i]
                out[oy, ox] = s / 4
                continue
            g0 = gg[oy, ox]
            if want == 1:
                out[oy, ox] = g0
                continue
            sm = 0.0; wt = 0.0
            for dy in range(-radius, radius + 1):
                for dx in range(-radius, radius + 1):
                    qy = oy + dy; qx = ox + dx
                    if qy < 0 or qx < 0 or qy >= oh or qx >= ow:
                        continue
                    if CH[(qy // 2) & 1, (qx // 2) & 1] != want:
                        continue
                    sp = np.exp(-(dx * dx + dy * dy) * spatial)
                    sig = 0.1 * max(g0, gg[qy, qx]) + 4.0
                    r = (gg[qy, qx] - g0) / sig
                    wgt = sp / (1.0 + r * r * r * r)
                    sm += wgt * dd[qy, qx]; wt += wgt
            out[oy, ox] = g0 + (sm / wt if wt > 0 else 0.0)
    return out


def hybrid(raw, lap=0.0, radius=4, spatial=1 / 16, eps=1.0, cubic=0.0):
    ch = CH
    g = green200(raw, ch, np.float32(lap), np.float32(eps), np.float32(cubic))
    return convert(raw, ch, g, 1, radius, np.float32(spatial), 0, 0.0)


@njit(cache=True, parallel=True)
def convert_quad(q, green, radius, spatial, avg):
    """50MP-grid reconstruction from 2x2 sums q (Quad, block 2). Own sites are
    kept; foreign green from `green`; R/B = green + per-pixel colour difference
    interpolated from same-colour pixels with green-similarity weights.
    avg=1 uses the 2x2 block-mean difference (current High) instead of per pixel."""
    h, w = q.shape
    out = np.empty((h, w), np.float32)
    dd = np.zeros((h, w), np.float32)
    gg = np.zeros((h, w), np.float32)
    for y in prange(h):
        for x in range(w):
            if avg:
                y0 = y // 2 * 2; x0 = x // 2 * 2
                s = 0.0; gs = 0.0; c = 0
                for j in range(y0, min(h, y0 + 2)):
                    for i in range(x0, min(w, x0 + 2)):
                        s += q[j, i] - green[j, i]; gs += green[j, i]; c += 1
                dd[y, x] = s / c; gg[y, x] = gs / c
            else:
                dd[y, x] = q[y, x] - green[y, x]; gg[y, x] = green[y, x]
    for y in prange(h):
        for x in range(w):
            want = CH[y & 1, x & 1]
            own = CH[(y // 2) & 1, (x // 2) & 1]
            if want == own:
                out[y, x] = q[y, x]
                continue
            g0 = green[y, x]
            if want == 1:
                out[y, x] = g0
                continue
            sm = 0.0; wt = 0.0
            for dy in range(-radius, radius + 1):
                for dx in range(-radius, radius + 1):
                    qy = y + dy; qx = x + dx
                    if qy < 0 or qx < 0 or qy >= h or qx >= w:
                        continue
                    if CH[(qy // 2) & 1, (qx // 2) & 1] != want:
                        continue
                    if avg:
                        # block-centre distance for the averaged variant
                        cy = (qy // 2 * 2 + 0.5) - y; cx = (qx // 2 * 2 + 0.5) - x
                    else:
                        cy = float(dy); cx = float(dx)
                    sp = np.exp(-(cx * cx + cy * cy) * spatial)
                    sig = 0.1 * max(g0, gg[qy, qx]) + 4.0
                    r = (gg[qy, qx] - g0) / sig
                    wgt = sp / (1.0 + r * r * r * r)
                    sm += wgt * dd[qy, qx]; wt += wgt
            out[y, x] = g0 + (sm / wt if wt > 0 else 0.0)
    return out


def quad(q, lap=0.0, radius=3, spatial=1 / 4, eps=1.0, cubic=0.0, avg=0):
    """q: float32 array of 2x2 sums divided by 4 (DN units)."""
    q = np.ascontiguousarray(q, np.float32)
    g = green200(q, CH, np.float32(lap), np.float32(eps), np.float32(cubic), 2)
    return convert_quad(q, g, radius, np.float32(spatial), avg)
