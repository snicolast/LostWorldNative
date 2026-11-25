// The rules of the game's flow that the port adds around the arcade game
// (game_flow.cpp): the subtitles' language and the stage captions, FREE
// PLAY, who is playing, the menu standing in for the attract, PLAY's way in
// and the modes' frames. The emulator calls them at each vblank
// (runtime.cpp, rt_event); app.cpp drives them through runtime.h's flags.
// Internal to the game thread.
#pragma once
#include <cstdint>

struct Ctx;

// Start of the frame: language, stage captions, FREE PLAY, who plays
void game_flow_frame_start();
// After the scripted inputs: the attract guard and flag, survival, stats,
// random spawns, the boss maker, and PLAY given up
void game_flow_modes(Ctx& c);
// PLAY > CLASSIC on its way to the route selection (g_arcade_entry): true
// when it is there, or gave up
bool arcade_entry_frame();
// --make-boss-state (boss_maker.cpp)
void boss_maker_frame();

// What the emulator shares with them (runtime.cpp)
extern uint64_t g_frame;              // vblanks since power-on
extern bool g_logo_up;                // the boot has reached the SEGA logo once
extern bool g_attract_watch_reset;    // a state was put back: no game ended there
bool game_state_pending();            // a state load or save is under way
[[noreturn]] void exit_from_game_thread(int code);
