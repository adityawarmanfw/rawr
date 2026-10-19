"""Analyse a SweepActivity run (tools/vivo_fullres_probe/pull_dcg.sh output).

usage: uv run --with numpy --with matplotlib python -I tools/vivo_fullres_probe/analyze_dcg.py tmp/dcg_sweep/dcg_<stamp>

Per sensor-mode row, from the green planes of the bracket t/8, t/2, t, t, 2t, 8t:
  clip      saturation code (max of the 8t frame) and its headroom above black
  read      temporal read noise (DN) from the t pair in the darkest bins
  DR        log2((clip - black) / read noise), engineering dynamic range in stops
  q_*       code-occupancy test: above the gain switch the low-gain samples are scaled by the ratio, so codes step by
            q_ratio; q_sw is that level, q_stops its distance below clip (ratio ~ 2**q_stops when HCG fills its range)
  n_*       noise test: level where the read-noise floor steps up and the noise factor of the step (works when the
            merge dithers and the codes stay dense)
  lin       worst deviation of the 2t/t and t/(t/2) exposure ratios from the requested ratio (kink/merge seam)
  quad      Quad-Bayer test on the full frame: ~1 for Bayer, >>1 for an unremosaiced 2x2 CFA
"""
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np


def load(path, w, h):
    return np.fromfile(path, dtype="<u2").reshape(h, w).astype(np.float64)


def black_of(s):
    vals = [float(v) for v in s.split(";") if v]
    return sum(vals) / len(vals) if vals else 0.0


def log_bins(lo, hi, n=40):
    return np.geomspace(max(lo, 1.0), hi, n)


def binned(x, y, edges, stat):
    idx = np.digitize(x, edges)
    out = []
    for i in range(1, len(edges)):
        sel = idx == i
        if sel.sum() >= 2000:
            out.append((math.sqrt(edges[i - 1] * edges[i]), stat(y[sel]), int(sel.sum())))
    return out


def analyse_row(run, frames):
    full = next((f for f in frames if f["kind"] == "full"), None)
    greens = [f for f in frames if f["kind"] == "green"]
    if not greens:
        return None
    w, h = int(greens[0]["w"]) // 2, int(greens[0]["h"]) // 2
    black = black_of(greens[0]["black"])
    white = int(greens[0]["white"]) if greens[0]["white"] else None
    by_mult = defaultdict(list)
    for f in greens:
        by_mult[float(f["mult"])].append(f)
    img = {m: [load(run / f["file"], w, h) for f in fs] for m, fs in by_mult.items()}
    exp = {m: float(fs[0]["exp"] or fs[0]["exp_req"]) for m, fs in by_mult.items()}
    r = {"row": greens[0]["row"] + ("" if greens[0].get("format", "raw16") == "raw16" else "/10"), "size": f'{greens[0]["w"]}x{greens[0]["h"]}', "fps": greens[0]["fps"],
         "black": black, "white": white, "iso": greens[0]["iso"], "mode_fb": greens[0]["sensor_mode"]}

    longest = img[max(img)][0]
    clip = float(longest.max())
    r["clip"] = clip
    r["sat_frac_8t"] = float((longest >= clip - 2).mean())
    span = clip - black

    # photon transfer from the t pair: flicker-normalised difference
    a, b = img[1.0][0], img[1.0][1]
    sa, sb = a - black, b - black
    good = (a < clip - 4) & (b < clip - 4)
    gain = np.median(sa[good & (sa > 0.05 * span)] / np.maximum(sb[good & (sa > 0.05 * span)], 1e-3))
    sb = sb * gain
    mean = (sa + sb) / 2
    diff = sa - sb
    edges = log_bins(0.5, span)
    ptc = binned(mean[good], diff[good], edges, lambda d: float(np.var(d) / 2))
    dark = [v for m, v, n in ptc if m < max(4.0, span / 2000)]
    read_var = min(dark) if dark else (ptc[0][1] if ptc else float("nan"))
    r["read"] = math.sqrt(read_var)
    r["DR"] = math.log2(span / r["read"]) if r["read"] > 0 else float("nan")
    r["ptc"] = ptc
    # A merged DCG frame keeps one DN-per-electron scale, so the shot-noise slope does not change at the switch.
    # Two signatures do: (1) the low-gain samples are digitally scaled by the ratio, so above the switch only every
    # ratio-th code occurs (unless the merge dithers); (2) the read-noise floor steps up there.
    pool = np.concatenate([x.ravel() for fs in img.values() for x in fs])
    pool = pool[(pool < clip - 2) & (pool > black)].astype(np.int64)
    occ = []
    for lo_e, hi_e in zip(edges[:-1], edges[1:]):
        lo_c, hi_c = int(black + lo_e), int(black + hi_e)
        if hi_c - lo_c < 64:
            continue
        sel = pool[(pool >= lo_c) & (pool < hi_c)]
        if sel.size < 20 * (hi_c - lo_c):
            continue
        codes = np.unique(sel)
        step = float(np.median(np.diff(codes))) if codes.size > 2 else float("nan")
        occ.append((math.sqrt(lo_e * hi_e), codes.size / (hi_c - lo_c), step))
    r["occ"] = occ
    r["q_switch"] = r["q_ratio"] = r["q_stops"] = float("nan")
    for i, (m, o, step) in enumerate(occ):
        if o < 0.6 and all(x[1] < 0.6 for x in occ[i:]):
            r["q_switch"] = m
            r["q_ratio"] = float(np.median([x[2] for x in occ[i:]]))
            r["q_stops"] = math.log2(span / m)
            break
    # noise floor step: residual variance after removing the shot term fitted below the step
    r["n_switch"] = r["n_stops"] = r["n_step"] = float("nan")
    k = [(m, (v - read_var) / m, n) for m, v, n in ptc if m > 20 * r["read"]]
    r["k"] = k
    if len(k) >= 6:
        ms = np.array([m for m, _, _ in k])
        vs = np.array([v for m, v, n in ptc if m > 20 * r["read"]])
        best, at = 1.0, None
        for i in range(3, len(ms) - 2):
            slope = np.median((vs[:i] - read_var) / ms[:i])
            pred = read_var + slope * ms
            jump = np.median(vs[i:i + 3] / pred[i:i + 3])
            if jump > best:
                best, at = jump, i
        if at is not None and best > 1.15:
            r["n_switch"], r["n_stops"], r["n_step"] = float(ms[at]), math.log2(span / ms[at]), float(math.sqrt(best))
    # linearity: measured exposure ratio against the requested one, binned by the shorter frame's level
    lin = {}
    worst = 0.0
    for lo_m, hi_m in ((0.5, 1.0), (1.0, 2.0), (0.125, 1.0)):
        if lo_m not in img or hi_m not in img:
            continue
        lo, hi = np.mean(img[lo_m], axis=0) - black, np.mean(img[hi_m], axis=0) - black
        want = exp[hi_m] / exp[lo_m]
        sel = (hi < clip - black - 8) & (lo > 8 * r["read"])
        curve = binned(lo[sel], hi[sel] / np.maximum(lo[sel], 1e-3) / want, log_bins(8 * r["read"], span), np.median)
        lin[f"{hi_m:g}/{lo_m:g}"] = curve
        if curve:
            vals = np.array([v for _, v, _ in curve])
            worst = max(worst, float(np.max(np.abs(vals / np.median(vals) - 1))))
    r["lin"] = lin
    r["lin_dev"] = worst

    r["quad"] = float("nan")
    if full:
        fw, fh = int(full["w"]), int(full["h"])
        p = load(run / full["file"], fw, fh)
        mid = (p > black + 0.05 * span) & (p < clip - 0.1 * span)
        d_even = np.abs(p[:, 0:-2:2] - p[:, 1:-1:2])[mid[:, 0:-2:2]]
        d_odd = np.abs(p[:, 1:-1:2] - p[:, 2::2])[mid[:, 1:-1:2]]
        if d_even.size and d_odd.size:
            r["quad"] = float(np.median(d_odd) / max(np.median(d_even), 1e-3))
    return r


