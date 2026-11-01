// Survival mode (wave arena) and its investigation trace.
//
// --survival STAGE:ZONE takes the player round that zone, stop after stop,
// in laps. The game's own encounter machine does the work: an encounter is a
// script of words at 0x1268B8 run by the task fn_0003951C, and the camera only
// walks on to the next stop when that task ends. The first encounter is copied
// to free RAM with its end replaced by an endless wait, so the task never
// ends; each time it reaches that wait the stop is cleared. Then the game's
// own walker is let go (0x1A3960, as the end of an encounter does): it carries
// the camera along the stage's path, turning and climbing as in the arcade,
// and the next stop trigger stops it. The encounter of that stop is skipped
// (the task is still the first one) and a wave is written from its enemy
// calls and camera turns instead, more of them every wave. Sometimes the walk
// is cut short by an ambush, and sometimes a golden BONUS target shows up.
// After the zone's last stop the game state kept at the first one is put
// back (game_state_mark) under a fade to black: a new, harder lap.
//
// Getting there is automatic: from the attract (the window puts the game back
// there first, see app.cpp) the coin, start and course select are pressed
// while the game runs at full speed and muted behind the loading screen, and
// the course is made to start at the chosen stage. The game then waits on the
// start screen. The last hit never takes the player's last life (the hit functions
// get one more first), so the game never reaches its CONTINUE screen: that hit
// ends the hunt with the results screen; playing again starts over there.
//
// --trace-survival writes out/survival_trace.txt: per frame the stage, zone,
// camera, tasks and the encounter machine, plus every task the game creates and
// every call of the zone trigger task. The addresses are in docs/game_ram.md.
#include "survival.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "../common/clock.h"
#include "../common/random_seed.h"
#include "bosses.h"
#include "encounter_script.h"
#include "inputs.h"
#include "ppc_rt.h"
#include "weather.h"

namespace {

// Game RAM (docs/game_ram.md)
constexpr uint32_t STAGE = 0x1A623C;        // byte: current stage (0 = stage 1)
constexpr uint32_t ZONE = 0x1A3984;         // low nibble: current zone of the stage
constexpr uint32_t CAMERA_POS = 0x1A6F28;   // floats x, y, z: camera position
constexpr uint32_t TASK_HEAD = 0x1C10B0;    // first task; node +8 next, +0xC function
constexpr uint32_t FN_TASK_NEW = 0x00116A48;   // (size, fn) -> task
constexpr uint32_t PLAYER_LIFE = 0x1A3720;  // player 1 object (0x1A36C8) + 0x58: lives left
constexpr uint32_t FN_PLAYER_HIT[2] = {0x000463D4, 0x00071460};   // hurt the player: lives - 1
// Player 1's score, the score its counter shows (fn_0003E800 draws the digits
// again when the two differ; the game writes -1 there to force it), then the score of each
constexpr uint32_t SCORE[7] = {0x1A857C, 0x1C1D54,
                               0x1A858C, 0x1A8590, 0x1A8594, 0x1A8598, 0x1A859C};   // stage (0x1A858C + 4 * stage)
constexpr uint32_t SETTING_LIFE = 0x122A;   // byte: LIFE setting (RAM copy of the EEPROM)
constexpr uint32_t SETTING_DIFF = 0x122D;   // high nibble: difficulty (RAM copy, read live)
constexpr uint32_t PLAYLIST = 0x1C2AFC;    // 5 ints: stages of the course, in order
constexpr uint32_t PLAYLIST_INDEX = 0x1C2B10;
constexpr uint32_t FN_NEXT_STAGE = 0x00081364;   // stage byte = PLAYLIST[++PLAYLIST_INDEX]
constexpr uint32_t FN_PLAYLIST_INIT = 0x000812DC;   // PLAYLIST = 0..4, PLAYLIST_INDEX = -1 (new game)
// Encounter machine (its script: encounter_script.h)
using script::FN_ENCOUNTER_START;
constexpr uint32_t FN_ENCOUNTER = 0x0003951C;         // the task that runs the script
constexpr uint32_t FN_STOP = 0x0003B3F4;              // zone trigger: the camera stops (CAM_WALK = 0)
constexpr uint32_t TRIGGER_TASK = 0x1C12A0;           // the zone trigger task (fn_00014764); +0: its first node
constexpr uint32_t VM_PC = 0x1A847C;
constexpr uint32_t VM_ALIVE = 0x1A8468;     // enemies alive (what op -1 waits for)
constexpr uint32_t VM_ALIVE2 = 0x1A83D4;    // the count op -1 waits for while flag 0x200 is set
constexpr uint32_t VM_FLAGS = 0x1A8414;
constexpr uint32_t VM_WAIT = 0x1A842C;      // frames waited by the current op
constexpr uint32_t VM_COUNTDOWN = 0x1C1D0C; // op -21: frames left (loaded from its argument when 0)
constexpr uint32_t LOOK_AT = 0x1A3454;      // floats: point the camera turns to (op -9)
// Camera walk along the stage path
constexpr uint32_t CAM_WALK = 0x1A3960;     // 1: walk on (set when an encounter ends), 0: stop (trigger 0x3B3F4)
constexpr uint32_t CAM_NODE = 0x1A3A0C;     // node of the path the camera heads for
constexpr uint32_t CAM_SPEED = 0x1A39C8;    // float: speeds up and slows down by itself
// Gait (fn_0001A4DC writes them; the stop trigger fn_0001A5B4 too): target speed
// 0..0xFF, 0 walking / 0xFF stopped, and how fast it gets there
constexpr uint32_t CAM_GAIT_SPEED = 0x1A3994, CAM_GAIT_HALT = 0x1A399C, CAM_GAIT_RAMP = 0x1A3990;
// Free RAM (the game uses up to 0x200000) for the survival scripts
constexpr uint32_t FREE_RAM = 0x700000;
constexpr int32_t WAIT_FOREVER = 0x3FFFFFFF;
constexpr uint64_t CLEAR_FRAMES = 230;      // a stop cleared: the walk on starts after this (4 s), less every lap
constexpr uint64_t WALK_LONGEST = 3600;     // a walk that never reaches a stop (1 min): the lap ends there
constexpr uint64_t FADE_FRAMES = 40;        // the fade to black at the end of a lap, and back
constexpr int32_t COUNTDOWN_FRAMES = int32_t(SURVIVAL_COUNTDOWN * 57.5);   // the countdown, as a -21 wait
constexpr int WAVE_MIN = 8, WAVE_MAX = 30;  // dinosaurs in a wave: at least as many as the arena's first (the game's own)
using script::FN_RAPTOR;
// The dead triceratops of the arcade's story (task 0x5B830, only in encounter 3,
// the Jungle shack): survival's first wave runs that encounter, so it is left out
constexpr uint32_t FN_DEAD_TRICERATOPS = 0x0005B758;
// Its raptors (encounter 3, group 1) stand on it; without it they would stand
// in the air, so they come from the next group of that stop, on the ground
constexpr int TRICERATOPS_ENCOUNTER = 3, TRICERATOPS_GROUP = 1, GROUND_GROUP = 2;
constexpr float NEAR_LOOK = 120;            // another stop's section joins a wave if it looks this near one of this stop's
constexpr int AMBUSH_CHANCE = 35;           // % of the walks with an ambush on the way
constexpr int BONUS_CHANCE = 50;            // % of the walks with a golden BONUS target
constexpr uint64_t BONUS_FRAMES = 200;      // how long it stays up (3.5 s)
constexpr float BONUS_HIT = 30;             // how near a shot must land (game screen pixels)
constexpr int MAX_LIVES = 9;                // the hearts count shows one digit: past it, points
constexpr int GOLD_POINTS = 5000;
constexpr uint64_t DYING_FRAMES = 80;       // the killing attack and its flash play out (1.4 s), controls locked
constexpr uint32_t COURSE_SELECT = 0x1C2AD8;   // 0 attract, 1-2 choosing a course, 0xC playing
constexpr uint64_t LOADING_GIVE_UP = 40000;    // game frames (at full speed, ~50 s; the slowest arena takes ~9000)
constexpr const char* RECORDS = "save/survival.ini";

// Testing aids from the environment, read once
struct TestEnv {
    bool autostart = getenv("LW_SURV_AUTOSTART");   // no window to shoot at: start and play again by itself
    bool bonus = getenv("LW_SURV_BONUS");           // a BONUS target every walk, shot by itself
    bool ambush = getenv("LW_SURV_AMBUSH");         // an ambush every walk
    bool no_cache = getenv("LW_SURV_NO_CACHE");     // never load a kept arena
    const char* life = getenv("LW_SURV_LIFE");      // LIFE setting forced (with --trace-survival)
    bool god = getenv("LW_SURV_GOD");
    bool autoplay = getenv("LW_SURV_AUTOPLAY");
    const char* ram = getenv("LW_SURV_RAM");
};
const TestEnv& test_env() {
    static const TestEnv e;
    return e;
}

bool g_trace = false;
FILE* g_log = nullptr;
std::set<uint32_t> g_hooks;
uint64_t g_frame_no = 0;

float rdf(uint32_t a) { return float(f2d(rd32(a))); }


void hook(uint32_t fn) {
    g_hooks.insert(fn);
    rt_hook_function(fn);
}

void log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
}

