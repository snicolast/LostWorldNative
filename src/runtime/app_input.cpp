// The window's controls (app_input.h)
#include "app_input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

#include "game_ram.h"
#include "inputs.h"
#include "options.h"
#include "ppc_rt.h"
#include "raw_mice.h"
#include "runtime.h"
#include "survival.h"
#include "../common/clock.h"
#include "../video/r3d_renderer.h"

void Controls::aim_at(int player, HWND hwnd, POINT pt, const Options& opt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    ScreenLayout lay = compute_layout(float(rc.right), float(rc.bottom), opt.max_aspect);
    float x = (pt.x - lay.ox - lay.game_x0 * lay.scale) / (SCREEN_W * lay.scale);
    float y = (pt.y - lay.oy) / (SCREEN_H * lay.scale);
    set_player_aim(player, x, y);
}


void Controls::update_aim_reach(HWND hwnd, const Options& opt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    ScreenLayout lay = compute_layout(float(rc.right), float(rc.bottom), opt.max_aspect);
    g_aim_reach = lay.game_x0 / SCREEN_W;
}


// A shot outside the 3D view (the black bars of a window wider than it) or on
// its very edge (where a light gun puts a shot that misses the screen)
// reloads, as on the cabinet, instead of firing at the edge
static void shot_outside_reloads(int player) {
    PlayerInputs& pl = g_inputs.p[player];
    PlayerAim a = player_aim(player);   // the very edge too: where a gun can park an off-screen shot
    float reach = gun_reach();
    if (pl.trigger && (a.x <= -reach || a.y <= 0 || a.x >= 1 + reach || a.y >= 1)) {
        pl.trigger = false;
        pl.offscreen = true;
    }
}

void Controls::poll_keyboard_mouse(HWND hwnd, Options& opt) {
    // Scripted input (--auto) takes over the controls
    if (GetForegroundWindow() != hwnd || !g_auto.events.empty() || survival_owns_inputs() || g_arcade_entry) {
        if (g_auto.events.empty() && !survival_owns_inputs() && !g_arcade_entry)   // in the background: no gun stays held
            for (PlayerInputs& pl : g_inputs.p) pl.trigger = pl.offscreen = false;
        raw_mice::update(hwnd, false, false);
        return;
    }
    // A second mouse becomes player 2's when it clicks during a game, also
    // after player 2 joined with start (a light gun's button) before firing,
    // unless a gamepad is player 2's
    const PlayerInputs &in1 = g_inputs.p[0], &in2 = g_inputs.p[1];
    bool join_allowed = opt.several_mice && !menu_open && (in1.playing || in2.playing) && !(in2.playing && p2_pad);
    raw_mice::update(hwnd, join_allowed, !menu_open);
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (menu_open) {
        g_inputs.test = g_inputs.service = false;
        for (PlayerInputs& pl : g_inputs.p) pl.coin = pl.start = pl.trigger = pl.offscreen = false;
        return;
    }
    PlayerInputs& p1 = g_inputs.p[0];
    p1.coin = down('5');
    g_inputs.p[1].coin = down('6');
    g_inputs.p[0].start = down('1');
    g_inputs.p[1].start = down('2');
    g_inputs.test = down(VK_F2);
    g_inputs.service = down(VK_F3);
    POINT cursor;
    GetCursorPos(&cursor);
    if (cursor.x != last_cursor.x || cursor.y != last_cursor.y) p1_pad = -1;   // the mouse moved: it aims again
    last_cursor = cursor;
    if (raw_mice::two_mice()) {   // each mouse its own gun; player 2's click joins while player 1 plays
        for (int p = 0; p < 2; p++) {
            raw_mice::Gun g = raw_mice::gun(p, hwnd);
            PlayerInputs& pl = g_inputs.p[p];
            if (p == 1 && g.clicked && !pl.playing && p1.playing) player_join(1);
            pl.trigger = g.trigger;
            pl.offscreen = g.reload;
            if (p == 1) {
                if (g.pos.x != p2_gun_last.x || g.pos.y != p2_gun_last.y) {   // the gun moved: it aims again
                    p2_pad_aims = -1;
                    p2_gun_moved = now_seconds();
                }
                p2_gun_last = g.pos;
            }
            if (p == 1 ? p2_pad_aims < 0 : p1_pad < 0) {
                aim_at(p, hwnd, g.pos, opt);
                shot_outside_reloads(p);
            }
        }
    } else {
        p1.trigger = down(VK_LBUTTON);
        p1.offscreen = down(VK_RBUTTON);
        g_inputs.p[1].trigger = g_inputs.p[1].offscreen = false;   // player 2's pad adds its buttons after this
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (p1_pad < 0) {
            aim_at(0, hwnd, pt, opt);
            shot_outside_reloads(0);
        }
    }
    if (block_buttons) {   // the click that closed the menu does not fire
        bool held = false;
        for (PlayerInputs& pl : g_inputs.p) held |= pl.trigger || pl.offscreen;
        if (held) for (PlayerInputs& pl : g_inputs.p) pl.trigger = pl.offscreen = false;
        else block_buttons = false;
    }
    // C toggles the extra crosshair
    bool c_down = down('C');
    if (c_down && !c_was_down) opt.show_crosshair = !opt.show_crosshair;
    c_was_down = c_down;
}