def plot(run, rows):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    fig, ax = plt.subplots(1, 3, figsize=(18, 5.5))
    for r in rows:
        lab = f'r{r["row"]}'
        if r["ptc"]:
            m, v, _ = zip(*r["ptc"])
            ax[0].loglog(m, np.sqrt(v), label=lab)
        if r["occ"]:
            m, o, _ = zip(*r["occ"])
            ax[1].semilogx(m, o, label=lab)
        c = r["lin"].get("2/1") or next(iter(r["lin"].values()), None)
        if c:
            m, v, _ = zip(*c)
            ax[2].semilogx(m, v, label=lab)
    ax[0].set(title="temporal noise vs signal (t pair)", xlabel="signal DN above black", ylabel="noise DN")
    ax[1].set(title="code occupancy (drops to 1/ratio above the gain switch)", xlabel="signal DN", ylabel="used codes / range")
    ax[2].set(title="2t/t response, normalised (kinks = merge seam)", xlabel="t-frame signal DN", ylabel="ratio / requested")
    for a in ax:
        a.grid(True, which="both", alpha=0.3)
    ax[2].legend(fontsize=7, ncol=2)
    fig.tight_layout()
    fig.savefig(run / "dcg_summary.png", dpi=110)


def main():
    run = Path(sys.argv[1])
    frames = defaultdict(list)
    with open(run / "frames.csv") as f:
        for row in csv.DictReader(f):
            frames[(int(row["row"]), row.get("format", "raw16"))].append(row)
    rows = []
    for idx in sorted(frames):
        try:
            r = analyse_row(run, frames[idx])
        except Exception as e:  # keep the other rows
            print(f"row {idx}: analysis failed: {e}")
            continue
        if r:
            rows.append(r)
    head = (f'{"row":>3} {"size":>10} {"fps":>3} {"iso":>5} {"black":>6} {"white":>6} {"clip":>6} {"read":>6} '
            f'{"DR":>5} {"q_sw":>7} {"q_stops":>7} {"q_ratio":>7} {"n_sw":>7} {"n_stops":>7} {"n_step":>6} {"lin%":>5} {"quad":>5}')
    lines = [head]
    for r in rows:
        lines.append(f'{r["row"]:>3} {r["size"]:>10} {r["fps"]:>3} {r["iso"]:>5} {r["black"]:>6.0f} {str(r["white"]):>6} '
                     f'{r["clip"]:>6.0f} {r["read"]:>6.2f} {r["DR"]:>5.2f} {r["q_switch"]:>7.0f} {r["q_stops"]:>7.2f} {r["q_ratio"]:>7.1f} '
                     f'{r["n_switch"]:>7.0f} {r["n_stops"]:>7.2f} {r["n_step"]:>6.2f} {100 * r["lin_dev"]:>5.1f} {r["quad"]:>5.2f}')
    text = "\n".join(lines)
    print(text)
    (run / "dcg_summary.txt").write_text(text + "\n")
    plot(run, rows)
    print(f"\nwrote {run / 'dcg_summary.txt'} and dcg_summary.png")


if __name__ == "__main__":
    main()
