// CLASSIC > RANDOM SPAWNS: the arcade game with its encounters shuffled.
//
// Each shooting stop is a script of the game's encounter machine (words at
// 0x1268B8, docs/game_ram.md). When an encounter starts, its words are put back
// as they are in the ROM and then changed in place, keeping their number and
// every camera turn, wait, sound and cutscene: the camera walks the stage path
// as in the arcade. What changes: the spawn position group and sometimes the
// type of each raptor (only for ones the zone uses where the camera looks),
// the position of enemies placed by coordinates (the stage 2 river), when each
// enemy comes, and the order of runs of plain sections (camera turn, enemies,
// wait). Bosses get a random amount of life, and each stage random weather
// (weather.h).
#pragma once
#include <cstdint>
#include <string>

struct Ctx;

bool random_spawns_option(const std::string& name);   // --random-spawns: on from the start (tests)
void random_spawns_hooks();                           // once, before the game thread starts
// Game thread: entry of a hooked function (rt_trace)
void random_spawns_enter(Ctx& c, uint32_t fn);
void random_spawns_frame();   // every vblank
// Window thread: the next arcade game is shuffled or the original
void random_spawns_set(bool on);
bool random_spawns_playing();   // a shuffled game is being played (not its attract or another mode)
