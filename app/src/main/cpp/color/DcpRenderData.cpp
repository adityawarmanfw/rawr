#include "color/DcpRenderData.h"

#include <algorithm>
#include <cmath>
namespace rawrcam::color {
std::vector<float> dcpRenderData(const DcpProfile& p) {
    // Three table descriptors (h,s,v,offset), encodings, then tone offset/count.
    std::vector<float> data(20, 0.0f);
    const DcpHueSatTable* tables[] = {&p.hueSatMap1, &p.hueSatMap2, &p.lookTable};
    for (size_t i = 0; i < 3; ++i) {
        const auto& t = *tables[i];
        if (t.empty()) continue;
        data[i * 4] = float(t.hues);
        data[i * 4 + 1] = float(t.sats);
        data[i * 4 + 2] = float(t.vals);
        data[i * 4 + 3] = float(data.size());
        data[12 + i] = float(t.encoding);
        data.insert(data.end(), t.data.begin(), t.data.end());
    }
    data[17] = float(std::exp2(p.baselineExposureOffset.value_or(0.0)));
    if (p.toneCurve.empty()) return data;
    // Natural cubic spline through the authored control points. Solve for
    // second derivatives with a tridiagonal elimination, then sample once.
    const size_t n = p.toneCurve.size() / 2;
    std::vector<double> x(n), y(n), second(n, 0), upper(n, 0), rhs(n, 0);
    for (size_t i = 0; i < n; ++i) {
        x[i] = p.toneCurve[2 * i];
        y[i] = p.toneCurve[2 * i + 1];
    }
    for (size_t i = 1; i + 1 < n; ++i) {
        const double left = x[i] - x[i - 1], right = x[i + 1] - x[i];
        const double diagonal = 2 * (left + right) - left * upper[i - 1];
        upper[i] = right / diagonal;
        rhs[i] = (6 * ((y[i + 1] - y[i]) / right - (y[i] - y[i - 1]) / left) - left * rhs[i - 1]) / diagonal;
    }
    for (size_t i = n - 1; i-- > 1;) second[i] = rhs[i] - upper[i] * second[i + 1];
    constexpr size_t samples = 4097;
    data[15] = float(data.size());
    data[16] = float(samples);
    size_t segment = 0;
    for (size_t i = 0; i < samples; ++i) {
        const double value = double(i) / (samples - 1);
        while (segment + 2 < n && value > x[segment + 1]) ++segment;
        const double h = x[segment + 1] - x[segment];
        const double b = std::clamp((value - x[segment]) / h, 0.0, 1.0), a = 1 - b;
        const double result = a * y[segment] + b * y[segment + 1] +
                              ((a * a * a - a) * second[segment] + (b * b * b - b) * second[segment + 1]) * h * h / 6;
        data.push_back(float(std::clamp(result, 0.0, 1.0)));
    }
    return data;
}
}  // namespace rawrcam::color
