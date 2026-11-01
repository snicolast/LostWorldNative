#include "r3d_rain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr float FALL_SPEED = 55.0f;   // world units per second (the camera stands some 13 above the ground)
constexpr float EXPOSURE = 0.025f;    // seconds of motion each streak shows
constexpr float DROP_WIDTH = 0.025f;  // world units
constexpr float MAX_STREAK = 2.5f;    // world units

// The game's rain: untextured white see-through world polygons with nothing
// else set (shininess 2 is part of how they are stored)
bool game_streak(const R3DVertex& v) {
    return (v.flags & 0xFFFFFF) == (R3D_WORLD | (2u << R3D_SHININESS_SHIFT)) && (v.color & 0xFFFFFF) == 0xFFFFFF &&
           (v.color >> 24) < 0xF0;
}

float hash(uint32_t k) {
    k ^= k >> 16;
    k *= 0x7FEB352Du;
    k ^= k >> 15;
    k *= 0x846CA68Bu;
    k ^= k >> 16;
    return float(k & 0xFFFFFF) / float(0x1000000);
}

// x wrapped into [-period / 2, period / 2)
float wrap(float x, float period) { return x - period * std::floor(x / period + 0.5f); }

constexpr float WIND_DIR[2] = {0.8f, 0.6f};   // across the ground, world frame
constexpr float WIND_CALM = 4.0f;              // world units per second between gusts
constexpr float WIND_GUST = 14.0f;             // ... more at the height of a gust

// Gusts, 0..1: two slow waves out of step
float gust_at(float t) { return 0.5f + 0.3f * std::sin(t * 0.37f) + 0.2f * std::sin(t * 0.91f + 1.3f); }

// How far the wind has carried things by then: the integral of its speed
float wind_travel(float t) {
    float gusts = 0.5f * t + 0.3f / 0.37f * (1.0f - std::cos(t * 0.37f)) +
                  0.2f / 0.91f * (std::cos(1.3f) - std::cos(t * 0.91f + 1.3f));
    return WIND_CALM * t + WIND_GUST * gusts;
}

}  // namespace

void R3DRain::wind(float time, float out[4]) {
    out[0] = WIND_DIR[0];
    out[1] = WIND_DIR[1];
    out[2] = gust_at(time);
    out[3] = wind_travel(time);
}

