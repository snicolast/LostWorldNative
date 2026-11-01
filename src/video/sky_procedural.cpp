#include "sky_procedural.h"

#include <algorithm>
#include <cmath>

// The same sky as shaders/sky_procedural.glsl (keep both in step):
// tests/sky_procedural_test.cpp checks that the two give the same colours.

namespace {
float sky_hash(int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFFFu) / 16777216.0f;
}

// Value noise that repeats every `period` cells along x (the layer wraps around)
float sky_noise(float x, float y, int period) {
    float fx = std::floor(x), fy = std::floor(y);
    int ix = int(fx), iy = int(fy);
    int x0 = ((ix % period) + period) % period, x1 = (x0 + 1) % period;
    float u = x - fx, v = y - fy;
    u = u * u * (3 - 2 * u);
    v = v * v * (3 - 2 * v);
    float a = sky_hash(x0, iy) + (sky_hash(x1, iy) - sky_hash(x0, iy)) * u;
    float b = sky_hash(x0, iy + 1) + (sky_hash(x1, iy + 1) - sky_hash(x0, iy + 1)) * u;
    return a + (b - a) * v;
}

float sky_fbm(float x, float y, int period, int octaves) {
    float s = 0, a = 0.5f;
    for (int i = 0; i < octaves; i++) {
        s += a * sky_noise(x, y, period);
        x = x * 2 + 5.3f;
        y = y * 2 + 9.7f;
        period *= 2;
        a *= 0.5f;
    }
    return s;
}

float sky_density(float u, float v) { return (std::max)(sky_fbm(u, v, 8, 6) - 0.45f, 0.0f) * 10.0f; }
}  // namespace

float sky_smooth(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Colour of a band of a procedural sky (fy: row / 16 - 0.5, interpolated)
void sky_band_colour(const uint32_t* bands, float fy, int which, float out[3]) {
    fy = std::clamp(fy, 0.0f, float(SKY_BANDS - 1));
    int i0 = int(fy), i1 = (std::min)(i0 + 1, SKY_BANDS - 1);
    float f = fy - float(i0);
    uint32_t a = bands[i0 * 2 + which], b = bands[i1 * 2 + which];
    for (int c = 0; c < 3; c++) {
        int shift = 16 - 8 * c;
        out[c] = (float((a >> shift) & 255) * (1 - f) + float((b >> shift) & 255) * f) / 255;
    }
}

// Sky colour at a point of a sky layer: x, y in its 512x512 picture; the
// upper layer continues above the lower one
void sky_colour(float x, float y, bool upper, const uint32_t* bands, float drift, float out[3]) {
    float sky[3], lit[3];
    sky_band_colour(bands, y / 16 - 0.5f, 0, sky);
    sky_band_colour(bands, y / 16 - 0.5f, 1, lit);
    if (upper) y -= SKY_SIZE;
    const float HORIZON = 500.0f, HEIGHT = 1012.0f;
    float t = std::clamp((HORIZON - y) / HEIGHT, 0.0f, 1.0f);   // 0 at the horizon, 1 at the top
    // More depth than the original: richer colour, darker towards the top,
    // a pale glow at the horizon, and whiter cloud tops
    float l = 0.2126f * sky[0] + 0.7152f * sky[1] + 0.0722f * sky[2];
    for (int c = 0; c < 3; c++) {
        sky[c] = (std::max)(l + (sky[c] - l) * 1.3f, 0.0f) * (1 - 0.3f * sky_smooth(0.1f, 0.8f, t)) +
                 0.06f * (1 - sky_smooth(0.0f, 0.15f, t));
        lit[c] += (1 - lit[c]) * 0.3f;
    }
    // The layer is a cylinder around the camera: u around it (8 cells), v up
    // it, squeezed towards the horizon by perspective
    float el = 0.03f + 1.5f * t;   // elevation (radians)
    float u = x * (8.0f / 512.0f) + drift;
    float v = 6.0f * std::tanh(2.5f / std::tan(el) / 6.0f) - 6.0f * t;
    float wu = sky_fbm(u * 0.5f + 3.1f, v * 0.5f, 4, 5), wv = sky_fbm(u * 0.5f, v * 0.5f + 7.7f, 4, 5);
    u += 0.5f * wu;
    v += 0.5f * wv;
    // Cloud density, then the light that reaches each point from above
    // through the cloud: lit tops, shaded bases, bright thin edges
    float density = sky_density(u, v);
    float above = 0;
    for (int i = 1; i <= 4; i++) above += sky_density(u, v - 0.06f * i);
    float light = std::exp(-above * 0.9f);
    float cover = (1 - std::exp(-density * 3.0f)) * sky_smooth(0.0f, 0.12f, t);
    float billows = 0.8f + 0.4f * sky_fbm(u * 4 + 1.7f, v * 4 + 2.9f, 32, 3);
    float distant = sky_smooth(0.17f, 0.0f, t) * 0.6f;
    for (int c = 0; c < 3; c++) {
        float shadow = (sky[c] * 0.6f + lit[c] * 0.3f) * 0.75f;
        float cloud = (shadow + ((std::min)(lit[c] * 1.05f, 1.0f) - shadow) * light + (1 - cover) * 0.15f) * billows;
        cloud += (sky[c] - cloud) * distant;
        out[c] = sky[c] + (cloud - sky[c]) * cover;
    }
}