// Live tasks: node -> function
std::map<uint32_t, uint32_t> tasks() {
    std::map<uint32_t, uint32_t> m;
    for (uint32_t t = rd32(TASK_HEAD); t && t < RAM_SIZE && m.size() < 4096; t = rd32(t + 8)) m[t] = rd32(t + 0xC);
    return m;
}

}  // namespace

// Arenas: the first encounter with known enemies of each zone (found with
// LW_SURV_AUTOPLAY and the trace, docs/game_ram.md). Stages 2 and 5 do not use
// the encounter machine; stage 1 zone 7 is the T-Rex chase, where the game
// loops the camera and loads the zone again and again
const SurvivalZone SURVIVAL_ZONES[] = {
    {1, 1, L"Jungle shack", false, 3},
    {1, 5, L"Dark forest", false, 8},
    {3, 0, L"Visitor center hall", false, 19},
    {3, 1, L"Service corridor", false, 30},
    {4, 1, L"Control room", true, 43},
};
const int SURVIVAL_ZONE_COUNT = int(sizeof(SURVIVAL_ZONES) / sizeof(SURVIVAL_ZONES[0]));

namespace {
// The state kept of an arena
std::string arena_state_path(int stage, int zone) {
    return "save/survival/" + std::to_string(stage) + "_" + std::to_string(zone) + ".state";
}

// Unlocked arenas (save/survival/unlocked.ini: "stage:zone lives life", the
// lives a new game started with and the LIFE setting then: when the kept
// state is loaded the player gets those, more or fewer as the setting is
// now). Read by the window, written by the game thread
struct Unlock {
    int lives = 0, life = -1;
};
std::mutex g_unlock_mutex;
std::map<std::pair<int, int>, Unlock> g_unlocks;
bool g_unlocks_read = false;
std::vector<int> g_new_unlocks;
constexpr const char* UNLOCKS = "save/survival/unlocked.ini";

void read_unlocks() {   // (mutex held)
    if (g_unlocks_read) return;
    g_unlocks_read = true;
    if (FILE* f = fopen(UNLOCKS, "r")) {
        char line[64];
        int st, z;
        Unlock u;
        while (fgets(line, sizeof(line), f)) {
            u.life = -1;
            if (sscanf(line, "%d:%d %d %d", &st, &z, &u.lives, &u.life) >= 3) g_unlocks[{st, z}] = u;
        }
        fclose(f);
    }
}

void write_unlocks() {   // (mutex held)
    CreateDirectoryA("save", nullptr);
    CreateDirectoryA("save/survival", nullptr);
    if (FILE* f = fopen(UNLOCKS, "w")) {
        for (auto& [k, v] : g_unlocks) fprintf(f, "%d:%d %d %d\n", k.first, k.second, v.lives, v.life);
        fclose(f);
    }
}

// The lives to start the kept state with, at LIFE setting `life` (0: unknown)
int unlock_lives(int stage, int zone, int life) {
    std::lock_guard<std::mutex> lock(g_unlock_mutex);
    read_unlocks();
    auto it = g_unlocks.find({stage, zone});
    if (it == g_unlocks.end() || it->second.lives <= 0) return 0;
    int lives = it->second.lives + (it->second.life >= 0 ? life - it->second.life : 0);
    return std::clamp(lives, 1, 9);
}

// True if it was locked
bool unlock(int stage, int zone, int lives, bool notice) {
    std::lock_guard<std::mutex> lock(g_unlock_mutex);
    read_unlocks();
    auto it = g_unlocks.find({stage, zone});
    if (it != g_unlocks.end() && it->second.lives > 0) return false;
    g_unlocks[{stage, zone}] = {lives, int(rd8(SETTING_LIFE))};
    write_unlocks();
    for (int i = 0; i < SURVIVAL_ZONE_COUNT; i++)
        if (notice && SURVIVAL_ZONES[i].stage == stage && SURVIVAL_ZONES[i].zone == zone) g_new_unlocks.push_back(i);
    printf("[survival] arena %d:%d unlocked\n", stage, zone);
    return true;
}
}  // namespace

namespace {
// Its kept state is gone and the way there did not reach it: to be reached
// in the arcade game again
void relock(int stage, int zone) {
    std::lock_guard<std::mutex> lock(g_unlock_mutex);
    read_unlocks();
    if (g_unlocks.erase({stage, zone})) write_unlocks();
}
}  // namespace

bool survival_unlocked(int stage, int zone) {
    int w, sc;
    if (survival_record(stage, zone, w, sc)) return true;
    std::lock_guard<std::mutex> lock(g_unlock_mutex);
    read_unlocks();
    return g_unlocks.count({stage, zone}) != 0;
}

std::vector<int> survival_take_unlocked() {
    std::lock_guard<std::mutex> lock(g_unlock_mutex);
    std::vector<int> v;
    v.swap(g_new_unlocks);
    return v;
}

const wchar_t* survival_zone_name(int stage, int zone) {
    for (const SurvivalZone& z : SURVIVAL_ZONES)
        if (z.stage == stage && z.zone == zone) return z.name;
    return nullptr;
}

namespace {

uint32_t script_addr(uint32_t pc) { return script::BASE + 4 * pc; }
uint32_t script_pc(uint32_t addr) { return (addr - script::BASE) / 4; }
int32_t ram_word(uint32_t pc) { return int32_t(rd32(script_addr(pc))); }

// The words of a call of encounter `id` (delay, function, nargs, args), as survival runs it
std::vector<int32_t> as_run(const script::Item& it, int id) {
    std::vector<int32_t> item = it.w;
    if (id == TRICERATOPS_ENCOUNTER && it.fn() == FN_RAPTOR && item.size() >= 6 && item[5] == TRICERATOPS_GROUP)
        item[5] = GROUND_GROUP;
    return item;
}

// Calls of known enemies in encounter `id` (for the trace: which zones can be arenas; -1 unreadable)
int encounter_enemies(int id) {
    uint32_t h = script::find(ram_word, id);
    if (h == script::NOT_FOUND) return 0;
    int n = 0;
    if (!script::walk(ram_word, h, [&](const script::Item& it) { n += it.enemy(); })) return -1;
    return n;
}

// Best wave per stage and zone, with its score and lap (save/survival.ini:
// "stage:zone wave score lap"; older files have no lap)
struct Best {
    int wave = 0, score = 0, lap = 0;
};
// Read from the file once and kept in memory: the menu asks for every arena
// each time it draws. The game thread writes a new record, the window reads
std::mutex g_records_mutex;
std::map<std::pair<int, int>, Best> g_records;
bool g_records_read = false;

void read_records() {   // (mutex held)
    if (g_records_read) return;
    g_records_read = true;
    if (FILE* f = fopen(RECORDS, "r")) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            int s, z;
            Best b;
            if (sscanf(line, "%d:%d %d %d %d", &s, &z, &b.wave, &b.score, &b.lap) >= 4) g_records[{s, z}] = b;
        }
        fclose(f);
    }
}

Best record_of(int stage, int zone) {
    std::lock_guard<std::mutex> lock(g_records_mutex);
    read_records();
    auto it = g_records.find({stage, zone});
    return it == g_records.end() ? Best{} : it->second;
}

void keep_record(int stage, int zone, const Best& b) {
    std::lock_guard<std::mutex> lock(g_records_mutex);
    read_records();
    g_records[{stage, zone}] = b;
    if (FILE* f = fopen(RECORDS, "w")) {
        for (auto& [k, v] : g_records) fprintf(f, "%d:%d %d %d %d\n", k.first, k.second, v.wave, v.score, v.lap);
        fclose(f);
    }
}

