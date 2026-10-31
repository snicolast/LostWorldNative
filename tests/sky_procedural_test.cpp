// The procedural sky is written twice: shaders/sky_procedural.glsl draws it on
// screen, sky_procedural.cpp makes the picture that stands in for the layer's
// own pixels (haze, bloom, horizon colour). This test compiles the GLSL file
// as C++ (with the few GLSL types and functions it uses) and checks that the
// two give the same colours over both sky layers, with and without drift, for
// a few band tables. It fails when one of them is changed and the other not.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "sky_procedural.h"

static int g_failed = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAILED line %d: %s\n", __LINE__, #cond); \
            g_failed++;                                      \
        }                                                    \
    } while (0)

// ---- Just enough GLSL for sky_procedural.glsl ----
namespace glsl {
using uint = uint32_t;

struct vec2 {
    float x, y;
    explicit vec2(float v) : x(v), y(v) {}
    vec2(float x_, float y_) : x(x_), y(y_) {}
    vec2& operator+=(vec2 o) { x += o.x, y += o.y; return *this; }
};
struct ivec2 {
    int x, y;
    ivec2(int x_, int y_) : x(x_), y(y_) {}
    explicit ivec2(vec2 v) : x(int(v.x)), y(int(v.y)) {}
};
struct vec3 {
    float x, y, z;
    explicit vec3(float v) : x(v), y(v), z(v) {}
    vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    vec3& operator*=(float s) { x *= s, y *= s, z *= s; return *this; }
};

inline vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline vec2 operator*(vec2 a, vec2 b) { return {a.x * b.x, a.y * b.y}; }
inline vec2 operator*(vec2 a, float s) { return {a.x * s, a.y * s}; }
inline vec2 operator*(float s, vec2 a) { return a * s; }
inline vec2 operator-(float s, vec2 a) { return {s - a.x, s - a.y}; }
inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator+(vec3 a, float s) { return {a.x + s, a.y + s, a.z + s}; }
inline vec3 operator*(vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

inline vec2 floor(vec2 v) { return {std::floor(v.x), std::floor(v.y)}; }
inline vec2 fract(vec2 v) { return v - floor(v); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline vec3 min(vec3 a, vec3 b) { return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)}; }
inline vec3 max(vec3 a, float b) { return {max(a.x, b), max(a.y, b), max(a.z, b)}; }
inline float clamp(float v, float lo, float hi) { return min(max(v, lo), hi); }
inline float mix(float a, float b, float t) { return a + (b - a) * t; }
inline vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
inline float smoothstep(float e0, float e1, float x) {
    float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4305)   // GLSL's literals are float, C++'s double
#endif
#include "shaders/sky_procedural.glsl"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}  // namespace glsl

// ---- Band tables (BGRA, sky and lit cloud per band of 16 rows) ----
static uint32_t rgb(float r, float g, float b) {
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
    return 0xFF000000u | c(r) << 16 | c(g) << 8 | c(b);
}

// kind 0: blue day sky, darker towards the top; 1: sunset, orange low down;
// 2: grey overcast; 3: night blues (as sky_night gives it, brighter)
static void make_bands(int kind, uint32_t bands[SKY_BANDS * 2]) {
    for (int band = 0; band < SKY_BANDS; band++) {
        float h = float(band) / (SKY_BANDS - 1);   // 0 at the top, 1 at the horizon
        float sky[3], lit[3];
        switch (kind) {
        case 0:
            sky[0] = 0.15f + 0.45f * h, sky[1] = 0.30f + 0.40f * h, sky[2] = 0.70f + 0.20f * h;
            lit[0] = 0.85f, lit[1] = 0.88f, lit[2] = 0.95f;
            break;
        case 1:
            sky[0] = 0.25f + 0.70f * h * h, sky[1] = 0.20f + 0.35f * h, sky[2] = 0.45f - 0.25f * h;
            lit[0] = 1.00f, lit[1] = 0.75f + 0.1f * h, lit[2] = 0.55f;
            break;
        case 2:
            sky[0] = sky[1] = 0.45f + 0.2f * h, sky[2] = 0.50f + 0.2f * h;
            lit[0] = lit[1] = lit[2] = 0.80f;
            break;
        default:
            sky[0] = 0.15f, sky[1] = 0.21f, sky[2] = 0.39f - 0.2f * (1 - h);
            lit[0] = 0.52f, lit[1] = 0.58f, lit[2] = 0.76f;
            break;
        }
        bands[band * 2] = rgb(sky[0], sky[1], sky[2]);
        bands[band * 2 + 1] = rgb(lit[0], lit[1], lit[2]);
    }
}

int main() {
    // Half a step of an 8-bit channel: the two may differ only by rounding
    // (the C++ build of the GLSL works partly in double)
    const float TOLERANCE = 0.5f / 255;
    float worst = 0;
    long points = 0, clouds = 0;
    for (int kind = 0; kind < 4; kind++) {
        uint32_t bands[SKY_BANDS * 2];
        make_bands(kind, bands);
        for (int upper = 0; upper < 2; upper++)
            for (float drift : {0.0f, 1.37f, 6.9f}) {
                float lo = 1, hi = 0;   // brightness range: the clouds are there
                for (int y = 0; y < SKY_SIZE; y += 3)
                    for (int x = 0; x < SKY_SIZE; x += 5) {
                        float px = x + 0.5f, py = y + 0.5f;
                        float cpu[3];
                        sky_colour(px, py, upper != 0, bands, drift, cpu);
                        float sky[3], lit[3];
                        sky_band_colour(bands, py / 16 - 0.5f, 0, sky);
                        sky_band_colour(bands, py / 16 - 0.5f, 1, lit);
                        glsl::vec3 gpu = glsl::sky_procedural(glsl::vec2(px, upper ? py - SKY_SIZE : py), drift,
                                                              glsl::vec3(sky[0], sky[1], sky[2]),
                                                              glsl::vec3(lit[0], lit[1], lit[2]));
                        float g[3] = {gpu.x, gpu.y, gpu.z};
                        for (int c = 0; c < 3; c++) {
                            float d = std::fabs(cpu[c] - g[c]);
                            if (!(d <= TOLERANCE) && worst <= TOLERANCE)   // first one only
                                printf("bands %d%s drift %.2f at (%d, %d) channel %d: cpu %.5f glsl %.5f\n", kind,
                                       upper ? " upper" : "", drift, x, y, c, cpu[c], g[c]);
                            if (!(d <= worst)) worst = d;   // NaN counts too
                        }
                        float l = 0.2126f * cpu[0] + 0.7152f * cpu[1] + 0.0722f * cpu[2];
                        float l_sky = 0.2126f * sky[0] + 0.7152f * sky[1] + 0.0722f * sky[2];
                        lo = std::min(lo, l), hi = std::max(hi, l);
                        clouds += l > l_sky + 0.1f;
                        points++;
                    }
                CHECK(hi - lo > 0.2f);
            }
    }
    printf("%ld points, %.1f%% with clouds, largest difference %.6f (%.3f of an 8-bit step)\n", points,
           100.0 * clouds / points, worst, worst * 255);
    CHECK(worst <= TOLERANCE);
    CHECK(clouds > points / 20);
    if (g_failed) {
        printf("%d check(s) failed: sky_procedural.cpp and shaders/sky_procedural.glsl no longer agree\n", g_failed);
        return 1;
    }
    printf("sky_procedural: CPU and GLSL agree\n");
    return 0;
}
