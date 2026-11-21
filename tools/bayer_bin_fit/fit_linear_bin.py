# Learned linear 2x Bayer bin (native/raw_ingress BayerBinner). Four NxN kernels (one per output site) on the
# white-balanced mosaic, fitted by least squares: input = mosaic of a clean RGB scene + the measured 200 MP
# sensor grid (f85_grid_phase16.npy: 16x16 relative pattern; random offset/strength/sign) + noise; target =
# Lanczos 2x downscale of the clean scene, mosaicked. Exact colour reproduction of flat colours is enforced;
# the 4-pixel grid lines (the 50 MP Nyquist) are nulled exactly. Held-out images are scored against the target,
# and a thin saturated bar checks that narrow coloured detail keeps its colour.
#
# usage: fit_linear_bin.py N LAMBDA [notch8] [beta=B] [chroma=W,FC] [usm=A] [target=l2] [null8axis] [pen8=P]
#                          [null=fy,fx] [tag=NAME] images=DIR
#   images: a directory of high-resolution JPEGs (4000-9000 px wide); the first 9 train, the next 3 test.
#   chroma=W,FC: penalty (weight W) on each colour's response below FC cycles per source px differing from
#     the target's for its own colour and from zero for the others. Without it the fit borrows the
#     neighbourhood's luma for every colour (the training photos are mostly low-saturation), and colour
#     detail a few pixels wide turns grey.
#   Writes k_<tag>.npy (4 x N x N). The shipped kernel (see README.md):
#     fit_linear_bin.py 12 1e-4 notch8 beta=0.02 chroma=0.2,0.12 tag=plain images=DIR
import glob, os, sys, numpy as np
from PIL import Image
Image.MAX_IMAGE_PIXELS = None  # local high-resolution photos
from scipy.ndimage import convolve1d
rng = np.random.default_rng(1)
N = int(sys.argv[1]) if len(sys.argv) > 1 else 8          # kernel side (source px), centred on 2X+0.5
LAM = float(sys.argv[2]) if len(sys.argv) > 2 else 1e-4
NOTCH8 = 'notch8' in sys.argv[3:]   # also null the period-8 diagonal grid
BETA = float(next((a[5:] for a in sys.argv[3:] if a.startswith('beta=')), '0'))  # stopband penalty weight
TARGET_A = 2 if 'target=l2' in sys.argv[3:] else 3        # Lanczos lobes of the target downscale
PEN8 = float(next((a[5:] for a in sys.argv[3:] if a.startswith('pen8=')), '0'))  # soft penalty, axis period-8
NULL8AXIS = 'null8axis' in sys.argv[3:]
EXTRA = [tuple(map(float, a[5:].split(','))) for a in sys.argv[3:] if a.startswith('null=')]   # extra (fy,fx) nulls
USM = float(next((a[4:] for a in sys.argv[3:] if a.startswith('usm=')), '0'))  # sharpen the target (amount)
RATIO = int(next((a[6:] for a in sys.argv[3:] if a.startswith('ratio=')), '2'))  # 2 (200 -> 50 MP) or 3 (-> 22 MP)
assert RATIO in (2, 3) and (RATIO == 2) == (N % 2 == 0), 'ratio 2 needs an even N, ratio 3 an odd N'
CHROMA = next((tuple(map(float, a[7:].split(','))) for a in sys.argv[3:] if a.startswith('chroma=')), None)  # weight, cutoff
TAG = next((a[4:] for a in sys.argv[3:] if a.startswith('tag=')), f'N{N}')
IMAGES = next((a[7:] for a in sys.argv[3:] if a.startswith('images=')), None)
if not IMAGES: sys.exit('images=DIR is required')
files = sorted(glob.glob(os.path.join(IMAGES, '*.jpg')))
files = [f for f in files if 4000 <= Image.open(f).size[0] <= 9000][:12]
train, test = files[:9], files[9:]
grid = np.load(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'f85_grid_phase16.npy')) / 100.0  # raw positions mod 16
def scene(f):
    im = Image.open(f); w, h = im.size; im.draft('RGB', (w // 2, h // 2)); im = im.convert('RGB')
    im = im.resize((w // 4 // 4 * 4, h // 4 // 4 * 4), Image.LANCZOS)
    a = np.asarray(im, np.float32) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)   # linear, already neutral
# Output pixel X is centred on source 2X+0.5 (ratio 2) or 3X+1 (ratio 3).
CENTRE = 0.5 if RATIO == 2 else 1.0
def lanczos_down2(c):  # per channel, output centre at source RATIO*X + CENTRE
    if RATIO == 2: t = np.arange(-5.5, 6.0, 1.0)
    else: t = np.arange(-3 * TARGET_A + 1, 3 * TARGET_A, 1.0)
    x = t / RATIO
    k = np.sinc(x) * np.sinc(x / TARGET_A) * (np.abs(x) < TARGET_A); k /= k.sum()
    def ax(a, axis):
        if RATIO == 2:
            a = convolve1d(a, k, axis=axis, mode='reflect', origin=-1)   # even-length: centre between 2X and 2X+1
            return a.take(np.arange(0, a.shape[axis] - 1, 2), axis=axis)
        a = convolve1d(a, k, axis=axis, mode='reflect')
        return a.take(np.arange(1, a.shape[axis] - 1, 3), axis=axis)
    out = ax(ax(c, 0), 1)
    if USM > 0:   # unsharp mask on the ideal 50 MP image: sigma 1 output px
        from scipy.ndimage import gaussian_filter
        out = out + USM * (out - gaussian_filter(out, 1.0))
    return out
SITE = np.array([[0, 1], [1, 2]])                       # RGGB
def mosaic(rgb):
    h, w, _ = rgb.shape; yy, xx = np.mgrid[0:h, 0:w]; return rgb[yy, xx, SITE[yy & 1, xx & 1]]
def degrade(m, strength=None):
    h, w = m.shape; oy, ox = rng.integers(0, 16, 2)
    g = np.roll(np.roll(grid, oy, 0), ox, 1); g = np.tile(g, (h // 16 + 1, w // 16 + 1))[:h, :w]
    s = rng.uniform(0.5, 2.0) * rng.choice([-1, 1]) if strength is None else strength
    m = m * (1 + s * g)
    return m + rng.normal(size=m.shape) * np.sqrt(2e-5 * np.maximum(m, 0) + 1e-7)
# Source offsets of the window from R*X + C0: -3..4 for N=8 at ratio 2 (C0 0), -8..8 for N=17 at ratio 3 (C0 1).
off = np.arange(N) - (N // 2 - 1) if RATIO == 2 else np.arange(N) - N // 2
C0 = 0 if RATIO == 2 else 1
def windows(m, X, Y):
    m = m.astype(np.float64)  # rows of NxN windows around (R*X + C0, R*Y + C0)
    H, W = m.shape; ys = np.clip(RATIO * Y[:, None] + C0 + off[None, :], 0, H - 1); xs = np.clip(RATIO * X[:, None] + C0 + off[None, :], 0, W - 1)
    return m[ys[:, :, None], xs[:, None, :]].reshape(len(X), N * N)
rows = {p: [] for p in range(4)}; tgt = {p: [] for p in range(4)}
for f in train:
    rgb = scene(f); T = mosaic(lanczos_down2(rgb)); 
    for rep in range(2):
        m = degrade(mosaic(rgb))
        oh, ow = T.shape; X = rng.integers(4, ow - 4, 60000); Y = rng.integers(4, oh - 4, 60000)
        A = windows(m, X, Y); p = 2 * (Y & 1) + (X & 1)
        for q in range(4): rows[q].append(A[p == q]); tgt[q].append(T[Y[p == q], X[p == q]])
# Site colour of each window position for output phase q: the window starts at R*X + C0, whose parity is
# fixed at ratio 2 and follows the output's at ratio 3.
wy, wx = np.meshgrid(off, off, indexing='ij')
def window_colours(q): return SITE[(RATIO * (q >> 1) + C0 + wy) & 1, (RATIO * (q & 1) + C0 + wx) & 1].ravel()
K = {}
for q in range(4):
    A = np.concatenate(rows[q]); b = np.concatenate(tgt[q]); c = [0, 1, 1, 2][q]; wcol = window_colours(q)
    C = [(wcol == k).astype(np.float64) for k in range(3)]                # sum of weights per colour
    d = [1.0 if k == c else 0.0 for k in range(3)]
    # Zero response along the 4-pixel lines fx = 1/4 and fy = 1/4 (the output's Nyquist): per row and
    # per column of the window, cos and sin sums vanish. Kills the 4-pixel grid and aliases there.
    for i in range(N):
        for trig in (np.cos, np.sin):
            row = np.zeros((N, N)); row[i, :] = trig(2 * np.pi * 0.25 * off); C.append(row.ravel()); d.append(0.0)
            col = np.zeros((N, N)); col[:, i] = trig(2 * np.pi * 0.25 * off); C.append(col.ravel()); d.append(0.0)
    if NULL8AXIS:
        for fy, fx in ((0.125, 0.0), (0.0, 0.125)):
            ph = 2 * np.pi * (fy * wy + fx * wx)
            C += [np.cos(ph).ravel(), np.sin(ph).ravel()]; d += [0.0, 0.0]
    for fy, fx in EXTRA:
        for sy, sx in ((fy, fx), (fx, fy)):          # both orientations
            ph = 2 * np.pi * (sy * wy + sx * wx)
            C += [np.cos(ph).ravel(), np.sin(ph).ravel()]; d += [0.0, 0.0]
    if NOTCH8:
        for fy, fx in ((0.125, 0.125), (0.125, -0.125)):
            ph = 2 * np.pi * (fy * wy + fx * wx)
            C += [np.cos(ph).ravel(), np.sin(ph).ravel()]; d += [0.0, 0.0]
    C = np.stack(C); d = np.array(d)
    # drop linearly dependent constraint rows
    q_, r_ = np.linalg.qr(C.T); keep = np.abs(np.diag(r_)) > 1e-9; C, d = C[keep], d[keep]
    M = A.T @ A / len(b) + LAM * np.eye(N * N); v = A.T @ b / len(b)
    if PEN8 > 0:
        for fy, fx in ((0.125, 0.0), (0.0, 0.125)):
            e = np.exp(2j * np.pi * (fy * wy + fx * wx)).ravel()
            M = M + PEN8 * np.real(np.outer(e.conj(), e))
    if BETA > 0:
        fs = np.linspace(-0.5, 0.5, 41); FY, FX = np.meshgrid(fs, fs, indexing='ij'); R = np.hypot(FY, FX)
        # beyond the output's Nyquist (0.5 / RATIO on either axis, with a little margin) the output must be flat
        stop = (np.maximum(np.abs(FY), np.abs(FX)) > (0.27 if RATIO == 2 else 0.18))
        E = np.exp(2j * np.pi * (FY[stop][:, None] * wy.ravel()[None, :] + FX[stop][:, None] * wx.ravel()[None, :]))
        M = M + BETA * np.real(E.conj().T @ E) / stop.sum()
    if CHROMA:
        # Colour fidelity below the cutoff (cycles per source px): each colour's sub-kernel must reproduce the
        # target response for its own colour and pass nothing of the others, so narrow coloured detail keeps
        # its colour instead of taking the neighbourhood's luma.
        g_, fc = CHROMA
        def t1(f):
            t = (off + C0 - CENTRE) / RATIO; k = np.sinc(t) * np.sinc(t / TARGET_A) * (np.abs(t) < TARGET_A); k = k / k.sum()
            return np.sum(k[None, :] * np.exp(2j * np.pi * f[:, None] * off[None, :]), axis=1)
        fs = np.linspace(-fc, fc, 13); FY, FX = np.meshgrid(fs, fs, indexing='ij'); sel = np.hypot(FY, FX) <= fc
        fy, fx = FY[sel], FX[sel]
        T = t1(fy) * t1(fx)
        if USM > 0: T = T * (1 + USM * (1 - np.exp(-2 * np.pi ** 2 * ((RATIO * fy) ** 2 + (RATIO * fx) ** 2))))
        E = np.exp(2j * np.pi * (fy[:, None] * wy.ravel()[None, :] + fx[:, None] * wx.ravel()[None, :]))
        for k in range(3):
            Ek = E * (wcol == k)[None, :]; Dk = T if k == c else np.zeros_like(T)
            M = M + g_ * np.real(Ek.conj().T @ Ek) / len(fy)
            v = v + g_ * np.real(Ek.conj().T @ Dk) / len(fy)
    KKT = np.block([[M, C.T], [C, np.zeros((len(d), len(d)))]]); sol = np.linalg.solve(KKT, np.concatenate([v, d]))
    K[q] = sol[:N * N].reshape(N, N)
np.save(f'k_{TAG}.npy', np.stack([K[q] for q in range(4)]))
def apply(m, Ks):  # full-image linear bin
    H, W = m.shape; oh, ow = H // RATIO, W // RATIO; out = np.zeros((oh, ow)); S = 2 * RATIO
    pad = np.pad(m, ((N, N), (N, N)), mode='reflect')
    for q in range(4):
        qy, qx = q >> 1, q & 1; acc = 0; ny, nx = (oh - qy + 1) // 2, (ow - qx + 1) // 2
        for i, dy in enumerate(off):
            for j, dx in enumerate(off):
                y0, x0 = N + RATIO * qy + C0 + dy, N + RATIO * qx + C0 + dx
                acc = acc + Ks[q][i, j] * pad[y0: y0 + S * ny: S, x0: x0 + S * nx: S]
        out[qy::2, qx::2] = acc
    return out
def psnr(a, b): return 10 * np.log10(1 / np.mean((a - b) ** 2))
res = []
for f in test:
    rgb = scene(f); T = mosaic(lanczos_down2(rgb)); clean = mosaic(rgb)
    lin = apply(degrade(clean, 0.0), K); lin_g = apply(degrade(clean, 1.5), K)
    res.append((os.path.basename(f)[:28], psnr(lin, T), psnr(lin_g, T)))
for r in res: print(f'{r[0]:30s} PSNR vs target: {r[1]:.2f} dB (with grid x1.5: {r[2]:.2f})')
# frequency check: response at grid frequencies and aliasing, luma (sum over kernels for a neutral input)
Kn = sum(K[q] for q in range(4)) / 4
def resp(fy, fx): return abs(np.sum(Kn * np.exp(2j * np.pi * (fy * wy + fx * wx))))
print('luma response: f=0.0625 %.2f  0.125 %.2f  0.1875 %.2f | period-4 (0.25,0) %.3f (0,0.25) %.3f (0.25,0.25) %.3f | period-8 diag (0.125,0.125) %.2f'
      % (resp(0, .0625), resp(0, .125), resp(0, .1875), resp(.25, 0), resp(0, .25), resp(.25, .25), resp(.125, .125)))

def bar_test(Ks, width):
    # vertical orange bar on blue sky, `width` source px; colour at the bar centre (each channel interpolated
    # from its own sites) vs the target's: saturation kept, 1 = all of it
    n = 96; x = np.arange(n); prof = ((x >= 48 - width / 2) & (x < 48 + width / 2)).astype(float)
    from scipy.ndimage import gaussian_filter1d
    prof = gaussian_filter1d(prof, 1.0)
    sky, orange = np.array([.45, .55, .70]), np.array([.80, .30, .08])
    rgb = np.repeat((sky[None, :] + prof[:, None] * (orange - sky)[None, :])[None], n, 0)
    T = lanczos_down2(rgb); o = apply(mosaic(rgb), Ks); xc = (48 - CENTRE) / RATIO
    def at(row, par, img):  # samples of row at columns of parity par, interpolated at xc
        xs = np.arange(par, img.shape[1], 2); return np.interp(xc, xs, img[row, xs])
    got = np.array([at(20, 0, o), at(20, 1, o), at(21, 1, o)])
    want = np.array([np.interp(xc, np.arange(T.shape[1]), T[20, :, k]) for k in range(3)])
    sat = lambda c: (c[0] - c[2]) / max(c.mean(), 1e-6)
    return sat(got) / sat(want)
print('thin orange bar, saturation kept vs target:', ' '.join(f'w{w}: {bar_test(K, w):.2f}' for w in (3, 4, 6, 8, 12, 20)))