// One run of the mode, from the menu's choice to its results: reset() starts
// each one from these values
struct Run {
    uint64_t loading_frames = 0;
    SurvivalStep step = SurvivalStep::Preparing;
    int stage = 0, zone = 0;      // 0-based stage byte, zone nibble
    enum { WAITING, STARTING, HOLDING } state = WAITING;
    bool zone_empty = false;      // its encounters had no enemies (stage 1 zone 0, the walk in): the next zone's will do
    SurvivalMode mode = SurvivalMode::Off;
    int encounter = -1;
    std::vector<std::vector<int32_t>> enemies;   // {delay, fn, nargs, args...} of the encounter
    std::vector<std::vector<int32_t>> looks;     // {-9, x, y, z}
    std::vector<int32_t> setup;                  // first -25 / -26 of the encounter
    uint32_t end_pc = 0;          // the endless wait the machine reaches after a wave
    std::vector<uint32_t> enemy_pcs;   // where the script being run calls an enemy
    int wave = 0;                 // 0: the encounter itself
    bool armed = false;           // a new course: its first stage becomes the chosen one
    int lives = 0, start_lives = 0;
    // The lap: fight at a stop, a pause once it is cleared, the walk on to the
    // next one (maybe an ambush on the way), and after the last stop the fade
    // back to the first
    enum Phase { FIGHT, CLEARED, WALK, AMBUSH, LAP_OUT, LAP_LOAD } phase = FIGHT;
    uint64_t phase_at = 0;        // frame the phase began
    int lap = 1;
    int stop = 0;                 // stops reached in this lap (0: the first)
    bool last_stop = false;       // no stop of the zone left after this one
    int pending = -1;             // encounter whose trigger the walk went past (the next stop's)
    bool stop_passed = false;     // the next stop's trigger went off (during an ambush the walker was stopped already)
    bool ambush_planned = false;  // this walk: an ambush halfway to the next stop
    float walk_length = 0;        // how far the next stop was (straight) when the walk began
    uint64_t bonus_at = 0;        // this walk: when the BONUS target shows up (0: none)
    int first_encounter = -1;     // the arena's first stop, where each lap starts again
    size_t first_count = 0;       // dinosaurs of its encounter (the first wave, the game's own)
    std::vector<int> zone_encounters;   // the encounters with enemies of the zone (its stops)
    std::vector<int32_t> raptor_types;  // the raptor types they use
    uint32_t last_sig = 0;        // the wave before (the same one never comes twice in a row)
    std::vector<std::vector<int32_t>> first_enemies, first_looks;
    std::vector<int32_t> first_setup;
    // What the arena's encounter does before its first camera turn and enemy
    // (doors, lights: in the Service corridor fn_0003B46C opens the room the
    // raptors come from). The state a lap goes back to was kept before that
    // ran, so the first wave of each new lap does it again
    std::vector<int32_t> first_prelude;
    bool prelude_due = false;
    int saved_lives = 0, saved_score = 0;   // kept over the state put back at a new lap
    uint64_t fade_in_at = 0;      // the picture comes back from black (after a new lap)
    double lap_started = 0;       // banner
    double ambush_started = 0;
    // The golden BONUS target (fractions of the 4:3 screen)
    bool bonus_on = false;
    uint64_t bonus_until = 0;
    float bonus_bx = 0, bonus_by = 0, bonus_x = 0, bonus_y = 0;
    bool trigger_was[2] = {};
    int reward = 0;
    double reward_at = 0;

    bool last_hit = false;        // a hit function ran with one life left
    uint64_t dying_until = 0;     // the last attack plays out until this frame, then the results
    Best best_before;
    bool new_record = false;
    double wave_started = 0;
    double results_at = 0;
    uint64_t travel_frames = 0;   // on the way to the arena (the loading bar creeps on)
    uint32_t loading_lives = 0;   // the player's lives as the stage began (kept on the way in)
    bool shoot_in = false;        // SurvivalZone::shoot_in of this arena
    // The arena kept the first time it was reached (save/survival/), made
    // with this LIFE and difficulty setting: the way there only once
    std::string cache;            // its file: being loaded or saved this run
    bool cache_loading = false, cache_saving = false, cache_tried = false;
    uint8_t keep_life = 0, keep_diff = 0;   // the settings, kept over the state loaded
};

struct Survival : Run {
    // Kept from run to run
    bool on = false;
    bool gate = false;            // the window is still putting the game back at the attract
    int base_diff = 7;            // the settings' difficulty, put back when a run ends
    bool confirm = false;         // set by the window: start, or play again
    uint32_t rng = 12345;
    std::mutex view_mutex;        // the window reads mode, wave, score...
    SurvivalView view;

    int alive() const { return std::max(int(rd32(VM_ALIVE)), int(rd32(VM_ALIVE2))); }

    uint32_t rand(uint32_t n) { return script::rand_below(rng, n); }

    // Copies the encounter starting at header pc h to free RAM, ending in an
    // endless wait, and gathers its enemies and camera turns
    bool take_over(uint32_t h) {
        std::vector<int32_t> out;
        std::vector<uint32_t> calls;
        bool counted = false;   // the countdown is in: after the first camera turn, before the first enemy
        // What the encounter waits before its first enemy (-21, and that call's own
        // delay) is part of the countdown: it ends when the first one can come
        int32_t waited = 0;
        size_t count_at = 0;    // the countdown's word in out
        bool first_enemy = true;
        int32_t bad = 0;
        std::vector<int32_t> prelude;
        bool in_prelude = true;
        bool ok = script::walk(ram_word, h, [&](const script::Item& it) {
            if (it.end()) return;
            if (in_prelude && (it.op() == -9 || it.enemy())) in_prelude = false;
            if (in_prelude && it.op() != -10 && it.op() != -21 && it.fn() != FN_DEAD_TRICERATOPS) {
                std::vector<int32_t> item = it.w;
                if (it.call()) item[0] = 0;   // all at once, before the countdown
                prelude.insert(prelude.end(), item.begin(), item.end());
            }
            if (!it.call()) {
                const std::vector<int32_t>& item = it.w;
                int32_t op = it.op();
                if (op == -9) looks.push_back(item);
                if ((op == -25 || op == -26) && setup.size() < 7) setup.insert(setup.end(), item.begin(), item.end());
                out.insert(out.end(), item.begin(), item.end());
                if (op == -21 && first_enemy) {
                    if (counted) out[count_at] = std::max(0, out[count_at] - item[1]);
                    else waited += item[1];
                }
                if (op == -9 && !counted) {
                    out.push_back(-21), out.push_back(std::max(0, COUNTDOWN_FRAMES - waited));
                    count_at = out.size() - 1, counted = true;
                }
                return;
            }
            std::vector<int32_t> item = as_run(it, encounter);
            if (it.fn() == FN_DEAD_TRICERATOPS) {   // its delay is kept as a wait
                if (item[0] > 0) out.push_back(-21), out.push_back(item[0]);
                return;
            }
            if (it.enemy()) {
                if (!counted) {
                    out.push_back(-21), out.push_back(std::max(0, COUNTDOWN_FRAMES - waited));
                    count_at = out.size() - 1, counted = true;
                }
                if (first_enemy) out[count_at] = std::max(0, out[count_at] - item[0]), first_enemy = false;
                enemies.push_back(item);
                calls.push_back(uint32_t(out.size()));
            }
            out.insert(out.end(), item.begin(), item.end());
        }, &bad);
        if (!ok) {
            printf("[survival] unknown operation %d in encounter %d\n", bad, encounter);
            return false;
        }
        if (enemies.empty()) {
            printf("[survival] encounter %d has no known enemies\n", encounter);
            return false;
        }
        first_prelude = prelude;
        uint32_t base = FREE_RAM;
        write_script(base, out, calls);
        wr32(VM_PC, script_pc(base));
        printf("[survival] encounter %d: %zu enemy calls, %zu camera turns\n", encounter,
               enemies.size(), looks.size());
        return true;
    }

    // An encounter is a row of sections: a camera turn (-9) to where a group of
    // spawn positions is, maybe its setup (-25 / -26), its enemy calls (the
    // raptor's third argument is that group) and a wait for all of them (-1)
    struct Section {
        std::vector<int32_t> look, setup;
        std::vector<std::vector<int32_t>> calls;
    };
    // What survival takes from encounter `id`: its enemy calls, camera turns,
    // first setup and sections (complete: it was read to its end)
    struct Encounter {
        std::vector<std::vector<int32_t>> enemies, looks;
        std::vector<int32_t> setup;
        std::vector<Section> sections;
        bool complete = false;
    };
    Encounter encounter_of(int id) const {
        Encounter enc;
        uint32_t h = script::find(ram_word, id);
        if (h == script::NOT_FOUND) return enc;
        Section cur;
        auto close = [&] {
            if (!cur.calls.empty()) enc.sections.push_back(cur);
            cur.calls.clear();
            cur.setup.clear();
        };
        enc.complete = script::walk(ram_word, h, [&](const script::Item& it) {
            if (it.call()) {
                if (!it.enemy()) return;
                std::vector<int32_t> item = as_run(it, id);
                enc.enemies.push_back(item);
                cur.calls.push_back(item);
                return;
            }
            const std::vector<int32_t>& item = it.w;
            int32_t op = it.op();
            if (op == -9) enc.looks.push_back(item), close(), cur.look = item;
            if (op == -25 || op == -26) {
                if (enc.setup.size() < 7) enc.setup.insert(enc.setup.end(), item.begin(), item.end());
                cur.setup.insert(cur.setup.end(), item.begin(), item.end());
            }
            if (op == -1) close();
        });
        close();
        return enc;
    }