void R3DRain::observe(const R3DScene& scene) {
    game_.clear();
    const R3DViewport* w = nullptr;
    for (const R3DViewport& v : scene.viewports)
        if (v.world) w = &v;
    float dir[3] = {0, 0, 0}, length = 0;
    if (w) {
        const float* axes = w->world_axes;
        for (uint32_t t = 0; t + 2 < w->trans_count; t += 3) {
            const R3DVertex* v = &scene.vertices[w->trans_first + t];
            if (!game_streak(v[0])) continue;
            game_.push_back(w->trans_first + t);
            // The longest edge runs along the streak
            float best[3] = {0, 0, 0}, best_len = 0;
            for (int e = 0; e < 3; e++) {
                const float* a = v[e].pos;
                const float* b = v[(e + 1) % 3].pos;
                float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len > best_len) best_len = len, best[0] = d[0], best[1] = d[1], best[2] = d[2];
            }
            if (best_len <= 0) continue;
            float wd[3];
            for (int k = 0; k < 3; k++)
                wd[k] = axes[k * 3] * best[0] + axes[k * 3 + 1] * best[1] + axes[k * 3 + 2] * best[2];
            float s = wd[1] > 0 ? -1.0f / best_len : 1.0f / best_len;   // pointing down
            for (int k = 0; k < 3; k++) dir[k] += wd[k] * s;
            length += best_len;
        }
        // The camera's motion through the world (cuts and jumps are ignored)
        if (have_camera_) {
            float v[3], speed = 0;
            for (int k = 0; k < 3; k++) {
                v[k] = (w->camera[k] - last_camera_[k]) * 57.5f;
                speed += v[k] * v[k];
            }
            if (speed > 0 && std::sqrt(speed) < 4.0f * FALL_SPEED)
                for (int k = 0; k < 3; k++) camera_velocity_[k] += 0.2f * (v[k] - camera_velocity_[k]);
        }
        for (int k = 0; k < 3; k++) last_camera_[k] = w->camera[k];
        have_camera_ = true;
    } else {
        have_camera_ = false;
    }
    // A dozen streak triangles or more: it rains (the game draws some 250)
    size_t count = game_.size();
    float target = count >= 12 ? 1.0f : 0.0f;
    game_amount_ += (target - game_amount_) * (target > game_amount_ ? 1.0f / 90.0f : 1.0f / 240.0f);
    target = std::max(target, forced_);
    // Under a roof, rain that is only ours stops within a few frames
    float fade_out = sheltered_ && game_amount_ < 0.05f ? 1.0f / 8.0f : 1.0f / 240.0f;
    amount_ += (target - amount_) * (target > amount_ ? 1.0f / 90.0f : fade_out);
    static const char* force = getenv("LW_RAIN");   // debugging aid: fixed amount (0 to 1)
    if (force) amount_ = game_amount_ = float(atof(force));
    float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (count >= 12 && dl > 0) {
        // The game's slant, kept within reason, eased so a turn of the camera
        // does not swing it
        float d[3] = {dir[0] / dl, std::min(dir[1] / dl, -0.8f), dir[2] / dl};
        float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; k++) fall_[k] += 0.05f * (d[k] / n - fall_[k]);
    }
    static const bool debug = getenv("LW_RAINDBG") != nullptr;   // debugging aid
    if (debug && w)
        printf("[rain] %zu triangles, mean length %.2f, fall %.2f %.2f %.2f, camera %.1f %.1f %.1f ground %.1f "
               "vel %.1f %.1f %.1f amount %.2f\n",
               count, count ? length / float(count) : 0.0f, fall_[0], fall_[1], fall_[2], w->camera[0], w->camera[1],
               w->camera[2], w->ground, camera_velocity_[0], camera_velocity_[1], camera_velocity_[2], amount_);
}

