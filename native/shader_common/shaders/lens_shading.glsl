// Camera2 LensShadingMap: row-major grid, interleaved [R, G_even, G_odd, B].
// For green samples the map channel is selected from the sensor-row parity, not
// from Rawr's logical G1/G2 label; this keeps GBRG/BGGR correct as well.
//
// The host uploads the map as a flat float array with one vec4 per grid point
// (rawr/shading/LensShadingMapView.h), so each grid corner is fetched with a
// single 128-bit load and the channel is selected from registers. This is
// bit-identical to scalar loads: the same four floats feed the same bilinear
// mix in the same order, while the per-tap index math drops the *4/+channel
// operations and the loads coalesce.
//
// Includer contract:
//   LSC_BINDING      required; storage-buffer binding in set 0.
//   LSC_ENABLED      bool expression, default pc.lscEnabled != 0u.
//   LSC_GRID_SIZE    uvec2 expression, default uvec2(pc.lscWidth, pc.lscHeight).
//   LSC_SENSOR_SIZE  uvec2 expression, default uvec2(pc.width, pc.height).
#ifndef LSC_ENABLED
#define LSC_ENABLED (pc.lscEnabled != 0u)
#endif
#ifndef LSC_GRID_SIZE
#define LSC_GRID_SIZE uvec2(pc.lscWidth, pc.lscHeight)
#endif
#ifndef LSC_SENSOR_SIZE
#define LSC_SENSOR_SIZE uvec2(pc.width, pc.height)
#endif

layout(std430, set = 0, binding = LSC_BINDING) readonly buffer LensShadingGains { vec4 lscGainVec[]; };

uint camera2LensShadingChannel(ivec2 sensorPixel, int colorChannel) {
    if (colorChannel == 1) return 1u + uint(sensorPixel.y & 1);
    return uint(colorChannel);
}

float lensShadingGain(ivec2 sensorPixel, int colorChannel) {
    uvec2 grid = LSC_GRID_SIZE;
    uvec2 sensor = LSC_SENSOR_SIZE;
    if (!(LSC_ENABLED) || grid.x < 2u || grid.y < 2u) return 1.0;
    vec2 uv = vec2(sensorPixel) / vec2(max(float(sensor.x - 1u), 1.0), max(float(sensor.y - 1u), 1.0));
    vec2 gridPoint = clamp(uv, vec2(0.0), vec2(1.0)) * vec2(grid.x - 1u, grid.y - 1u);
    uvec2 p0 = uvec2(floor(gridPoint));
    uvec2 p1 = min(p0 + uvec2(1u), uvec2(grid.x - 1u, grid.y - 1u));
    vec2 f = fract(gridPoint);
    uint channel = camera2LensShadingChannel(sensorPixel, colorChannel);
    uint b00 = p0.y * grid.x + p0.x;
    uint b10 = p0.y * grid.x + p1.x;
    uint b01 = p1.y * grid.x + p0.x;
    uint b11 = p1.y * grid.x + p1.x;
    float g00 = lscGainVec[b00][channel];
    float g10 = lscGainVec[b10][channel];
    float g01 = lscGainVec[b01][channel];
    float g11 = lscGainVec[b11][channel];
    return mix(mix(g00, g10, f.x), mix(g01, g11, f.x), f.y);
}

// All four map channels [R, G_even, G_odd, B] at one (sub-pixel) sensor
// position: one bilinear lookup for a whole 2x2 Bayer cell instead of one per
// sample. The cell's samples sit within a pixel of its centre while a grid
// step spans hundreds, so the gain differs from per-sample lookups by far
// less than a code. Preview only; still/video keep per-sample gains.
vec4 lensShadingGainsAt(vec2 sensorPosition) {
    uvec2 grid = LSC_GRID_SIZE;
    uvec2 sensor = LSC_SENSOR_SIZE;
    if (!(LSC_ENABLED) || grid.x < 2u || grid.y < 2u) return vec4(1.0);
    vec2 uv = sensorPosition / vec2(max(float(sensor.x - 1u), 1.0), max(float(sensor.y - 1u), 1.0));
    vec2 gridPoint = clamp(uv, vec2(0.0), vec2(1.0)) * vec2(grid.x - 1u, grid.y - 1u);
    uvec2 p0 = uvec2(floor(gridPoint));
    uvec2 p1 = min(p0 + uvec2(1u), uvec2(grid.x - 1u, grid.y - 1u));
    vec2 f = fract(gridPoint);
    vec4 g00 = lscGainVec[p0.y * grid.x + p0.x];
    vec4 g10 = lscGainVec[p0.y * grid.x + p1.x];
    vec4 g01 = lscGainVec[p1.y * grid.x + p0.x];
    vec4 g11 = lscGainVec[p1.y * grid.x + p1.x];
    return mix(mix(g00, g10, f.x), mix(g01, g11, f.x), f.y);
}