    // What a wave at this stop can be made of: the sections of its encounter,
    // and those of the zone's other encounters that turn the camera near
    // where one of this stop's turns does (the same place, seen from here)
    std::vector<Section> wave_pool() const {
        std::vector<Section> pool = encounter_of(encounter).sections;
        std::vector<std::vector<int32_t>> own;
        for (const Section& sc : pool)
            if (!sc.look.empty()) own.push_back(sc.look);
        for (int id : zone_encounters) {
            if (id == encounter) continue;
            for (const Section& sc : encounter_of(id).sections) {
                if (sc.look.empty()) continue;
                bool close = false;
                for (auto& l : own)
                    close |= std::hypot(float(l[1] - sc.look[1]), float(l[3] - sc.look[3])) < NEAR_LOOK;
                if (close) pool.push_back(sc);
            }
        }
        if (pool.empty() && !enemies.empty()) {   // (no sections found: everything as one)
            Section sc;
            sc.calls = enemies;
            sc.setup = setup;
            if (!looks.empty()) sc.look = looks[0];
            pool.push_back(sc);
        }
        return pool;
    }

    // Enemy calls of one batch from a section: the raptors in their own slots
    // (one alive per slot), sometimes another raptor type of the zone's
    // (wait: frames between the camera turn and the first of them; -1 a short random one)
    std::vector<int32_t> batch(const Section& sc, int k, int& slot, uint32_t& sig, int32_t wait = -1) {
        std::vector<int32_t> w;
        if (!sc.look.empty())   // the camera turns there first
            w.insert(w.end(), sc.look.begin(), sc.look.end());
        if (!sc.look.empty() || wait >= 0) w.push_back(-21), w.push_back(wait >= 0 ? wait : int32_t(20 + rand(30)));
        w.insert(w.end(), sc.setup.begin(), sc.setup.end());
        std::set<uint32_t> others;   // other enemy kinds: each at most once in a batch (their own slots)
        for (int j = 0; j < k; j++) {
            auto e = sc.calls[rand(uint32_t(sc.calls.size()))];
            if (uint32_t(e[1]) != FN_RAPTOR && !others.insert(uint32_t(e[1])).second) continue;
            e[0] = int32_t(j == 0 ? rand(30) : 10 + rand(50));
            if (uint32_t(e[1]) == FN_RAPTOR && e.size() >= 6) {
                e[3] = slot, slot = slot % 5 + 1;
                if (!raptor_types.empty() && rand(100) < 40) e[4] = raptor_types[rand(uint32_t(raptor_types.size()))];
            }
            sig = sig * 31u + uint32_t(e[1]) * 7u + uint32_t(e.size() >= 6 ? e[4] * 13 + e[5] : 0);
            batch_calls.push_back(uint32_t(w.size()));
            w.insert(w.end(), e.begin(), e.end());
        }
        return w;
    }
    std::vector<uint32_t> batch_calls;   // where batch() put the enemy calls (offsets in its words)

    // The waves come from encounter `id` from now on
    bool adopt(int id) {
        Encounter e = encounter_of(id);
        if (!e.complete || e.enemies.empty()) return false;
        enemies = e.enemies, looks = e.looks, setup = e.setup;
        encounter = id;
        log("  waves from encounter %d\n", id);
        return true;
    }

    // Nodes of the zone trigger task (fn_00014764) that are still to come; the
    // list has the newest first, so the last ones are the nearest along the path
    std::vector<uint32_t> triggers_ahead() const {
        std::vector<uint32_t> nodes;
        uint32_t task = rd32(TRIGGER_TASK);
        if (!task || task >= RAM_SIZE || rd32(task + 0xC) != 0x14764) return nodes;
        for (uint32_t node = rd32(task); node && node < RAM_SIZE && nodes.size() < 256; node = rd32(node))
            nodes.push_back(node);
        return nodes;
    }

    // The next stop along the path (x, z), or false when none is left
    bool next_stop(float& x, float& z) const {
        auto nodes = triggers_ahead();
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
            if (rd32(*it + 4) == FN_STOP) {
                x = rdf(*it + 8), z = rdf(*it + 0x10);
                return true;
            }
        return false;
    }

    float to_next_stop() const {
        float x, z;
        if (!next_stop(x, z)) return 0;
        return std::hypot(x - rdf(CAMERA_POS), z - rdf(CAMERA_POS + 8));
    }

    // The encounter of the next stop: the nearest start trigger along the path
    // with enemies. None left: this is the zone's last stop (the stops after
    // it, if any, only lead to the next zone)
    int next_encounter() const {
        auto nodes = triggers_ahead();
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
            if (rd32(*it + 4) == FN_ENCOUNTER_START && encounter_enemies(int(rd32(*it + 0x18))) > 0)
                return int(rd32(*it + 0x18));
        return -1;
    }

    // The zone's encounters with enemies (this one and those of the stops ahead)
    // and the raptor types they use
    void find_zone_encounters() {
        zone_encounters = {encounter};
        for (uint32_t node : triggers_ahead()) {
            int id = int(rd32(node + 0x18));
            if (rd32(node + 4) == FN_ENCOUNTER_START && encounter_enemies(id) > 0 &&
                std::find(zone_encounters.begin(), zone_encounters.end(), id) == zone_encounters.end())
                zone_encounters.push_back(id);
        }
        std::set<int32_t> types;
        for (int id : zone_encounters)
            for (const Section& sc : encounter_of(id).sections)
                for (auto& e : sc.calls)
                    if (uint32_t(e[1]) == FN_RAPTOR && e.size() >= 6) types.insert(e[4]);
        raptor_types.assign(types.begin(), types.end());
        log("  zone: %zu encounters, %zu raptor types\n", zone_encounters.size(), raptor_types.size());
    }

    void set_phase(Phase p) {
        phase = p;
        phase_at = g_frame_no;
    }

    // The game's walker goes on along the path, as at the end of an encounter
    // (op -999: fn_0001A4DC(0x22, 0xFF, 0) and 0x1A3960 = 1); the camera stops
    // turning to the last point an encounter showed and looks ahead again
    void walk_on() {
        wr32(CAM_GAIT_RAMP, 0x22), wr32(CAM_GAIT_SPEED, 0xFF), wr32(CAM_GAIT_HALT, 0);
        wr32(VM_FLAGS, rd32(VM_FLAGS) & ~8u);
        wr32(CAM_WALK, 1);
    }

    void halt() {
        wr32(CAM_WALK, 0);
        wr32(CAM_GAIT_SPEED, 0), wr32(CAM_GAIT_HALT, 0xFF), wr32(CAM_GAIT_RAMP, 0);   // brakes like a stop trigger
    }

    uint64_t clear_frames() const { return std::max<uint64_t>(60, CLEAR_FRAMES - 40 * uint64_t(lap - 1)); }

    // The stop is cleared: on to the next one
    void start_walk() {
        pending = -1;
        stop_passed = false;
        uint64_t f = g_frame_no;
        bool ambush = rand(100) < uint32_t(AMBUSH_CHANCE) || test_env().ambush;
        bool bonus = rand(100) < uint32_t(BONUS_CHANCE) || test_env().bonus;
        walk_length = to_next_stop();
        ambush_planned = ambush && walk_length > 40;   // not on a short one
        bonus_at = bonus ? f + 20 + rand(80) : 0;
        walk_on();
        set_phase(WALK);
        log("  walk on (ambush %d, bonus %d)\n", int(ambush), int(bonus));
    }

    // Dinosaurs on the way: the walker stops and a short wave comes from the
    // next stop's encounter (the one the camera is heading to)
    void start_ambush() {
        halt();
        int id = pending >= 0 ? pending : next_encounter();   // (its trigger may be behind already)
        std::vector<Section> pool = id >= 0 ? encounter_of(id).sections : std::vector<Section>{};
        if (pool.empty()) pool = wave_pool();
        int count = std::min(5, 2 + lap), slot = 1;
        uint32_t sig = 0;
        std::vector<int32_t> w = {-21, 10};
        batch_calls.clear();
        auto b = batch(pool[rand(uint32_t(pool.size()))], count, slot, sig);   // the camera turns to them
        std::vector<uint32_t> calls;
        for (uint32_t c : batch_calls) calls.push_back(uint32_t(w.size()) + c);
        w.insert(w.end(), b.begin(), b.end());
        w.push_back(-1);
        uint32_t addr = FREE_RAM + 0x30000;   // its own buffer: the machine waits in another one
        write_script(addr, w, calls);
        wr32(VM_PC, script_pc(addr));
        wr32(VM_WAIT, 0);
        wr32(VM_COUNTDOWN, 0);
        ambush_planned = false;
        ambush_started = now_seconds();
        set_phase(AMBUSH);
        printf("[survival] ambush: %d enemies from encounter %d\n", count, id);
    }

