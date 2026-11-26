// The list of game modes and the walks over it (game_modes.h)
#define WIN32_LEAN_AND_MEAN   // runtime.h brings windows.h: no min/max macros
#define NOMINMAX
#include "game_modes.h"

#include "game_flow.h"
#include "ppc_rt.h"
#include "random_spawns.h"
#include "runtime.h"
#include "stats.h"
#include "survival.h"

namespace {

// Survival: waves at one stop of a stage (survival.cpp)
bool survival_call(Ctx& c, uint32_t addr) {
    return c.lr == SURVIVAL_SPAWN_RETURN && survival_spawn_call(c, addr);
}
bool survival_boot_done() {
    return !survival_owns_inputs() || g_frame > 200000;   // (survival gives up first)
}

// Random spawns (random_spawns.cpp)
bool random_spawns_vblank(Ctx&) {
    random_spawns_frame();
    return false;
}

// Play statistics (stats.cpp)
bool stats_vblank(Ctx&) {
    stats_frame();
    return false;
}

// --make-boss-state (boss_maker.cpp)
bool boss_maker_on() { return g_boss_maker >= 0; }
bool boss_maker_vblank(Ctx&) {
    if (g_boss_maker >= 0) boss_maker_frame();
    return false;
}

// In the order their vblank runs
const GameMode MODES[] = {
    {.name = "survival", .on_function = survival_enter, .on_call = survival_call, .on_vblank = survival_frame,
     .takes_over = survival_enabled, .owns_boot = survival_enabled, .boot_done = survival_boot_done},
    {.name = "stats", .on_function = stats_enter, .on_vblank = stats_vblank, .on_state_loaded = stats_state_loaded},
    {.name = "random spawns", .on_function = random_spawns_enter, .on_vblank = random_spawns_vblank},
    {.name = "boss maker", .on_vblank = boss_maker_vblank, .takes_over = boss_maker_on},
};

}  // namespace

void game_modes_function(Ctx& c, uint32_t fn) {
    for (const GameMode& m : MODES)
        if (m.on_function) m.on_function(c, fn);
}

bool game_modes_call(Ctx& c, uint32_t addr) {
    for (const GameMode& m : MODES)
        if (m.on_call && m.on_call(c, addr)) return true;
    return false;
}

bool game_modes_vblank(Ctx& c) {
    bool hold = false;
    for (const GameMode& m : MODES)
        if (m.on_vblank && m.on_vblank(c)) hold = true;
    return hold;
}

void game_modes_state_loaded() {
    for (const GameMode& m : MODES)
        if (m.on_state_loaded) m.on_state_loaded();
}

bool game_mode_takes_over() {
    for (const GameMode& m : MODES)
        if (m.takes_over && m.takes_over()) return true;
    return false;
}

const GameMode* game_mode_owning_boot() {
    for (const GameMode& m : MODES)
        if (m.owns_boot && m.owns_boot()) return &m;
    return nullptr;
}
