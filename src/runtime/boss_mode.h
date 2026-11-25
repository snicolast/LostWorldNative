// Boss mode, the window's side (app.cpp): each fight starts from a game state
// in save/boss, made the first time by LostWorld.exe --make-boss-state N in
// the background (in a job object, so they stop with the game). The game
// side, saving and loading the states, is bosses.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "bosses.h"

class Frontend;

struct BossMode {
    std::vector<HANDLE> makers = std::vector<HANDLE>(BOSS_COUNT, nullptr);
    HANDLE job = nullptr;
    int pending = -1;     // picked: loads once the boot screens are skipped
    bool loading = false; // asked the game thread for it
    int fight = -1;       // the fight on
    double over_at = 0;   // when its stage ended or the game was over
    int lives = 0;        // player 1's lives, last seen before it was out
    double shown_at = 0;  // when the menu closes on the fight just put back
    std::string rom_zip;       // the zip the ROM came from (empty: a data folder): the makers use the same

    // At the start: the job object, which states are made already (the
    // menu's BOSS page) and LW_BOSS
    void init(Frontend& frontend);
    // What the window does for it: run the game unseen while the fight goes
    // in, close the menu on it, and hand the controls to a menu it opened
    struct Steps {
        std::function<void()> run_unseen, close_menu, menu_over;
    };
    // Once per refresh: the state makers, the fight picked, the end of the fight
    void update(Frontend& frontend, const Steps& steps);
    void end(Frontend& frontend);       // no fight and no boss mode (the menu's BOSS pages as from the main menu)
    void to_menu(Frontend& frontend);   // ...and the main menu, with the arcade game at its home state behind it

private:
    void start_maker(int i, Frontend& frontend);
};