    // Walking: an ambush when planned, else on until the next stop trigger stops the walker
    void watch_walk() {
        uint64_t f = g_frame_no;
        if (ambush_planned && f > phase_at + 20 && rd32(CAM_WALK) == 1 && to_next_stop() < 0.55f * walk_length) {
            start_ambush();
            return;
        }
        bool zone_left = int(rd32(ZONE) & 15) != zone;
        if (f > phase_at + WALK_LONGEST || zone_left) {   // no stop ahead after all: the lap is over
            printf("[survival] the walk reaches no stop%s: new lap\n", zone_left ? " (another zone)" : "");
            halt();
            start_lap_out();
            return;
        }
        if (f < phase_at + 30 || rd32(CAM_WALK) != 0 || std::fabs(rdf(CAM_SPEED)) > 0.5f) return;
        arrive();
    }

    // At the next stop. Its encounter went by as a trigger (skipped: the
    // machine still runs the first one): the waves come from it now
    void arrive() {
        ambush_planned = false;
        stop++;
        last_stop = next_encounter() < 0;
        if (pending >= 0 && pending != encounter) adopt(pending);
        log("  stop %d reached (encounter %d)%s\n", stop, pending, last_stop ? ", the last one" : "");
        next_wave();
        set_phase(FIGHT);
    }

    // Every frame while the hunt is on
    void tick() {
        if (state != HOLDING) return;
        bool at_end = rd32(VM_PC) == end_pc;
        uint64_t f = g_frame_no;
        switch (phase) {
        case FIGHT:
            if (!at_end) break;
            last_stop = next_encounter() < 0;
            set_phase(CLEARED);
            log("  survival wave %d cleared%s\n", wave + 1, last_stop ? " (last stop)" : "");
            if (g_trace) {
                log("  triggers ahead:");
                for (uint32_t node : triggers_ahead())
                    log(" %X@%.0f,%.0f,%.0f", rd32(node + 4), rdf(node + 8), rdf(node + 0xC), rdf(node + 0x10));
                log("  cam %.0f,%.0f,%.0f zone %d\n", rdf(CAMERA_POS), rdf(CAMERA_POS + 4), rdf(CAMERA_POS + 8),
                    int(rd32(ZONE) & 15));
            }
            break;
        case CLEARED:
            if (f < phase_at + clear_frames()) break;
            if (last_stop) start_lap_out();
            else start_walk();
            break;
        case WALK: watch_walk(); break;
        case AMBUSH:
            if (at_end && stop_passed) {   // it was at the stop already
                arrive();
            } else if (at_end) {           // the way is clear again
                walk_on();
                set_phase(WALK);
                phase_at = f - 31;   // (a walk already under way)
            }
            break;
        case LAP_OUT:
            if (f >= phase_at + FADE_FRAMES) {   // black: the first stop comes back
                if (game_state_load_mark()) set_phase(LAP_LOAD);
                else new_lap(false);
            }
            break;
        case LAP_LOAD:
            if (int r = game_state_result()) new_lap(r > 0);
            break;
        }
        watch_bonus();
    }

    // The end of the zone: fade out, then the state kept at the first stop
    void start_lap_out() {
        saved_lives = int(rd32(PLAYER_LIFE));
        saved_score = score();
        bonus_on = false;
        bonus_at = 0;
        ambush_planned = false;
        set_phase(LAP_OUT);
        log("  lap %d over\n", lap);
    }

    // The game is back at the first stop (or, if that failed, still at the last)
    void new_lap(bool back) {
        if (back) {
            wr32(PLAYER_LIFE, uint32_t(saved_lives));
            for (uint32_t a : {SCORE[0], SCORE[2 + size_t(stage)]}) wr32(a, uint32_t(saved_score));
            wr32(SCORE[1], ~0u);   // the counter shows it
            enemies = first_enemies, looks = first_looks, setup = first_setup;
            encounter = first_encounter;
            prelude_due = true;
        } else {
            printf("[survival] could not go back to the first stop: the lap goes on here\n");
        }
        lives = saved_lives;
        lap++;
        stop = 0;
        last_stop = false;
        next_wave();
        set_phase(FIGHT);
        fade_in_at = g_frame_no;
        lap_started = now_seconds();
        printf("[survival] lap %d\n", lap);
    }

    void give_reward() {
        int lives_now = int(rd32(PLAYER_LIFE));
        if (lives_now < MAX_LIVES) {
            wr32(PLAYER_LIFE, uint32_t(lives_now + 1));
            reward = 1;
        } else {
            for (uint32_t a : {SCORE[0], SCORE[2 + size_t(stage)]}) wr32(a, rd32(a) + GOLD_POINTS);
            wr32(SCORE[1], ~0u);
            reward = 2;
        }
        reward_at = now_seconds();
        log("  gold reward %d\n", reward);
    }

    // The golden target: up for a while during a walk, drifting a little; a
    // shot on it (either player's gun) gives a life, or points with the hearts full
    void watch_bonus() {
        uint64_t f = g_frame_no;
        if (bonus_at && f >= bonus_at && phase == WALK) {
            bonus_at = 0;
            bonus_on = true;
            bonus_until = f + BONUS_FRAMES;
            bonus_bx = 0.22f + float(rand(560)) / 1000.0f;
            bonus_by = 0.22f + float(rand(300)) / 1000.0f;
            log("  bonus up\n");
        }
        if (!bonus_on) {
            for (int p = 0; p < 2; p++) trigger_was[p] = g_inputs.p[p].trigger;
            return;
        }
        float t = float(f - (bonus_until - BONUS_FRAMES)) / 57.5f;
        bonus_x = bonus_bx + 0.06f * std::sin(t * 1.7f);
        bonus_y = bonus_by + 0.04f * std::sin(t * 2.3f);
        bool hit = false;
        for (int p = 0; p < 2; p++) {
            bool down = g_inputs.p[p].trigger;
            PlayerAim a = player_aim(p);
            if (down && !trigger_was[p] && a.inside &&
                std::hypot((a.x - bonus_x) * 496.0f, (a.y - bonus_y) * 384.0f) < BONUS_HIT)
                hit = true;
            trigger_was[p] = down;
        }
        if (test_env().bonus && f >= bonus_until - BONUS_FRAMES + 60) hit = true;   // testing aid: shot by itself
        if (hit) {
            bonus_on = false;
            give_reward();
        } else if (f >= bonus_until || phase == LAP_OUT) {
            bonus_on = false;
        }
    }

    // Writes words plus the endless wait at the end; `calls`: where in them the enemies come
    void write_script(uint32_t addr, std::vector<int32_t> words, const std::vector<uint32_t>& calls) {
        enemy_pcs.clear();
        for (uint32_t k : calls) enemy_pcs.push_back(script_pc(addr) + k);
        words.push_back(-21);
        words.push_back(WAIT_FOREVER);
        for (size_t k = 0; k < words.size(); k++) wr32(addr + 4 * uint32_t(k), uint32_t(words[k]));
        end_pc = script_pc(addr) + uint32_t(words.size()) - 2;
    }

