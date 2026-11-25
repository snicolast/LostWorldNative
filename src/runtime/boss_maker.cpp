// Boss mode: making the boss states (game_flow.h). The fights themselves,
// from the window's side, are boss_mode.cpp; saving and loading the states,
// bosses.h
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>

#include "bosses.h"
#include "game_flow.h"
#include "game_ram.h"
#include "inputs.h"
#include "ppc_rt.h"
#include "runtime.h"
#include "../audio/scsp.h"

// ---- Boss mode: making the boss states ----
// --make-boss-state N plays by itself from the attract to boss N: it puts a
// credit in, starts, picks the boss's stage in the course select (the run's
// stage list at 0x1C2AFC) and keeps shooting, with its lives frozen; when the
// stage script reaches the fight it clears the score and saves the state.
int g_boss_maker = -1;   // index in BOSSES, -1 when not making one

void boss_maker_frame() {
    const BossInfo& b = BOSSES[g_boss_maker];
    static uint8_t lives = 0;
    static bool saving = false;
    uint64_t f = g_frame;
    std::string path = boss_state_path(g_boss_maker);
    if (saving) {
        int r = game_state_result();
        if (r == 0) return;
        bool ok = r > 0 && MoveFileExA((path + ".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        printf("[boss] %ls: %s (frame %llu; %u notes restarted on stopped voices)\n", b.name,
               ok ? "state saved" : "could not save", (unsigned long long)f, unsigned(Scsp::restarted()));
        exit_from_game_thread(ok ? 0 : 1);
    }
    if (f > 250000) {
        printf("[boss] %ls: the game did not reach the boss\n", b.name);
        exit_from_game_thread(1);
    }
    if (f < 900) return;   // past the boot (a button held there changes what it does)
    uint32_t select = rd32(ram::COURSE_SELECT);   // 0 attract, 1-2 choosing, 0xC playing
    if (select == 0) {                  // a credit and start, again until the course select opens
        auto_coin_start(g_inputs.p[0], f);
        return;
    }
    g_inputs.p[0].coin = g_inputs.p[0].start = false;
    if (select != 0xC) {   // the boss's stage first in the run
        if (int32_t(rd32(ram::PLAYLIST_INDEX)) == -1) wr32(ram::PLAYLIST, uint32_t(b.stage_index));
        return;
    }
    if (!lives) lives = rd8(ram::P1_LIVES_BYTE);
    if (lives) wr8(ram::P1_LIVES_BYTE, lives);
    if (rd8(ram::STAGE) == b.stage_index && int32_t(rd32(ram::VM_ENCOUNTER)) == b.label && int32_t(rd32(ram::VM_PC)) >= b.index) {
        g_inputs.p[0].trigger = g_inputs.p[0].offscreen = false;
        wr32(ram::P1_SCORE, 0);   // the score starts at zero
        wr32(ram::P1_STAGE_SCORES + 4 * 2, 0);   // 0x1A8594, stage 3's (the boss's own would be + 4 * b.stage_index)
        game_state_save(path + ".tmp");
        saving = true;
        return;
    }
    auto_sweep_fire(g_inputs.p[0], f);
}
