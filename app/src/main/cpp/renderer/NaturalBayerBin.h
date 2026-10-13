#pragma once
// QB binning, adapted from a privately shared GPL-3.0 implementation (2026).
// See THIRD_PARTY_NOTICES.md. Samples are already linearized/black-subtracted.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace rawrcam::renderer {
class NaturalBayerTile {
    static constexpr int H = 20;
    int w, h;
    std::array<std::vector<float>, 10> p;
    static constexpr float chroma[9] = {.24695943f,   .21372294f,    .13419503f,    .05120057f, 0,
                                        -.013715486f, -.0076747425f, -.0012080271f, 0};
    static constexpr float luma[4] = {.125f, .375f, .375f, .125f};
    static constexpr float soft[6] = {3.f / 128, 20.f / 128, 41.f / 128, 41.f / 128, 20.f / 128, 3.f / 128};
    static constexpr float strong[10] = {1.f / 512,   9.f / 512,  36.f / 512, 84.f / 512, 126.f / 512,
                                         126.f / 512, 84.f / 512, 36.f / 512, 9.f / 512,  1.f / 512};
    template <class F>
    void each(int border, F f) {
        for (int y = border; y < h - border; ++y)
            for (int x = border; x < w - border; ++x) f(y * w + x, x, y);
    }
    void blur(std::vector<float>& a, std::vector<float>& tmp, int b, const std::array<float, 5>& k) {
        for (int y = b; y < h - b; ++y)
            for (int x = b + 2; x < w - b - 2; ++x) {
                int i = y * w + x;
                float v = 0;
                for (int j = 0; j < 5; ++j) v += a[i + j - 2] * k[j];
                tmp[i] = v;
            }
        each(b + 2, [&](int i, int, int) {
            float v = 0;
            for (int j = 0; j < 5; ++j) v += tmp[i + (j - 2) * w] * k[j];
            a[i] = v;
        });
    }