    // A wave: the countdown (the camera already turned to where the first
    // ones come from), then batches of a few dinosaurs, each from a random section of the
    // pool (the camera turns to where they come from first), with random
    // pauses between them; the next batch sometimes comes before the last
    // one is all dead. More dinosaurs and bigger batches at every stop of the
    // lap and every lap; never the same wave twice in a row
    void next_wave() {
        wave++;
        int level = stop + 3 * (lap - 1);   // harder at every stop of the lap, and every lap
        int diff = std::min(15, base_diff + 1 + level / 2);
        wr8(SETTING_DIFF, uint8_t((rd8(SETTING_DIFF) & 0x0F) | (diff << 4)));
        int total = std::min(WAVE_MAX, std::max(WAVE_MIN, int(first_count)) + 2 * level);
        std::vector<Section> pool = wave_pool();
        std::vector<int32_t> w;
        std::vector<uint32_t> calls;
        int batches = 0;
        // Back at the first stop: what its encounter did before its enemies, again
        std::vector<int32_t> pre = prelude_due ? first_prelude : std::vector<int32_t>{};
        prelude_due = false;
        for (int attempt = 0; attempt < 6 && !pool.empty(); attempt++) {
            w = pre;
            calls.clear();
            batches = 0;
            uint32_t sig = 0;
            int slot = 1 + int(rand(5)), left = total, prev = -1;
            while (left > 0) {
                int k = std::min(left, std::clamp(2 + level / 3 + int(rand(2)), 2, 5));
                int pick = int(rand(uint32_t(pool.size())));
                if (pick == prev && pool.size() > 1) pick = (pick + 1 + int(rand(uint32_t(pool.size() - 1)))) % int(pool.size());
                prev = pick;
                sig = sig * 131u + uint32_t(pick) * 17u + uint32_t(k);
                batch_calls.clear();
                // The first batch: the camera turns to it, then the countdown
                auto b = batch(pool[size_t(pick)], k, slot, sig, batches == 0 ? COUNTDOWN_FRAMES : -1);
                for (uint32_t c : batch_calls) calls.push_back(uint32_t(w.size()) + c);
                w.insert(w.end(), b.begin(), b.end());
                left -= int(batch_calls.size()) > 0 ? int(batch_calls.size()) : k;
                batches++;
                if (left <= 0) break;
                // Between batches: all dead first, or (more so later on) only one left
                if (int(rand(100)) < std::min(70, 25 + 5 * level)) w.push_back(-27), w.push_back(1);
                else w.push_back(-1);
                w.push_back(-21), w.push_back(int32_t(10 + rand(70)));
            }
            if (sig != last_sig) {
                last_sig = sig;
                break;
            }
        }
        w.push_back(-1);
        // Two buffers: the new wave never overwrites the words being run
        uint32_t addr = FREE_RAM + 0x10000 * (1 + (wave & 1));
        write_script(addr, w, calls);
        wr32(VM_PC, script_pc(addr));
        wr32(VM_WAIT, 0);
        wr32(VM_COUNTDOWN, 0);   // the endless wait of the break was counting down
        wave_started = now_seconds();
        printf("[survival] wave %d: %zu enemies in %d batches (from %zu sections), difficulty %d\n", wave + 1,
               calls.size(), batches, pool.size(), diff);
        log("  survival wave %d count %zu batches %d pool %zu diff %d\n", wave + 1, calls.size(), batches, pool.size(),
            diff);
    }

    int score() const { return int(rd32(SCORE[0])); }

    void set_diff(int d) { wr8(SETTING_DIFF, uint8_t((rd8(SETTING_DIFF) & 0x0F) | (d << 4))); }
    void restore() { set_diff(base_diff); }   // the difficulty as the settings have it

    // The hunt is over: keep the record and show the results
    void finish() {
        Best best = record_of(stage + 1, zone);
        best_before = best;
        new_record = wave + 1 > best.wave || (wave + 1 == best.wave && score() > best.score);
        if (new_record) keep_record(stage + 1, zone, {wave + 1, score(), lap});
        printf("[survival] end: lap %d, wave %d, %d points%s\n", lap, wave + 1, score(),
               new_record ? " (record)" : "");
        mode = SurvivalMode::Results;
        results_at = now_seconds();
    }

    // Play again (testing aid: LW_SURV_AUTOSTART): back to the first stop, as a
    // new lap with lives, score and waves as at the start
    void restart() {
        set_diff(base_diff);
        wave = -1;
        lap = 0;
        saved_lives = start_lives;
        saved_score = 0;
        best_before = record_of(stage + 1, zone);
        new_record = false;
        mode = SurvivalMode::Playing;
        if (game_state_load_mark()) set_phase(LAP_LOAD);
        else new_lap(false);
    }

    // After a hit with one life left (survival_enter gave one more): did it take it?
    void watch_lives() {
        lives = int(rd32(PLAYER_LIFE));
        if (!last_hit) return;
        last_hit = false;
        if (lives <= 1) {   // it did: the hunt is over, once the attack has been seen
            lives = 0;
            if (!dying_until) dying_until = g_frame_no + DYING_FRAMES, log("  last hit\n");
        } else {            // that call did not hurt
            wr32(PLAYER_LIFE, uint32_t(--lives));
        }
    }

    // Buttons pressed on the way in, while the loading screen is up: a credit
    // and start (again until the course select opens), then a shot on it
    // (any course: the stage is set)
    void press_buttons() {
        uint64_t f = loading_frames;
        uint32_t select = rd32(COURSE_SELECT);
        bool boot = g_frame_no < 900;   // a button held during the boot changes what it does
        auto_coin_start(g_inputs.p[0], f, !boot && select == 0);
        g_inputs.p[0].trigger = !boot && (select == 1 || select == 2) && f % 90 < 6;
        g_inputs.p[0].offscreen = false;
        g_inputs.p[0].gun_x = 128, g_inputs.p[0].gun_y = 128;
        if (select == 0xC && shoot_in) {   // in the stage: shoots its way to the arena, lives kept
            auto_sweep_fire(g_inputs.p[0], f);
            if (!loading_lives) loading_lives = rd32(PLAYER_LIFE);
            if (loading_lives) wr32(PLAYER_LIFE, loading_lives);
        }
        step = boot || state == STARTING ? step
             : select == 0 ? SurvivalStep::Coin
             : select != 0xC ? SurvivalStep::Course
                             : SurvivalStep::Travelling;
    }

    float progress() const {
        switch (step) {
        case SurvivalStep::Preparing: return 0.03f;
        case SurvivalStep::Coin: return 0.12f;
        case SurvivalStep::Course: return 0.25f;
        case SurvivalStep::Travelling:   // creeps on: the trip has no known length
            return 0.35f + 0.6f * (1 - std::exp(-float(travel_frames) / 1500.0f));
        default: return 1;
        }
    }

    // The arena's first encounter is ours (take_over): the start screen
    void at_arena() {
        base_diff = rd8(SETTING_DIFF) >> 4;
        state = HOLDING;
        wave = 0;
        first_encounter = encounter;   // each lap starts again here
        first_enemies = enemies, first_looks = looks, first_setup = setup;
        first_count = enemies.size();
        find_zone_encounters();
        start_lives = lives = std::max(1, int(rd32(PLAYER_LIFE)));
        wave_started = now_seconds();
        if (mode == SurvivalMode::Loading) mode = SurvivalMode::Ready, step = SurvivalStep::Arrived;
        unlock(stage + 1, zone, start_lives, false);
    }

    // The arena kept the first time it was reached (Run::cache)
    std::string cache_path() const { return arena_state_path(stage + 1, zone); }
    // Loaded: the arena's first encounter (where survival or the arcade game
    // kept the state) is taken over from its start, with the lives of a new game
    bool resume() {
        int id = -1;
        for (const SurvivalZone& sz : SURVIVAL_ZONES)
            if (sz.stage == stage + 1 && sz.zone == zone) id = sz.encounter;
        if (id < 0) return false;
        uint32_t h = script::find(ram_word, id);   // its header in the script
        if (h == script::NOT_FOUND) return false;
        if (rd8(STAGE) != stage || int(rd32(ZONE) & 15) != zone) return false;
        encounter = id;
        enemies.clear(), looks.clear(), setup.clear();
        if (!take_over(h)) return false;
        if (int l = unlock_lives(stage + 1, zone, rd8(SETTING_LIFE)); l > 0) wr32(PLAYER_LIFE, uint32_t(l));
        at_arena();
        return true;
    }

    // A new run: everything as at the start
    void reset(int s, int z) {
        static_cast<Run&>(*this) = Run{};
        stage = s - 1;
        zone = z;
        mode = SurvivalMode::Loading;
        confirm = false;
        for (const SurvivalZone& sz : SURVIVAL_ZONES)
            if (sz.stage == s && sz.zone == z) shoot_in = sz.shoot_in;
        best_before = record_of(s, z);
    }

