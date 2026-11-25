// The window's controls (app.cpp): keyboard, mice and light guns
// (raw_mice.h), gamepads (gamepad.h), all turned into the cabinet's inputs
// (inputs.h). Polled on the window thread, once per picture.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

#include "gamepad.h"

struct Options;
struct PlayerAim;

struct Controls {
    bool menu_open = false;       // a menu has the controls
    bool block_buttons = false;   // after a menu closes: buttons ignored until released
    Gamepads pads;
    GamepadSettings pad_settings;

    // The gun covers the whole 3D view, also the side bands of a wide one: how
    // far it reaches beyond the 4:3 screen (each new picture, as the window may
    // change)
    void update_aim_reach(HWND hwnd, const Options& opt);
    // Keyboard and mouse (only while the window is active):
    //   5 = coin, 1 = start, mouse = gun (left button fires, right reloads),
    //   6 / 2 = player 2's coin and start, F2 = test, F3 = service.
    // With FREE PLAY the game itself needs no credit (g_free_play)
    // (C toggles opt.show_crosshair)
    void poll_keyboard_mouse(HWND hwnd, Options& opt);
    // Gamepads (gamepad.h), after the keyboard and mouse: their buttons are added
    // to those of the player pad_player() gives them. The first pad shares player
    // 1 with the mouse, and whichever moved last aims. START joins the game, takes
    // player 1's sight from the mouse or, pressed by the pad that plays, pauses
    // it; BACK pauses. Returns true when a pad asks for the pause menu.
    bool poll_gamepads(HWND hwnd);
    // Settings > Controls > Hide idle crosshair: a gun that has not moved (more
    // than a hand's tremble, 1% of the screen) nor fired for that many seconds
    // loses its crosshair until it does. A player who joined by mistake (a light
    // gun's start button) leaves no crosshair standing still on the screen
    bool crosshair_idle(int p, const PlayerAim& aim, const Options& opt);

private:
    int p1_pad = -1;            // the gamepad aiming for player 1; -1: the mouse (whichever moved last)
    bool p2_pad = false;        // a gamepad is player 2's (poll_gamepads)
    // While player 2 has a gun of its own (a second mouse, raw_mice.h): the
    // gamepad aiming for player 2 instead, -1: the gun. A pad of player 2's takes
    // the sight only when it aims and the gun has been still for a moment, so a
    // pad that is never touched (a Wii remote that SDL also opens as a gamepad)
    // does not hold player 2's sight where it is
    int p2_pad_aims = -1;
    double p2_gun_moved = -100; // now_seconds() when player 2's gun last moved
    POINT last_cursor{-100000, -100000}, p2_gun_last{-100000, -100000};
    bool c_was_down = false;
    float at_x[2] = {-9, -9}, at_y[2] = {-9, -9};   // crosshair_idle
    double used_at[2] = {};
    bool blocked[Gamepads::MAX] = {}, fire_was[Gamepads::MAX] = {}, reload_was[Gamepads::MAX] = {};   // poll_gamepads
    uint32_t lives_was[2] = {};

    // Window pixels -> fraction of the 4:3 game screen (accounting for the black
    // bars; beyond 0..1 in the side bands of a wider 3D view)
    void aim_at(int player, HWND hwnd, POINT pt, const Options& opt);
};
