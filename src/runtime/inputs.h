// Player controls. Written by the window (keyboard/mouse), the pads or by
// --auto, and read by the emulated hardware at 0xF0040000. The cabinet has two
// guns: p[0] is player 1 and p[1] player 2.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct PlayerInputs {
    std::atomic<bool> coin{false}, start{false}, trigger{false}, offscreen{false};
    std::atomic<int> gun_x{128}, gun_y{128};   // raw ADC values, 0..255
    std::atomic<bool> playing{false};          // in the game (set by the game thread from its RAM)
};

struct Inputs {
    PlayerInputs p[2];
    std::atomic<bool> service{false}, test{false};

    // F0040004, active low. Bank 0: bit0 coin 1, bit1 coin 2, bit2 test,
    // bit3 service, bit4 start 1, bit5 start 2 (fn_001180A8 packs bit 4 with
    // player 1's port and bit 5 with player 2's). Bank 1: bit5 = EEPROM (set by runtime)
    uint8_t system_port(bool bank1) const {
        uint8_t v = 0xFF;
        if (!bank1) {
            if (p[0].coin) v &= ~0x01;
            if (p[1].coin) v &= ~0x02;
            if (test) v &= ~0x04;
            if (service) v &= ~0x08;
            if (p[0].start) v &= ~0x10;
            if (p[1].start) v &= ~0x20;
        }
        return v;
    }
    // F0040008 player 1, F004000C player 2: bit0 trigger, bit1 second trigger (used to reload)
    uint8_t player_port(int player) const {
        uint8_t v = 0xFF;
        if (p[player].trigger) v &= ~0x01;
        if (p[player].offscreen) v &= ~0x02;
        return v;
    }
    // ADC channels: 0 = P1 X, 1 = P1 Y, 2 = P2 X, 3 = P2 Y (raw values; the window maps the mouse)
    uint8_t adc(int channel) const {
        if (channel >= 4) return 0;
        const PlayerInputs& pl = p[channel >> 1];
        return uint8_t(channel & 1 ? pl.gun_y : pl.gun_x);
    }
};

extern Inputs g_inputs;

// Testing aids (the boss states, survival's way in, LW_SURV_AUTOPLAY): a
// credit and start every 240 frames while `on`, and shots swept over the
// screen with a reload now and then
inline void auto_coin_start(PlayerInputs& p, uint64_t f, bool on = true) {
    p.coin = on && f % 240 < 6;
    p.start = on && f % 240 >= 60 && f % 240 < 66;
}
inline void auto_sweep_fire(PlayerInputs& p, uint64_t f) {
    p.trigger = f % 12 < 3;
    p.offscreen = f % 240 < 3;
    p.gun_x = int(40 + (f * 7) % 180);
    p.gun_y = int(40 + (f * 3) % 170);
}

// Aim of a player as a fraction of the 4:3 game screen (outside 0..1 = off
// it, or in the side bands of a wide 3D view): sets that gun's ADC values and
// keeps it for the crosshair. The mouse, the second mouse and the pads all
// aim through here.
void set_player_aim(int player, float x, float y);
struct PlayerAim { float x, y; bool inside; };   // inside: in the 3D view
PlayerAim player_aim(int player);
// How far the 3D view reaches beyond each side of the 4:3 screen, as a
// fraction of its width (0 = the original 4:3 picture). Set by the window
extern std::atomic<float> g_aim_reach;
// The reach the gun has now: none on the stage select screen, whose dot the
// game keeps on its 4:3 picture
float gun_reach();
// In a wide 3D view, the aim in a stage in the game's own units (what
// fn_0000CBF0 makes of the ADC values) for where the gun points, also beyond
// the 4:3 screen, which the 8-bit ADC values cannot reach. False: the game's
// own stays (4:3, --aim, or a gun moved by something else)
bool gun_aim_exact(int player, float& out_x, float& out_y);
extern std::atomic<bool> g_select_aim;   // the stage select screen is up (its gun mapping differs)
extern std::atomic<bool> g_fixed_aim;    // --aim: the ADC values stay fixed
// FREE PLAY (settings, --free-play): the game's own free play, as its test
// menu sets it (runtime.cpp, each frame): START alone starts and continues
extern std::atomic<bool> g_free_play;

// Which player a pad controls (0 or 1), given the connected pads as a bit
// mask: the first connected pad shares player 1 with the mouse, whenever it
// was plugged in, and the others are player 2's
int pad_player(int pad_index, uint32_t connected);

// Joining a game in progress: a credit and then that player's start, as the
// arcade asks for (with FREE PLAY the start alone). Driven by the clock, so
// whoever polls the controls (the window at the monitor's rate) calls
// join_update() each time.
void player_join(int player);
void join_update();

// --auto "coin@1500,start@1600": scheduled presses (6 frames each); a "2"
// suffix is player 2's (coin2, start2, trigger2, reload2) and so are aim2:X/Y
// (raw ADC values) and point2:X/Y (fractions of the 4:3 screen, as the mouse)
struct AutoInput {
    struct Event { std::string name; uint64_t frame; };
    std::vector<Event> events;
    void parse(const std::string& s);
    void apply(uint64_t frame);
};
