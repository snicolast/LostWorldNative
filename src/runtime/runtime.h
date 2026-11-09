// What the window side (app.cpp) uses of the emulator in runtime.cpp: the
// game thread and the state the two threads share. Everything else in
// runtime.cpp is private to the emulation.
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "eeprom.h"
#include "inputs.h"
#include "../audio/audio_out.h"
#include "../audio/sound_board.h"
#include "../video/r3d_state.h"
#include "../video/tilegen.h"
#include "../common/clock.h"

// ROMs, loaded by main() from g_data_dir
extern std::vector<uint8_t> g_crom, g_banked, g_vrom;
extern std::string g_data_dir;
bool load_rom(const std::string& name, std::vector<uint8_t>& out, bool required);

// Run limits and tracing (--frames, --diff, --trace)
extern int g_frame_limit;          // with --headless, 600 by default
extern bool g_collect_stats;       // keep access statistics and the full texture stream for report()
extern std::map<uint32_t, uint64_t> g_trace_hits;   // function -> hit count
extern uint32_t g_diff_fn;
extern uint64_t g_diff_frame;
extern int g_boss_maker;           // --make-boss-state: boss index, -1 when not making one
extern std::atomic<int> g_subtitles;   // 0 none, 1 English, 2 Japanese (the board's own)

// Devices shared with the window thread
extern Eeprom93C46 g_eeprom;
extern SoundBoard g_sound;
extern AudioOut g_audio;
extern AutoInput g_auto;

// The game thread: runs the recompiled program on a 512 MB native stack
DWORD WINAPI game_thread(LPVOID);
void frame_log(const char* fmt, ...);   // LW_FRAMELOG (app.cpp); does nothing without it
extern LARGE_INTEGER g_qpf, g_start_time;
extern bool g_realtime;            // pace the game to real time (off with LW_FAST)
extern std::atomic<bool> g_boot_skip;   // the power-on screens run through with nothing on screen
extern std::atomic<bool> g_hold;        // menus: the game stands still while set
// The menu stands in for the arcade's attract (runtime.cpp): with
// g_skip_attract set (app.cpp: the menu on, classic arcade), a game that ends
// opens the main menu (g_game_over_menu) instead of the attract, with the
// home state put back, and g_arcade_entry (1 to start it, PLAY) runs the game
// on to its route selection unseen, with a credit and START
extern std::atomic<bool> g_skip_attract, g_game_over_menu;
// The game is in its attract (logos, title, demo, rankings, INSERT COIN):
// app.cpp shows the author's credit over it
extern std::atomic<bool> g_in_attract;
extern std::atomic<int> g_arcade_entry;
extern std::atomic<bool> g_quit, g_parked;   // shutdown handshake: ask, then wait for the park
extern std::atomic<uint64_t> g_game_busy_us, g_game_frames;   // CPU time of the game thread, read once a second

// Aim assist: the enemies the game placed on screen during its last frame,
// from its own world-to-screen projection (fn_0003BE44, called by each enemy
// with the point it stands on). x, y in pixels of the 496x384 screen; scale =
// screen pixels per world unit at the enemy's distance
struct EnemyMark {
    float x, y, scale;
};
std::vector<EnemyMark> enemy_marks();

// What the game drew: the last closed 3D frame and the 2D layer memory of the
// frame on screen, each behind its mutex
extern std::mutex g_r3d_mutex;
extern std::shared_ptr<const R3DFrameState> g_r3d_latest;
extern std::condition_variable g_r3d_cv;   // notified (after the mutex is released) when g_r3d_latest changes
extern R3DTextureRam g_texture_ram;
extern std::mutex g_video_mutex;
extern std::vector<uint8_t> g_video_vram;
extern uint64_t g_video_frame;
// Bumped each time a page of g_video_vram is copied again (g_video_mutex):
// a copy of it only needs the pages whose number has changed since
extern uint64_t g_video_page_serial[TILEGEN_PAGES];
