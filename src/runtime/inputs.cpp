#include "inputs.h"

#include "../common/clock.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

Inputs g_inputs;
std::atomic<bool> g_select_aim{false}, g_fixed_aim{false}, g_free_play{false};

std::atomic<float> g_aim_reach{0.0f};

static std::atomic<float> g_aim_x[2] = {-1.0f, -1.0f}, g_aim_y[2] = {-1.0f, -1.0f};
static std::atomic<int> g_aim_adc[2] = {-1, -1};   // gun_x | gun_y << 8 as set_player_aim left them

void set_player_aim(int player, float x, float y) {
    g_aim_x[player] = x;
    g_aim_y[player] = y;
    if (g_fixed_aim) return;
    // Measured with --aim against the game's own marker, with the factory gun
    // calibration (screen px = step * (adc - zero), all points within 1 px).
    // The game maps the gun differently on the stage select screen (its dot,
    // tools/find_crosshair.py, 13 positions) than in a stage (the X, read from
    // --shot's out/scene.txt, 15 positions): 7% wider there.
    struct Mapping { float step_x, zero_x, step_y, zero_y; };
    static const Mapping STAGE{2.404f, 15.14f, 1.806f, 3.68f}, SELECT{2.569f, 21.84f, 1.954f, 11.52f};
    const Mapping& m = g_select_aim ? SELECT : STAGE;
    const float W = 496, H = 384;   // the game screen
    float ax = m.zero_x + x * W / m.step_x, ay = m.zero_y + y * H / m.step_y;   // player 1's gun
    if (player == 1) {
        // Player 2's gun has its own factory calibration: in a stage its X
        // sits at screen px = step * (adc - zero) with these values (fitted to
        // the game's marker at ADC 60, 128 and 200 / 60, 128 and 180, within
        // 2 px); the same correction applies to the stage select mapping
        static const Mapping P2_STAGE{2.349f, 18.84f, 1.818f, 0.03f};
        ax = P2_STAGE.zero_x + (ax - STAGE.zero_x) * STAGE.step_x / P2_STAGE.step_x;
        ay = P2_STAGE.zero_y + (ay - STAGE.zero_y) * STAGE.step_y / P2_STAGE.step_y;
    }
    int gx = std::clamp(int(ax + 0.5f), 0, 255), gy = std::clamp(int(ay + 0.5f), 0, 255);
    g_inputs.p[player].gun_x = gx;
    g_inputs.p[player].gun_y = gy;
    g_aim_adc[player] = gx | gy << 8;
}

bool gun_aim_exact(int player, float& out_x, float& out_y) {
    if (gun_reach() <= 0 || g_fixed_aim || player < 0 || player > 1) return false;
    // Only while the gun is where set_player_aim put it (not --auto's raw
    // values, nor survival's scripted shots on its way in)
    const PlayerInputs& pl = g_inputs.p[player];
    if (g_aim_adc[player] != (pl.gun_x | pl.gun_y << 8)) return false;
    // The game draws its X (and aims) at screen px = 248 + 0.9797 * x, 192 -
    // 0.9957 * y, for both guns: measured with --auto point:X/Y over the whole
    // 21:9 view (13 points, within 0.05 px of the X in out/scene.txt)
    out_x = (g_aim_x[player] * 496 - 248) / 0.9797f;
    out_y = (192 - g_aim_y[player] * 384) / 0.9957f;
    return true;
}

float gun_reach() { return g_select_aim ? 0.0f : g_aim_reach.load(); }

PlayerAim player_aim(int player) {
    float x = g_aim_x[player], y = g_aim_y[player];
    float reach = gun_reach();
    return {x, y, x >= -reach && y >= 0 && x <= 1 + reach && y <= 1};
}

int pad_player(int pad, uint32_t connected) {
    if (pad < 0 || pad >= 32) return 1;
    uint32_t below = connected & ((1u << pad) - 1);   // a pad before this one is player 1's
    return below ? 1 : 0;
}

// player_join: coin held for 100 ms, then start for 100 ms after a pause, so
// the game sees each press on its own (it reads the buttons at 57.5 Hz)
static double g_join_at[2] = {-1, -1};

void player_join(int player) {
    if (g_join_at[player] < 0) g_join_at[player] = now_seconds();
}

void join_update() {
    double now = now_seconds();
    for (int p = 0; p < 2; p++) {
        if (g_join_at[p] < 0) continue;
        double t = now - g_join_at[p];
        // Credits are shared: the coin goes in slot 1, as the "5" key does
        // (none with FREE PLAY: the game needs no credit then). Start is
        // released here too: a start held since the last join is no press
        // for the game
        if (t < 0.1 && !g_free_play) g_inputs.p[0].coin = true;
        g_inputs.p[p].start = t >= 0.25 && t < 0.35;
        if (t >= 0.45) g_join_at[p] = -1;
    }
}

void AutoInput::parse(const std::string& s) {
    size_t p = 0;
    while (p < s.size()) {
        size_t comma = s.find(',', p);
        std::string item = s.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
        size_t at = item.find('@');
        if (at != std::string::npos) events.push_back({item.substr(0, at), strtoull(item.c_str() + at + 1, nullptr, 10)});
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
}

void AutoInput::apply(uint64_t frame) {
    for (auto& e : events) {
        bool on = frame >= e.frame && frame < e.frame + 6;
        if (frame < e.frame || frame > e.frame + 6) continue;
        // "aim2:X/Y", "point2:X/Y" and a trailing 2 on a button are player 2's
        bool aim = e.name.rfind("aim", 0) == 0, point = e.name.rfind("point", 0) == 0;
        int player = aim     ? e.name.rfind("aim2:", 0) == 0
                     : point ? e.name.rfind("point2:", 0) == 0
                             : e.name.size() > 1 && e.name.back() == '2';
        std::string name = aim || point || !player ? e.name : e.name.substr(0, e.name.size() - 1);
        PlayerInputs& pl = g_inputs.p[player];
        if (name == "coin") pl.coin = on;
        else if (name == "start") pl.start = on;
        else if (name == "trigger") pl.trigger = on;
        else if (name == "test") g_inputs.test = on;
        else if (name == "service") g_inputs.service = on;
        else if (name == "reload") pl.offscreen = pl.trigger = on;   // shot outside the screen
        else if (aim && frame == e.frame) {   // aimX/Y: raw gun position from then on
            int x = 128, y = 128;
            if (sscanf(name.c_str() + (player ? 5 : 3), "%d/%d", &x, &y) == 2) pl.gun_x = x, pl.gun_y = y;
        } else if (point && frame == e.frame) {   // pointX/Y: aimed there as the mouse would (fractions of the 4:3 screen)
            float x = 0.5f, y = 0.5f;
            if (sscanf(name.c_str() + (player ? 7 : 6), "%f/%f", &x, &y) == 2) set_player_aim(player, x, y);
        }
    }
}