void R3DRain::build(const R3DViewport& w, float time, float pixel, std::vector<R3DVertex>& out) const {
    float rain = amount();
    if (rain < 0.01f) return;
    const float* X = w.world_axes;
    const float* Y = X + 3;
    const float* Z = X + 6;
    auto to_view = [&](const float* p, float* o) {
        for (int i = 0; i < 3; i++) o[i] = X[i] * p[0] + Y[i] * p[1] + Z[i] * p[2];
    };
    // The fall, plus the wind blowing it sideways in gusts
    float gust = gust_at(time), travel = wind_travel(time);
    float blow = WIND_CALM + WIND_GUST * gust;
    float velocity[3], streak[3];
    for (int k = 0; k < 3; k++) {
        velocity[k] = fall_[k] * FALL_SPEED + (k == 0 ? WIND_DIR[0] * blow : k == 2 ? WIND_DIR[1] * blow : 0.0f);
        // Seen from the moving camera the streaks slant against its motion
        streak[k] = (velocity[k] - 0.6f * camera_velocity_[k]) * EXPOSURE;
    }
    // A fast camera would smear them into long bars
    float streak_len = std::sqrt(streak[0] * streak[0] + streak[1] * streak[1] + streak[2] * streak[2]);
    if (streak_len > MAX_STREAK)
        for (float& c : streak) c *= MAX_STREAK / streak_len;
    // Near drops, then two wider rings of farther ones: half width of the
    // box around the camera, its height, the inner box left to the layer
    // before, drops, brightness
    struct Layer {
        float half, height, inner;
        int drops;
        float alpha;
    };
    static const Layer LAYERS[3] = {
        {15.0f, 40.0f, 0.0f, 2200, 0.55f},
        {50.0f, 60.0f, 15.0f, 7000, 0.42f},
        {140.0f, 100.0f, 50.0f, 6000, 0.28f},
    };
    float density = rain * (level_ >= 2 ? 1.0f : 0.45f);
    for (int l = 0; l < 3; l++) {
        const Layer& L = LAYERS[l];
        int drops = int(float(L.drops) * density);
        for (int i = 0; i < drops; i++) {
            uint32_t seed = uint32_t(l * 100003 + i) * 4u;
            // A point of the world box that falls and wraps around the camera
            float rel[3];
            rel[0] = wrap(hash(seed) * 2 * L.half + fall_[0] * FALL_SPEED * time + WIND_DIR[0] * travel - w.camera[0],
                          2 * L.half);
            rel[2] = wrap(hash(seed + 1) * 2 * L.half + fall_[2] * FALL_SPEED * time + WIND_DIR[1] * travel - w.camera[2],
                          2 * L.half);
            float phase = hash(seed + 2);
            rel[1] = wrap(phase * L.height + velocity[1] * time - w.camera[1], L.height);
            if (L.inner > 0 && std::fabs(rel[0]) < L.inner && std::fabs(rel[2]) < L.inner) continue;
            // Fade towards the sides of the box, where drops wrap around
            float edge = std::max(std::fabs(rel[0]), std::fabs(rel[2])) / L.half;
            float top = std::fabs(rel[1]) / (0.5f * L.height);
            float fade = std::clamp((1.0f - edge) / 0.2f, 0.0f, 1.0f) * std::clamp((1.0f - top) / 0.2f, 0.0f, 1.0f);
            if (fade <= 0) continue;
            float tail[3] = {rel[0] - streak[0], rel[1] - streak[1], rel[2] - streak[2]};
            float h[3], t[3];
            to_view(rel, h);
            to_view(tail, t);
            float z = -h[2];
            // In front of the camera and roughly in the picture
            if (z < 1.2f || -t[2] < 1.2f || std::fabs(h[0]) > 2.6f * z || std::fabs(h[1]) > 1.4f * z) continue;
            // About a pixel wide at least; thinner drops are fainter instead
            float width = std::max(DROP_WIDTH, pixel * z * 1.1f);
            fade *= std::clamp((z - 1.2f) / 3.0f, 0.0f, 1.0f);   // the nearest ones would cross the whole picture
            // Curtains of heavier rain sweeping by with the wind
            float px = rel[0] + w.camera[0], pz = rel[2] + w.camera[2];
            float along = px * WIND_DIR[0] + pz * WIND_DIR[1], across = pz * WIND_DIR[0] - px * WIND_DIR[1];
            float curtain = 0.5f + 0.5f * std::sin(0.045f * (along - travel) + 1.5f * std::sin(0.03f * across));
            fade *= 0.5f + curtain;
            float alpha = L.alpha * rain * fade * std::min(1.0f, 2.0f * DROP_WIDTH / width) * (0.6f + 0.8f * hash(seed + 3));
            if (alpha < 0.01f) continue;
            float s[3] = {t[0] - h[0], t[1] - h[1], t[2] - h[2]};
            // Sideways: across the streak and the line of sight
            float side[3] = {s[1] * h[2] - s[2] * h[1], s[2] * h[0] - s[0] * h[2], s[0] * h[1] - s[1] * h[0]};
            float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
            if (sl <= 1e-6f) continue;
            for (float& c : side) c *= 0.5f * width / sl;
            R3DVertex v{};
            v.color = 0xFFFFFFu | (uint32_t(std::min(alpha, 1.0f) * 255.0f) << 24);
            v.flags = R3D_WORLD | R3D_RAIN;
            v.fog = 1.0f;
            auto put = [&](const float* p, float across, float along) {
                for (int k = 0; k < 3; k++) {
                    v.pos[k] = p[k] + side[k] * across;
                    v.prev[k] = v.pos[k];
                }
                v.uv[0] = across;
                v.uv[1] = along;
                out.push_back(v);
            };
            put(h, 1, 0);
            put(h, -1, 0);
            put(t, 1, 1);
            put(h, -1, 0);
            put(t, -1, 1);
            put(t, 1, 1);
        }
    }
}
