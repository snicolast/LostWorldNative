// What The Lost World's emulator asks of whoever hosts it: the game's own
// window has these (src/runtime/app.cpp), and so do the modes its PC edition
// adds to the cabinet's game, each in a file of its own (survival.cpp,
// random_spawns.cpp, stats.cpp, boss_maker.cpp); inside NicoEngine they are
// stood in for here. Compiled by the engine only (docs/NICOENGINE.md).
//
// Apart from live_game.cpp, which starts the game and gives it its controls:
// a test of the engine's runs the emulator's own code with a game of its own
// in that file's place (its tests/live_test.cpp).
#include "game_flow.h"
#include "ppc_rt.h"
#include "random_spawns.h"
#include "runtime.h"
#include "stats.h"
#include "survival.h"

void frame_log(const char*, ...) {}   // (the game's frame log: a debugging aid of its window)
void console_prompt() {}              // (the terminal the game's own program was started from)

// Records and achievements belong to the game's own program and its save
// folder: a game run here counts nothing
void stats_frame() {}
void stats_enter(Ctx&, uint32_t) {}
void stats_state_loaded() {}

// The survival mode and the shuffled encounters are the PC edition's, picked
// in its menu: here the game is the cabinet's. Neither is ever on, no arena
// reached is kept for them, and no call of the game's is left out
bool survival_enabled() { return false; }
bool survival_owns_inputs() { return false; }
bool survival_frame(Ctx&) { return false; }
void survival_enter(Ctx&, uint32_t) {}
bool survival_spawn_call(Ctx&, uint32_t) { return false; }
void random_spawns_enter(Ctx&, uint32_t) {}
void random_spawns_frame() {}

// Making the boss states (--make-boss-state, its boss_maker.cpp) is a tool of
// the game's own program: never asked for here
int g_boss_maker = -1;
void boss_maker_frame() {}