bool Controls::crosshair_idle(int p, const PlayerAim& aim, const Options& opt) {
    double now = now_seconds();
    const PlayerInputs& pl = g_inputs.p[p];
    if (pl.trigger || pl.offscreen || std::fabs(aim.x - at_x[p]) > 0.01f || std::fabs(aim.y - at_y[p]) > 0.01f) {
        at_x[p] = aim.x, at_y[p] = aim.y;
        used_at[p] = now;
    }
    return opt.crosshair_hide > 0 && now - used_at[p] > double(opt.crosshair_hide);
}


// The aim assist's targets: a box around each enemy the game placed on
// screen, from the point it stands on up the body, sized with its distance.
// In world units (LW_AIM_BOX=up,half_width,half_height to try others)
static std::vector<AimTarget> aim_targets() {
    static float up = 7, half_w = 6, half_h = 5;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("LW_AIM_BOX")) sscanf(e, "%f,%f,%f", &up, &half_w, &half_h);
    }
    std::vector<AimTarget> out;
    for (const EnemyMark& m : enemy_marks()) {
        float cy = m.y - up * m.scale, w = half_w * m.scale, h = half_h * m.scale;
        out.push_back({(m.x - w) / SCREEN_W, (cy - h) / SCREEN_H, (m.x + w) / SCREEN_W, (cy + h) / SCREEN_H});
    }
    return out;
}

bool Controls::poll_gamepads(HWND hwnd) {
    bool focused = GetForegroundWindow() == hwnd;
    pads.set_targets(aim_targets());
    pads.poll(pad_settings, focused, menu_open);
    uint32_t connected = 0;
    for (int i = 0; i < Gamepads::MAX; i++)
        if (pads.pad(i).connected) connected |= 1u << i;
    if (p1_pad >= 0 && !(connected >> p1_pad & 1)) p1_pad = -1;   // unplugged: the mouse aims again
    if (p2_pad_aims >= 0 && !(connected >> p2_pad_aims & 1)) p2_pad_aims = -1;
    bool p2_gun = raw_mice::two_mice();
    if (!p2_gun) p2_pad_aims = -1;
    p2_pad = false;
    for (int i = 0; i < Gamepads::MAX; i++)
        if (connected >> i & 1 && pad_player(i, connected) == 1) p2_pad = true;
    if (menu_open) std::fill(std::begin(blocked), std::end(blocked), true);   // the button that closes it does not fire
    if (!focused || menu_open || !g_auto.events.empty() || survival_owns_inputs() || g_arcade_entry) return false;
    bool pause = false;
    for (int i = 0; i < Gamepads::MAX; i++) {
        const PadState& pad = pads.pad(i);
        if (!pad.connected) continue;
        int p = pad_player(i, connected);
        PlayerInputs& pl = g_inputs.p[p];
        if (pad.aimed && p == 0) p1_pad = i;
        if (pad.aimed && p == 1 && p2_gun && now_seconds() - p2_gun_moved > 0.5) p2_pad_aims = i;
        bool aims = p == 0 ? p1_pad == i : !p2_gun || p2_pad_aims == i;
        if (aims) set_player_aim(p, pad.aim_x, pad.aim_y);
        else {   // the mouse (or player 2's gun) aims: the pad's sight waits where it is
            PlayerAim a = player_aim(p);
            pads.set_aim(i, a.x, a.y);
        }
        bool fire = pad.fire, reload = pad.reload;
        if (blocked[i]) {
            if (fire || reload) fire = reload = false;
            else blocked[i] = false;
        }
        if (fire) pl.trigger = true;
        if (reload) pl.offscreen = true;
        if (fire && !fire_was[i]) {
            pads.rumble(i, 0.5f, 0.85f, 90);
            pads.rumble_triggers(i, 0.0f, 1.0f, 90);
        } else if (reload && !reload_was[i]) {
            pads.rumble(i, 0.6f, 0.2f, 70);
            pads.rumble_triggers(i, 0.8f, 0.0f, 70);
        }
        fire_was[i] = fire, reload_was[i] = reload;
        if (pad.back) pause = true;
        if (pad.start) {
            if (pl.playing && (p == 1 || p1_pad == i)) pause = true;
            else if (pl.playing) p1_pad = i;   // player 1 plays with the mouse (a pad plugged in mid-game)
            else {
                if (p == 0) p1_pad = i;   // this pad starts player 1's game
                player_join(p);
            }
        }
    }
    // A hit takes a life (game_ram.md): the pads of that player that were used
    // in the last minute rumble
    for (int p = 0; p < 2; p++) {
        uint32_t lives = rd32(p ? ram::P2_LIVES : ram::P1_LIVES);
        if (g_inputs.p[p].playing && lives < lives_was[p])
            for (int i = 0; i < Gamepads::MAX; i++)
                if (pads.pad(i).connected && pad_player(i, connected) == p && pads.now() - pads.pad(i).last_used < 60) {
                    pads.rumble(i, 1.0f, 0.9f, 380);
                    pads.rumble_triggers(i, 1.0f, 1.0f, 300);
                }
        lives_was[p] = lives;
    }
    return pause;
}
