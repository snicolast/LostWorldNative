// Survival mode (wave arena) and its investigation trace. See docs/game_ram.md
// for the game addresses used here.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Ctx;

bool survival_option(const std::string& name, const char* value);   // --trace-survival, --survival S:Z
// Game thread
bool survival_frame(Ctx& c);                  // start of every frame (vblank); true: hold the game
void survival_enter(Ctx& c, uint32_t fn);     // entry of a hooked function (through the --trace filter)
bool survival_spawn_call(Ctx& c, uint32_t fn);   // the zone trigger task calls a spawn function; true: skip it
constexpr uint32_t SURVIVAL_SPAWN_RETURN = 0x000148B4;   // return address of that call in fn_00014764

// Arenas that work (stages that use the encounter machine), for the menu
struct SurvivalZone {
    int stage, zone;          // as the player counts them (stage from 1)
    const wchar_t* name;
    bool shoot_in;            // the way there waits for shots (the loading shoots, lives kept)
    int encounter;            // its first encounter (the start of the arena)
    bool indoor;              // inside a building: no weather to choose (the game's own light)
};
extern const SurvivalZone SURVIVAL_ZONES[];
extern const int SURVIVAL_ZONE_COUNT;
const wchar_t* survival_zone_name(int stage, int zone);   // nullptr if not in the list
// From save/survival.ini: the best wave, its score and (if asked) its lap
bool survival_record(int stage, int zone, int& wave, int& score, int* lap = nullptr);
// Arenas are unlocked by reaching them in the arcade game (or having a record
// there); the arcade game keeps the arena's state on the way (save/survival/)
bool survival_unlocked(int stage, int zone);
std::vector<int> survival_take_unlocked();   // indices in SURVIVAL_ZONES unlocked since the last call (the notice)

// What the window shows and does
enum class SurvivalMode { Off, Loading, Ready, Playing, Results };
// Getting to the arena, for the loading screen
enum class SurvivalStep { Preparing, Coin, Course, Travelling, Arrived };
struct SurvivalView {
    SurvivalMode mode = SurvivalMode::Off;
    int stage = 0, zone = 0;   // as the player counts them (stage from 1)
    int wave = 0;              // from 1
    int score = 0, lives = 0;
    int lap = 1;               // laps of the zone, from 1
    int best_wave = 0, best_score = 0, best_lap = 0;   // record before this game
    bool new_record = false;
    double wave_started = 0;   // now_seconds() (common/clock.h) when the wave on screen began (banner)
    SurvivalStep step = SurvivalStep::Preparing;
    float progress = 0;        // 0..1 of the way in
    int left = 0, total = 0;   // dinosaurs of the wave still to beat (alive or to come), of how many
    bool resting = false;      // the stop is cleared: the pause, the walk on, the way back to the first stop
    float rest_left = 0;       // seconds of the pause before the walk
    bool walking = false;      // the game carries the camera on to the next stop
    bool ambush = false;       // dinosaurs on the way: the walk waits for them
    bool zone_done = false;    // the zone's last stop is cleared: a new lap from the first one
    double results_at = 0;     // when the results screen came up (same clock)
    double lap_started = 0, ambush_started = 0;   // banners (same clock)
    float fade = 0;            // 0..1 black over everything (the way back to the first stop)
    // The golden BONUS target, a fraction of the 4:3 game screen, and how much of its time is left
    bool bonus_on = false;
    float bonus_x = 0, bonus_y = 0, bonus_left = 0;
    int reward = 0;            // last reward: 1 a life, 2 points (when lives were full)
    double reward_at = 0;      // when (same clock)
};
// Every wave begins with this countdown (seconds from SurvivalView::wave_started):
// the game runs, the player can aim and shoot, and no enemy comes until it ends
constexpr double SURVIVAL_COUNTDOWN = 5.0;
// The results take shots only after this long: the player is still firing when the hunt ends
constexpr double SURVIVAL_RESULTS_DELAY = 2.0;
void survival_hooks();         // once, before the game thread starts
bool survival_cli_request(int& stage, int& zone);   // --survival S:Z given
// Window thread: starts a run (the game should be at the attract, or about to
// be put back there: gated until survival_ungate) and ends it
void survival_begin(int stage, int zone);
void survival_ungate();
void survival_end();
bool survival_enabled();
SurvivalView survival_view();
bool survival_owns_inputs();   // the mode drives the controls (getting into the stage)
void survival_confirm();       // shot or Enter on the start / results screen