   public:
    static int reflect(int i, int n) {
        int period = 2 * (n - 1);
        i = (i % period + period) % period;
        return std::min(i, period - i);
    }
    template <class Sample, class Check>
    void run(int ox, int oy, int tw, int th, int sw, int sh, const std::array<float, 4>& gains, int red, float floor,
             Sample sample, Check check, float* out, int stride) {
        w = 2 * tw + 2 * H;
        h = 2 * th + 2 * H;
        for (auto& a : p) a.assign(w * h, 0);
        auto& src = p[0];
        auto& hx = p[1];
        auto& hy = p[2];
        auto& u = p[3];
        auto& lum = p[4];
        auto& v = p[5];
        auto& ex = p[6];
        auto& ey = p[7];
        auto& tx = p[8];
        auto& ty = p[9];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int sx = reflect(2 * ox + x - H, sw), sy = reflect(2 * oy + y - H, sh);
                src[y * w + x] = sample(sx, sy) * gains[2 * (sy & 1) + (sx & 1)];
            }
        for (int y = 0; y < h; ++y)
            for (int x = 8; x < w - 8; ++x) {
                int i = y * w + x;
                float even = src[i] * chroma[0], odd = 0;
                for (int j : {1, 2, 3, 5, 6, 7}) {
                    float a = (src[i - j] + src[i + j]) * chroma[j];
                    if (j & 1)
                        odd += a;
                    else
                        even += a;
                }
                hx[i] = even + odd;
                hy[i] = (even - odd) * ((x & 1) == (red & 1) ? 1.f : -1.f);
            }
        each(8, [&](int i, int, int y) {
            float low = hy[i] * chroma[0], hb = low, ha = hx[i] * chroma[0];
            for (int j : {1, 2, 3, 5, 6, 7}) {
                float b = (hy[i - j * w] + hy[i + j * w]) * chroma[j];
                low += b;
                float s = j & 1 ? -1.f : 1.f;
                hb += b * s;
                ha += (hx[i - j * w] + hx[i + j * w]) * chroma[j] * s;
            }
            float s = (y & 1) == red / 2 ? 1.f : -1.f;
            u[i] = low;
            lum[i] = ha * s;
            v[i] = hb * s;
        });
        each(10, [&](int i, int, int) {
            auto grad = [&](const auto& a) {
                float x = a[i + 2] - a[i - 2], y = a[i + 2 * w] - a[i - 2 * w];
                return x * x + y * y;
            };
            ex[i] = grad(u);
            ey[i] = grad(lum);
        });
        blur(ex, hx, 10, {.25f, 0, .5f, 0, .25f});
        blur(ey, hx, 10, {.25f, 0, .5f, 0, .25f});
        each(12, [&](int i, int x, int y) {
            float a = ex[i] + floor, b = ey[i] + floor, q = b / (a + b);
            float weight = q * q / (q * q + (1 - q) * (1 - q));
            u[i] = u[i] * weight + lum[i] * (1 - weight);
            float sx = (x & 1) == (red & 1) ? 1.f : -1.f, sy = (y & 1) == red / 2 ? 1.f : -1.f;
            lum[i] = src[i] - u[i] * (sx + sy) - v[i] * sx * sy;
        });
        check();
        each(13, [&](int i, int, int) {
            float c = 2 * lum[i], dx = lum[i + 1] - lum[i - 1], dy = lum[i + w] - lum[i - w];
            tx[i] = dx * dx - dy * dy;
            ty[i] = 2 * dx * dy;
            auto derivatives = [&](int a, int b, float& curve, float& grad) {
                curve = grad = 0;
                for (int s : {a, b}) {
                    float d = lum[i - s] + lum[i + s] - c, g = lum[i + s] - lum[i - s];
                    curve += d * d;
                    grad += g * g;
                }
            };
            derivatives(1, w, ex[i], ey[i]);
            derivatives(w - 1, w + 1, hx[i], hy[i]);
        });
        for (auto* a : {&ex, &ey, &hx, &hy, &tx, &ty})
            blur(*a, src, 13, {1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16});
        each(15, [&](int i, int, int) {
            float trace = ey[i] + floor, a = tx[i] / trace, b = ty[i] / trace;
            float coherence = std::clamp((a * a + b * b - .25f) / .75f, 0.f, 1.f);
            coherence *= coherence;
            ex[i] = coherence * std::clamp(4 * (ex[i] - ey[i]) / (ex[i] + ey[i] + floor), 0.f, 1.f);
            hx[i] = coherence * std::clamp(4 * (hx[i] - .5f * hy[i]) / (hx[i] + hy[i] + floor), 0.f, 1.f);
        });
        auto diagonal = [&](const auto& a, auto& dst, int border, int s) {
            each(border, [&](int i, int, int) {
                dst[i] = (a[i - 2 * s] + a[i + 2 * s] + 4 * (a[i - s] + a[i + s]) + 6 * a[i]) / 16;
            });
        };
        diagonal(lum, src, 14, w + 1);
        diagonal(src, hy, 16, w - 1);
        std::array<std::vector<float>, 7> rows;
        for (auto& row : rows) row.resize(w);
        std::array<const std::vector<float>*, 6> fields{&lum, &u, &v, &ex, &hy, &hx};
        for (int dy = 0; dy < th; ++dy) {
            check();
            int y = H + 2 * dy;
            for (int x = H - 4; x < H + 2 * tw + 4; ++x) {
                for (int k = 0; k < 6; ++k) {
                    const float* taps = k == 0 ? luma : soft;
                    int n = k == 0 ? 4 : 6, left = k == 0 ? 1 : 2;
                    float sum = 0;
                    for (int j = 0; j < n; ++j) sum += (*fields[k])[(y + j - left) * w + x] * taps[j];
                    rows[k][x] = sum;
                }
                float sum = 0;
                for (int j = 0; j < 10; ++j) sum += lum[(y + j - 4) * w + x] * strong[j];
                rows[6][x] = sum;
            }
            for (int dx = 0; dx < tw; ++dx) {
                auto filter = [&](int row, const float* taps, int n, int left) {
                    float sum = 0;
                    for (int j = 0; j < n; ++j) sum += rows[row][H + 2 * dx + j - left] * taps[j];
                    return sum;
                };
                float yl = filter(0, luma, 4, 1), uu = filter(1, soft, 6, 2), vv = filter(2, soft, 6, 2),
                      risk = filter(3, soft, 6, 2);
                float yy = yl + risk * (filter(6, strong, 10, 4) - yl);
                yy += filter(5, soft, 6, 2) * (filter(4, soft, 6, 2) - yy);
                int x = ox + dx, yout = oy + dy;
                float sx = (x & 1) == (red & 1) ? 1.f : -1.f, sy = (yout & 1) == red / 2 ? 1.f : -1.f;
                float value = (yy + uu * (sx + sy) + vv * sx * sy) / gains[2 * (yout & 1) + (x & 1)];
                float lo = INFINITY, hi = -INFINITY;
                for (int j = 0; j < 6; ++j)
                    for (int k = 0; k < 6; ++k) {
                        float a = sample(reflect(2 * x - (x & 1) + 2 * k - 4, sw),
                                         reflect(2 * yout - (yout & 1) + 2 * j - 4, sh));
                        lo = std::min(lo, a);
                        hi = std::max(hi, a);
                    }
                out[dy * stride + dx] = std::clamp(value, lo, hi);
            }
        }
    }
};
}  // namespace rawrcam::renderer
