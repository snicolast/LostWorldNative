// The port's game modes, as the emulator sees them (game_modes.cpp): a list
// of hooks it calls without naming any mode. A new mode adds its entry to
// the table in game_modes.cpp; runtime.cpp and game_flow.cpp stay as they
// are. Game thread only.
#pragma once
#include <cstdint>

struct Ctx;

// Every hook may be null
struct GameMode {
    const char* name;
    // Entry of a hooked function (rt_trace, through the --trace filter)
    void (*on_function)(Ctx& c, uint32_t fn);
    // An indirect call (rt_call); true: the mode handled it, skip the call
    bool (*on_call)(Ctx& c, uint32_t addr);
    // Each vblank, after the scripted inputs; true: hold the game this frame
    bool (*on_vblank)(Ctx& c);
    // A game state was put back
    void (*on_state_loaded)();
    // The mode stands in for the arcade game (no attract, no FREE PLAY
    // continue, no home state from the boot)
    bool (*takes_over)();
    // The mode drives the boot itself: boot_done says when it is over
    bool (*owns_boot)();
    bool (*boot_done)();
};

void game_modes_function(Ctx& c, uint32_t fn);
bool game_modes_call(Ctx& c, uint32_t addr);
bool game_modes_vblank(Ctx& c);   // true: some mode holds the game
void game_modes_state_loaded();
bool game_mode_takes_over();      // a mode stands in for the arcade game
const GameMode* game_mode_owning_boot();   // null: the arcade game's own boot
