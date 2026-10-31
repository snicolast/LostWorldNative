// Procedural sky (SKY: PROCEDURAL) on the CPU, for the picture that stands in
// for a sky layer's own pixels (SkyPack: haze, bloom, horizon colour). The
// same sky as shaders/sky_procedural.glsl, which draws it on screen with the
// clouds drifting (keep both in step; tests/sky_procedural_test.cpp checks it).
#pragma once
#include <cstdint>

constexpr int SKY_SIZE = 512;    // a sky layer is a 512x512 picture
constexpr int SKY_BANDS = 32;    // bands of 16 rows, each with a sky and a cloud colour

// smoothstep
float sky_smooth(float e0, float e1, float x);
// Colour of a band of a procedural sky (bands: BGRA, sky and lit cloud per
// band; fy: row / 16 - 0.5, interpolated; which: 0 sky, 1 lit cloud)
void sky_band_colour(const uint32_t* bands, float fy, int which, float out[3]);
// Sky colour at a point of a sky layer: x, y in its 512x512 picture; the
// upper layer continues above the lower one. drift: the clouds' drift along x
void sky_colour(float x, float y, bool upper, const uint32_t* bands, float drift, float out[3]);