    void publish() {
        std::lock_guard<std::mutex> lock(view_mutex);
        view.mode = mode;
        view.stage = stage + 1;
        view.zone = zone;
        view.wave = wave + 1;
        view.score = mode == SurvivalMode::Loading ? 0 : score();
        view.lives = lives;
        view.best_wave = best_before.wave;
        view.best_score = best_before.score;
        view.new_record = new_record;
        view.wave_started = wave_started;
        view.results_at = results_at;
        view.lap = lap;
        view.best_lap = best_before.lap;
        view.lap_started = lap_started;
        view.ambush_started = ambush_started;
        bool playing = mode == SurvivalMode::Playing;
        view.walking = playing && phase == WALK;
        view.ambush = playing && phase == AMBUSH;
        view.zone_done = playing && ((phase == CLEARED && last_stop) || phase == LAP_OUT || phase == LAP_LOAD);
        view.resting = playing && (phase == CLEARED || phase == WALK || phase == LAP_OUT || phase == LAP_LOAD);
        view.rest_left = phase == CLEARED ? float(phase_at + clear_frames() - std::min(g_frame_no, phase_at + clear_frames())) / 57.524f : 0;
        float fade = 0;
        if (phase == LAP_OUT) fade = float(g_frame_no - phase_at) / float(FADE_FRAMES);
        else if (phase == LAP_LOAD) fade = 1;
        else if (fade_in_at && g_frame_no < fade_in_at + FADE_FRAMES) fade = 1 - float(g_frame_no - fade_in_at) / float(FADE_FRAMES);
        view.fade = playing ? std::clamp(fade, 0.0f, 1.0f) : 0;
        view.bonus_on = playing && bonus_on;
        view.bonus_x = bonus_x, view.bonus_y = bonus_y;
        view.bonus_left = bonus_on ? float(bonus_until - std::min(g_frame_no, bonus_until)) / float(BONUS_FRAMES) : 0;
        view.reward = reward;
        view.reward_at = reward_at;
        view.step = mode == SurvivalMode::Loading ? step : SurvivalStep::Arrived;
        uint32_t pc = rd32(VM_PC);
        int to_come = 0;
        for (uint32_t p : enemy_pcs) to_come += p >= pc;
        view.total = int(enemy_pcs.size());
        view.left = view.resting ? 0 : std::min(view.total, alive() + to_come);
        view.progress = mode == SurvivalMode::Loading ? progress() : 1;
    }
};

Survival g_surv;
int g_request_stage = 0, g_request_zone = 0;   // --survival
std::atomic<bool> g_begin{false}, g_ungate{false}, g_end{false};
std::atomic<int> g_begin_stage{0}, g_begin_zone{0};

}  // namespace

bool survival_option(const std::string& name, const char* value) {
    if (name == "--trace-survival") {
        g_trace = true;
        g_log = fopen(getenv("LW_SURV_TRACE") ? getenv("LW_SURV_TRACE") : "out/survival_trace.txt", "w");
        hook(FN_TASK_NEW);
        return true;
    }
    if (name == "--survival") {
        int s = 1, z = 1;
        if (!value || sscanf(value, "%d:%d", &s, &z) < 1) return false;
        g_request_stage = s, g_request_zone = z;
        return true;
    }
    return false;
}

bool survival_cli_request(int& stage, int& zone) {
    if (g_request_stage <= 0) return false;
    stage = g_request_stage, zone = g_request_zone;
    return true;
}

void survival_hooks() {
    hook(FN_ENCOUNTER);
    hook(FN_NEXT_STAGE);
    hook(FN_PLAYLIST_INIT);
    hook(FN_PLAYER_HIT[0]);
    hook(FN_PLAYER_HIT[1]);
}

bool survival_record(int stage, int zone, int& wave, int& score, int* lap) {
    Best b = record_of(stage, zone);
    if (b.wave <= 0) return false;
    wave = b.wave, score = b.score;
    if (lap) *lap = b.lap;
    return true;
}

// The window thread asks; the game thread does it at its next frame
void survival_begin(int stage, int zone) {
    g_begin_stage = stage, g_begin_zone = zone;
    g_begin = true;
    weather_roll();   // each run its own weather (weather.h)
    std::lock_guard<std::mutex> lock(g_surv.view_mutex);   // the loading screen right away
    g_surv.view = SurvivalView{};
    g_surv.view.mode = SurvivalMode::Loading;
    g_surv.view.stage = stage, g_surv.view.zone = zone;
    int w = 0, sc = 0, lp = 0;
    if (survival_record(stage, zone, w, sc, &lp))
        g_surv.view.best_wave = w, g_surv.view.best_score = sc, g_surv.view.best_lap = lp;
}

void survival_ungate() { g_ungate = true; }

void survival_end() {
    g_end = true;
    std::lock_guard<std::mutex> lock(g_surv.view_mutex);
    g_surv.view.mode = SurvivalMode::Off;
}

bool survival_enabled() { return g_surv.on || g_begin; }

SurvivalView survival_view() {
    std::lock_guard<std::mutex> lock(g_surv.view_mutex);
    return g_surv.view;
}

bool survival_owns_inputs() {
    return (g_surv.on && (g_surv.mode == SurvivalMode::Loading || g_surv.dying_until)) || g_begin;
}

void survival_confirm() { g_surv.confirm = true; }


void survival_enter(Ctx& c, uint32_t fn) {
    if (!g_hooks.count(fn)) return;
    if (g_surv.gate && fn != FN_TASK_NEW) return;
    if (fn == FN_TASK_NEW && g_trace) log("  task+ %08X size %X from %08X\n", c.r[4], c.r[3], c.lr);
    if (fn == FN_PLAYLIST_INIT && g_surv.on) g_surv.armed = true;
    if (fn == FN_NEXT_STAGE && g_surv.on && g_surv.armed) {
        // The course (Expert skips stages with more calls) lands on the chosen stage
        g_surv.armed = false;
        for (int32_t i = std::max(0, int32_t(rd32(PLAYLIST_INDEX)) + 1); i < 5; i++)
            wr32(PLAYLIST + 4 * uint32_t(i), uint32_t(g_surv.stage));
    }
    if ((fn == FN_PLAYER_HIT[0] || fn == FN_PLAYER_HIT[1]) && g_surv.on && g_surv.mode == SurvivalMode::Playing &&
        int(rd32(PLAYER_LIFE)) <= 1) {   // this hit would end the game: it takes the extra life instead
        wr32(PLAYER_LIFE, rd32(PLAYER_LIFE) + 1);
        g_surv.last_hit = true;
    }
    if (fn == FN_ENCOUNTER && g_surv.on) {
        Survival& s = g_surv;
        uint32_t pc = rd32(VM_PC);
        if (s.state == Survival::STARTING) {   // first run of the encounter: its header
            if (s.take_over(pc)) {
                s.at_arena();
                // The arena as it is now, for next time (it loads from there)
                if (s.cache.empty()) {
                    s.cache = s.cache_path();
                    CreateDirectoryA("save/survival", nullptr);
                    game_state_save(s.cache + ".tmp");
                    s.cache_saving = true;
                }
            } else {   // no enemies in this one: wait for the next encounter, of this zone or a later one
                s.enemies.clear(), s.looks.clear(), s.setup.clear();
                s.state = Survival::WAITING;
                s.zone_empty = true;
            }
        }
    }
}

extern int g_boss_maker;   // runtime.cpp: --make-boss-state plays by itself

// The arcade game reaches an arena: unlocked (with its notice), and the
// state as it is kept for survival mode
static int g_arcade_lives = 0;         // the most lives the player has had in this game (as a new game starts)
static std::string g_arcade_saving;    // the arena state being written
static void arcade_arena(int id) {
    int stage = rd8(STAGE) + 1, zone = int(rd32(ZONE) & 15);
    for (const SurvivalZone& sz : SURVIVAL_ZONES) {
        if (sz.stage != stage || sz.zone != zone || sz.encounter != id) continue;
        int lives = std::max(g_arcade_lives, int(rd32(PLAYER_LIFE)));
        unlock(stage, zone, lives, true);
        std::string path = arena_state_path(stage, zone);
        if (g_arcade_saving.empty() && !game_state_valid(path)) {
            CreateDirectoryA("save/survival", nullptr);
            game_state_save(path + ".tmp");
            g_arcade_saving = path;
        }
    }
}

bool survival_spawn_call(Ctx& c, uint32_t fn) {
    uint32_t r = c.r[3];   // node+4: {fn, x, y, z, radius, p0, p1}
    if (g_trace) {
        log("  spawn %08X pos %.1f %.1f %.1f r %.1f p %08X %08X\n", fn, rdf(r + 4), rdf(r + 8), rdf(r + 12),
            rdf(r + 16), rd32(r + 20), rd32(r + 24));
        if (fn == FN_ENCOUNTER_START)
            log("  encounter %d stage %d zone %d enemies %d\n", int(rd32(r + 20)), rd8(STAGE) + 1, int(rd32(ZONE) & 15),
                encounter_enemies(int(rd32(r + 20))));
    }
    Survival& s = g_surv;
    if (!s.on && fn == FN_ENCOUNTER_START && g_boss_maker < 0 && rd32(COURSE_SELECT) == 0xC)
        arcade_arena(int(rd32(r + 20)));
    if (s.gate) return false;
    int zone_now = int(rd32(ZONE) & 15);
    if (s.on && s.state == Survival::WAITING && fn == FN_ENCOUNTER_START && rd8(STAGE) == s.stage &&
        (zone_now == s.zone || (s.zone_empty && zone_now > s.zone))) {
        if (zone_now != s.zone) {
            printf("[survival] zone %d has no enemies: the arena is zone %d\n", s.zone, zone_now);
            s.zone = zone_now;
        }
        s.encounter = int(rd32(r + 20));
        s.state = Survival::STARTING;
        s.step = SurvivalStep::Travelling;
        printf("[survival] starts at encounter %d\n", s.encounter);
    }
    // Holding: no other encounter starts (the machine keeps running the first
    // one); the one the walk goes past gives the waves of the next stop
    if (s.on && s.state == Survival::HOLDING && fn == FN_ENCOUNTER_START) {
        if (s.phase == Survival::WALK || s.phase == Survival::AMBUSH) s.pending = int(rd32(r + 20));
        return true;
    }
    if (s.on && s.state == Survival::HOLDING && fn == FN_STOP && (s.phase == Survival::WALK || s.phase == Survival::AMBUSH))
        s.stop_passed = true;
    return false;
}

