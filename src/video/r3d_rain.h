#pragma once
// Rain (not in the original). Where the game draws its own rain (a few dozen
// white streaks in the T-Rex's forest), a dense fall of drops around the
// camera takes its place: thin streaks anchored to the world in three layers
// of distance, slanted by the wind (the direction of the game's own streaks)
// and by the camera's motion, hidden by the scenery like any other surface.
// The world gets wet with it (r3d.frag): darker surfaces, a sheen that
// reflects the fog and the lightning, puddles and ripples on the ground.
#include <cstdint>
#include <vector>

#include "r3d_scene.h"

class R3DRain {
public:
    // 0 = off, 1 = light, 2 = heavy
    void set_level(int level) { level_ = level; }
    // Rain of our own where the game has none (the weather, r3d_weather.h), 0..1
    void set_forced(float amount) { forced_ = amount; }
    // The camera is under a roof (r3d_shelter.h): our rain goes at once
    // instead of easing out over four seconds
    void set_sheltered(bool sheltered) { sheltered_ = sheltered; }
    // Once per new game frame: finds the game's rain in the world viewport
    // and eases ours in (over a second and a half) or out (over four)
    void observe(const R3DScene& scene);
    // How much it rains, 0..1 (0 with RAIN off): the wetness of the world
    float amount() const { return level_ > 0 ? amount_ : 0.0f; }
    // How much the game itself rains, 0..1 (with RAIN off too)
    float game_rain() const { return game_amount_; }
    // The game's own streaks (first vertex of each triangle), hidden while ours fall
    const std::vector<uint32_t>& game_streaks() const { return game_; }
    // Appends this image's drops, in the view space of the world viewport w:
    // time in seconds; pixel: size of a pixel at distance 1 (view units)
    void build(const R3DViewport& w, float time, float pixel, std::vector<R3DVertex>& out) const;
    // The wind at a game time: its direction across the ground (world x, z),
    // how hard it gusts (0..1) and how far it has carried things (world units)
    // since time 0. The drops, the fog banks and the curtains of rain in the
    // fog all move with it.
    static void wind(float time, float out[4]);

private:
    int level_ = 0;
    float amount_ = 0;
    float game_amount_ = 0;   // the game's own share of it
    float forced_ = 0;
    bool sheltered_ = false;
    float fall_[3] = {0, -1, 0};    // direction of the fall in the world frame, with the wind
    float camera_velocity_[3] = {}; // world units per second, eased
    float last_camera_[3] = {};
    bool have_camera_ = false;
    std::vector<uint32_t> game_;
};