bool survival_frame(Ctx&) {
    g_frame_no++;
    Survival& s = g_surv;
    bool hold = false;
    if (g_end.exchange(false) && s.on) {
        s.restore();
        s.on = false;
        s.mode = SurvivalMode::Off;
    }
    if (g_begin.exchange(false)) {
        if (s.on) s.restore();
        s.on = true;
        s.gate = true;
        s.rng = random_seed();
        s.reset(g_begin_stage, g_begin_zone);
        printf("[survival] stage %d, zone %d\n", s.stage + 1, s.zone);
    }
    if (g_ungate.exchange(false)) s.gate = false;
    if (!s.on) {   // the arcade game
        g_arcade_lives = rd32(COURSE_SELECT) == 0xC ? std::max(g_arcade_lives, int(rd32(PLAYER_LIFE))) : 0;
        if (!g_arcade_saving.empty() && game_state_result() != 0) {
            bool ok = game_state_result() > 0 && MoveFileExA((g_arcade_saving + ".tmp").c_str(),
                                                              g_arcade_saving.c_str(), MOVEFILE_REPLACE_EXISTING);
            printf("[survival] arena %s %s\n", ok ? "kept in" : "could not be kept in", g_arcade_saving.c_str());
            g_arcade_saving.clear();
        }
    }
    if (s.on) {
        switch (s.mode) {
        case SurvivalMode::Loading:
            if (s.gate) break;
            // The arena kept from another time, once the game is in its main
            // loop (a state loads only from there: without a window, past the boot)
            if (!s.cache_tried && (game_state_has_home() || g_frame_no >= 900) && !test_env().no_cache) {
                s.cache_tried = true;
                std::string path = s.cache_path();
                s.keep_life = rd8(SETTING_LIFE), s.keep_diff = rd8(SETTING_DIFF);   // the settings as they are now
                if (game_state_valid(path) && game_state_load(path)) s.cache = path, s.cache_loading = true;
            }
            if (s.cache_loading) {
                s.loading_frames = 1;
                int r = game_state_result();
                if (r == 0) break;
                s.cache_loading = false;
                if (r > 0) wr8(SETTING_LIFE, s.keep_life), wr8(SETTING_DIFF, s.keep_diff);
                if (r > 0 && s.resume()) {
                    printf("[survival] arena loaded from %s\n", s.cache.c_str());
                    break;
                }
                printf("[survival] %s did not load: the way there instead\n", s.cache.c_str());
                if (r > 0) DeleteFileA(s.cache.c_str());   // (not when the game was just not ready for it)
                s.cache.clear();
                if (r > 0) {   // loaded but not understood: from the attract again
                    s.on = false;
                    s.mode = SurvivalMode::Off;
                    s.publish();
                    break;
                }
            }
            s.loading_frames++;
            if (s.step == SurvivalStep::Travelling) s.travel_frames++;
            s.press_buttons();
            if (s.loading_frames > LOADING_GIVE_UP) {
                printf("[survival] did not reach zone %d of stage %d\n", s.zone, s.stage + 1);
                relock(s.stage + 1, s.zone);
                s.on = false;
                s.mode = SurvivalMode::Off;
                s.publish();   // the window goes back to the menu
            }
            break;
        case SurvivalMode::Ready:   // the start screen: the game waits for the first shot
            if (s.cache_saving && game_state_result() != 0) {
                s.cache_saving = false;
                bool ok = game_state_result() > 0 &&
                          MoveFileExA((s.cache + ".tmp").c_str(), s.cache.c_str(), MOVEFILE_REPLACE_EXISTING);
                printf("[survival] arena %s %s\n", ok ? "kept in" : "could not be kept in", s.cache.c_str());
            }
            if (s.confirm || test_env().autostart) {   // testing aid: no window to shoot at
                s.confirm = false;
                s.mode = SurvivalMode::Playing;
                s.wave_started = s.lap_started = now_seconds();
                for (uint32_t a : SCORE) wr32(a, 0);
                wr32(SCORE[1], ~0u);   // the counter shows the 0 (it still showed the score of the way in)
                game_state_mark();   // the first stop: every lap starts again from here
                log("  survival wave 1 count %zu (the encounter itself)\n", s.enemies.size());
                s.set_phase(Survival::FIGHT);
            } else {
                hold = true;
            }
            s.lives = int(rd32(PLAYER_LIFE));
            break;
        case SurvivalMode::Playing:
            s.tick();
            s.watch_lives();
            if (s.dying_until) {   // the player's controls do nothing meanwhile (survival_owns_inputs)
                g_inputs.p[0].trigger = g_inputs.p[0].offscreen = false;
                if (g_frame_no >= s.dying_until) {
                    s.dying_until = 0;
                    s.finish();
                }
            }
            hold = s.mode == SurvivalMode::Results;
            break;
        case SurvivalMode::Results:
            if (s.confirm || test_env().autostart) {
                s.confirm = false;
                s.restart();
            } else {
                hold = true;
            }
            break;
        default: break;
        }
        s.publish();
    }
    if (!g_trace) return hold;
    if (const char* life = test_env().life; life && g_frame_no > 1000) wr8(SETTING_LIFE, uint8_t(atoi(life)));
    if (test_env().god) {   // testing aid: the player's life never drops
        if (s.on) s.lives = std::max(s.lives, 5);
        uint32_t life = rd32(PLAYER_LIFE);
        if (life > 0 && life < 5) wr32(PLAYER_LIFE, 5);
    }
    if (test_env().autoplay && rd32(COURSE_SELECT) == 0xC)   // testing aid: shoots all over the screen
        auto_sweep_fire(g_inputs.p[0], g_frame_no);
    static std::map<uint32_t, uint32_t> last;
    auto now = tasks();
    for (auto& [t, fn] : last)
        if (!now.count(t) || now[t] != fn) log("  task- %08X at %08X\n", fn, t);
    last = now;
    log("f %llu st %d zone %08X cam %.2f %.2f %.2f tasks %zu node %d walk %d speed %.2f\n",
        (unsigned long long)g_frame_no, rd8(STAGE), rd32(ZONE), rdf(CAMERA_POS), rdf(CAMERA_POS + 4),
        rdf(CAMERA_POS + 8), now.size(), rd32(CAM_NODE), rd32(CAM_WALK), rdf(CAM_SPEED));
    {   // encounter machine
        uint32_t pc = rd32(VM_PC);
        static uint32_t last_pc = ~0u;
        if (pc != last_pc) {
            last_pc = pc;
            log("  vm pc %u op %d flags %X alive %d/%d wait %d look %.1f %.1f %.1f\n", pc,
                int32_t(rd32(script_addr(pc))), rd32(VM_FLAGS), rd32(VM_ALIVE), rd32(VM_ALIVE2), rd32(VM_WAIT),
                rdf(LOOK_AT), rdf(LOOK_AT + 4), rdf(LOOK_AT + 8));
        }
    }
    if (g_frame_no % 60 == 0) {
        std::map<uint32_t, int> by_fn;
        for (auto& [t, fn] : now) by_fn[fn]++;
        log("  tasks:");
        for (auto& [fn, n] : by_fn) log(" %X*%d", fn, n);
        log("\n");
        fflush(g_log);
    }
    // RAM snapshots for offline searches: LW_SURV_RAM=FROM,TO,EVERY (frames)
    static unsigned long long ram_from = 0, ram_to = 0, ram_every = 0;
    static FILE* ram = nullptr;
    if (const char* e = test_env().ram; e && !ram_every) {
        if (sscanf(e, "%llu,%llu,%llu", &ram_from, &ram_to, &ram_every) != 3) ram_from = 0, ram_to = ~0ull, ram_every = 60;
        ram = fopen("out/survival_ram.bin", "wb");
    }
    if (ram && g_frame_no >= ram_from && g_frame_no <= ram_to && g_frame_no % ram_every == 0) {
        uint32_t hdr[2] = {uint32_t(g_frame_no), 0};
        fwrite(hdr, 4, 2, ram);
        fwrite(g_ram + 0x130000, 1, 0xA0000, ram);
    }
    return hold;
}
