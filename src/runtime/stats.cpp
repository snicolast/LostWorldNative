// Records and achievements (stats.h).
//
// The game keeps per player, for its stage results screen, a block of
// counters at 0x1A6208 + 0x10 * player: +4 HITS, +6 TECHNICAL SHOT (hits of a
// type other than 0: a weak spot, a part hit just so) and +0xC shots fired.
// Every scoring hit goes through fn_0003DC60 (type, player from 1, points).
// None of it counts kills: a hit that takes an enemy down lowers the count of
// enemies of the encounter machine (0x1A8468) within the same frame, so a fall
// of that count right after a hit is a kill. The encounters that do not use
// the machine (the stage 1 cabin, the bosses) count hits only.
//
// The game is read at every vblank on the game thread; what the window shows
// is a copy taken under a mutex. Debug runs (tests, --auto, the survival
// god mode, the boss state maker) never call stats_init, so they count nothing.
#include "stats.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "ppc_rt.h"
#include "survival.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

// Game RAM (docs/game_ram.md)
constexpr uint32_t STAGE = 0x1A623C;          // byte: current stage (0 = stage 1)
constexpr uint32_t COURSE = 0x1C2AE0;         // 0 Beginner, 1 Expert, 2 Shuffle
constexpr uint32_t PLAYER_STATE = 0x1A3784 + 0xD;   // player 1 slot: 1 / 2 playing, 0 continue, 3 out
constexpr uint32_t ROUTE_SELECT = 0x1C2AD8;   // 1-2 choosing, 0xC playing
constexpr uint32_t PLAYER_LIFE = 0x1A3723;    // low byte of player 1's lives (0: down)
constexpr uint32_t SCORE = 0x1A857C;          // player 1 score; +0x10 + 4 * stage: score of that stage
constexpr uint32_t COUNTERS = 0x1A6208;       // player 1 results counters (+4 hits, +6 technical, +0xC shots)
constexpr uint32_t VM_ALIVE = 0x1A8468;       // enemies alive in the running encounter
constexpr uint32_t FN_ADD_SCORE = 0x0003DC60; // (type, player from 1, points): every scoring hit
constexpr double FRAME_HZ = 57.524;
constexpr double MIN_STAGE_SECONDS = 20;    // a stage played through takes minutes

constexpr const char* STATS_FILE = "save/stats.ini";
constexpr const char* ACHIEVEMENTS_FILE = "save/achievements.ini";

int64_t frames_of(double seconds) { return int64_t(seconds * FRAME_HZ); }

// What each achievement counts, read from the totals (one function per
// achievement, so checking them at every vblank is a call, not a search)
int64_t kills(const Stats& s) { return s.kills; }
template <int N> int64_t stage_clears(const Stats& s) { return s.stage[N].clears; }
int64_t games_finished(const Stats& s) { return s.games_finished; }
template <int N> int64_t course_finished(const Stats& s) { return s.finished_course[N]; }
int64_t one_credit(const Stats& s) { return s.one_credit_finishes; }
int64_t untouched(const Stats& s) { return s.untouched_stages; }
template <int PER_MILLE> int64_t stage_accuracy(const Stats& s) {   // 1 once a stage was cleared that well
    for (const Stats::Stage& st : s.stage)
        if (st.best_accuracy >= PER_MILLE) return 1;
    return 0;
}
int64_t technical(const Stats& s) { return s.best_stage_technical; }
int64_t shots(const Stats& s) { return s.shots; }
int64_t best_score(const Stats& s) { return *std::max_element(std::begin(s.best_score), std::end(s.best_score)); }
template <int N> int64_t boss_wins(const Stats& s) { return s.boss[N].wins; }
int64_t flawless_bosses(const Stats& s) {
    int n = 0;
    for (const Stats::Boss& b : s.boss) n += b.flawless;
    return n;
}
int64_t fast_boss(const Stats& s) {   // 1 once a boss went down in under a minute
    for (const Stats::Boss& b : s.boss)
        if (b.best_frames > 0 && b.best_frames < frames_of(60)) return 1;
    return 0;
}
int64_t best_wave(const Stats& s) { return s.best_wave; }
int64_t play_frames(const Stats& s) { return s.play_frames; }

const std::vector<AchievementInfo> TABLE = {
    {"first_blood", L"FIRST BLOOD", L"Bring down your first dinosaur", 1, kills},
    {"hunter", L"HUNTER", L"Bring down 100 dinosaurs", 100, kills},
    {"big_game_hunter", L"BIG GAME HUNTER", L"Bring down 1,000 dinosaurs", 1000, kills},
    {"stage1", L"INTO THE JUNGLE", L"Clear stage 1", 1, stage_clears<0>},
    {"stage2", L"KING OF THE LAKESIDE", L"Clear stage 2", 1, stage_clears<1>},
    {"stage3", L"STAGE 3 CLEARED", L"Clear stage 3", 1, stage_clears<2>},
    {"stage4", L"STAGE 4 CLEARED", L"Clear stage 4", 1, stage_clears<3>},
    {"finish", L"THE END OF THE LOST WORLD", L"Finish the game", 1, games_finished},
    {"finish_expert", L"EXPERT TRACKER", L"Finish the game on the Expert course", 1, course_finished<1>},
    {"finish_shuffle", L"SHUFFLE SURVIVOR", L"Finish the game on the Shuffle course", 1, course_finished<2>},
    {"one_credit", L"ONE CREDIT WONDER", L"Finish the game without continuing", 1, one_credit},
    {"untouchable", L"UNTOUCHABLE", L"Clear a stage without losing a life", 1, untouched},
    {"sharpshooter", L"SHARPSHOOTER", L"Clear a stage with a hit rate of 80% or more", 1, stage_accuracy<800>},
    {"dead_eye", L"DEAD EYE", L"Clear a stage with a hit rate of 95% or more", 1, stage_accuracy<950>},
    {"technician", L"TECHNICIAN", L"15 technical shots in one stage", 15, technical},
    {"trigger_happy", L"TRIGGER HAPPY", L"Fire 10,000 shots", 10000, shots},
    {"high_roller", L"HIGH ROLLER", L"Score 100,000 points in one game", 100000, best_score},
    {"boss1", L"MOTHER'S WRATH", L"Defeat the female T-Rex in boss mode", 1, boss_wins<0>},
    {"boss2", L"CROC HUNTER", L"Defeat the Deinosuchus in boss mode", 1, boss_wins<1>},
    {"boss3", L"CARNOTAURUS DOWN", L"Defeat the Carnotaurus in boss mode", 1, boss_wins<2>},
    {"boss4", L"KING OF SAN DIEGO", L"Defeat the T-Rex in boss mode", 1, boss_wins<3>},
    {"flawless_boss", L"FLAWLESS", L"Defeat a boss without losing a life", 1, flawless_bosses},
    {"speed_hunter", L"SPEED HUNTER", L"Defeat a boss in less than 60 seconds", 1, fast_boss},
    {"wave5", L"WAVE RIDER", L"Reach wave 5 in survival", 5, best_wave},
    {"wave10", L"LAST STAND", L"Reach wave 10 in survival", 10, best_wave},
    {"wave20", L"ENDLESS", L"Reach wave 20 in survival", 20, best_wave},
    {"veteran", L"VETERAN", L"Play for 5 hours", frames_of(5 * 3600), play_frames},
};

// ---- Files: "key = value" lines ----

struct Ini {
    std::vector<std::pair<std::string, int64_t>> rows;
    void put(const std::string& k, int64_t v) { rows.push_back({k, v}); }
    bool save(const char* path) const {
        std::string tmp = std::string(path) + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) return false;
        for (auto& [k, v] : rows) fprintf(f, "%s = %lld\n", k.c_str(), (long long)v);
        bool ok = fclose(f) == 0;
#ifdef _WIN32
        return ok && MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING);
#else
        return ok && rename(tmp.c_str(), path) == 0;
#endif
    }
    static Ini load(const char* path) {
        Ini ini;
        if (FILE* f = fopen(path, "r")) {
            char line[256], key[128];
            long long v;
            while (fgets(line, sizeof(line), f))
                if (sscanf(line, " %127[^= ] = %lld", key, &v) == 2) ini.put(key, v);
            fclose(f);
        }
        return ini;
    }
};

// Every field of Stats under its name in save/stats.ini
template <class F>
void each_field(Stats& s, F&& f) {
    f("games", s.games), f("games_finished", s.games_finished), f("play_frames", s.play_frames);
    f("shots", s.shots), f("hits", s.hits), f("technical", s.technical), f("kills", s.kills);
    f("lives_lost", s.lives_lost), f("continues", s.continues);
    for (int i = 0; i < STAT_STAGES; i++) {
        std::string p = "stage" + std::to_string(i + 1) + "_";
        Stats::Stage& st = s.stage[i];
        f(p + "clears", st.clears), f(p + "best_score", st.best_score), f(p + "best_accuracy", st.best_accuracy);
        f(p + "best_frames", st.best_frames);
    }
    for (int i = 0; i < STAT_BOSSES; i++) {
        std::string p = "boss" + std::to_string(i + 1) + "_";
        Stats::Boss& b = s.boss[i];
        f(p + "attempts", b.attempts), f(p + "wins", b.wins), f(p + "best_frames", b.best_frames);
        f(p + "flawless", b.flawless);
    }
    static const char* const COURSES[STAT_COURSES] = {"beginner", "expert", "shuffle"};
    for (int i = 0; i < STAT_COURSES; i++) {
        f(std::string("best_score_") + COURSES[i], s.best_score[i]);
        f(std::string("finished_") + COURSES[i], s.finished_course[i]);
    }
    f("best_wave", s.best_wave), f("best_stage_technical", s.best_stage_technical);
    f("untouched_stages", s.untouched_stages), f("one_credit_finishes", s.one_credit_finishes);
}

// ---- Tracking ----

struct Counters {
    int shots = 0, hits = 0, technical = 0;
    static Counters read() {
        return {int(rd16(COUNTERS + 0xC)), int(rd16(COUNTERS + 4)), int(rd16(COUNTERS + 6))};
    }
};

struct Tracker {
    bool on = false;
    std::mutex mutex;   // everything below: the window reads copies
    Stats stats;
    std::vector<int64_t> unlocked;
    std::vector<int> fresh;   // unlocked, not yet announced

    // Game thread
    uint64_t frame = 0;
    bool rebase = true;          // read the game again without counting the jump (start, game state loaded)
    int state = 3;               // player 1 slot state seen last
    Counters last;
    int lives = 0, alive = 0, stage = -1;
    uint64_t last_hit = 0;       // frame of the last scoring hit
    int hits_this_frame = 0, hits_last_frame = 0;
    // The game and stage on
    bool in_game = false;
    int game_continues = 0;
    int stage_lives_lost = 0;
    uint64_t stage_from = 0;
    Counters stage_base;         // the totals when the stage began
    int64_t base_shots = 0, base_hits = 0, base_technical = 0;
    // Boss mode
    int boss = -1;               // fight on
    uint64_t boss_from = 0;
    int boss_lives_lost = 0;
    uint64_t saved_at = 0;
    bool dirty = false;

    bool playing() const { return state == 1 || state == 2; }

    void load() {
        Ini ini = Ini::load(STATS_FILE);
        each_field(stats, [&](const std::string& k, auto& field) {
            for (auto& [key, v] : ini.rows)
                if (key == k) field = decltype(+field)(v);
        });
        unlocked.assign(TABLE.size(), 0);
        Ini a = Ini::load(ACHIEVEMENTS_FILE);
        for (size_t i = 0; i < TABLE.size(); i++)
            for (auto& [key, v] : a.rows)
                if (key == TABLE[i].key) unlocked[i] = v;
    }

    void save() {
        Ini ini;
        each_field(stats, [&](const std::string& k, auto& field) { ini.put(k, int64_t(field)); });
        Ini a;
        for (size_t i = 0; i < TABLE.size(); i++)
            if (unlocked[i]) a.put(TABLE[i].key, unlocked[i]);
        if (!ini.save(STATS_FILE) || !a.save(ACHIEVEMENTS_FILE)) printf("[achievements] could not save save/stats.ini\n");
        saved_at = frame;
        dirty = false;
    }

    // With the mutex held
    void check_achievements() {
        for (size_t i = 0; i < TABLE.size(); i++)
            if (!unlocked[i] && TABLE[i].progress(stats) >= TABLE[i].goal) {
                unlocked[i] = int64_t(time(nullptr));
                fresh.push_back(int(i));
                dirty = true;
                printf("[achievements] unlocked: %ls\n", TABLE[i].name);
            }
    }

    void begin_stage() {
        stage = rd8(STAGE);
        stage_from = frame;
        stage_lives_lost = 0;
        base_shots = stats.shots, base_hits = stats.hits, base_technical = stats.technical;
    }

    void clear_stage(int s) {
        // Expert skips stages at the start (the stage byte runs through them at once): not cleared
        if (s < 0 || s >= STAT_STAGES || boss >= 0 || frame - stage_from < uint64_t(frames_of(MIN_STAGE_SECONDS))) return;
        Stats::Stage& st = stats.stage[s];
        st.clears++;
        int frames = int(frame - stage_from);
        if (st.best_frames == 0 || frames < st.best_frames) st.best_frames = frames;
        st.best_score = std::max(st.best_score, int(rd32(SCORE + 0x10 + 4 * s)));
        int64_t shots = stats.shots - base_shots, hits = stats.hits - base_hits;
        if (shots > 0) st.best_accuracy = std::max(st.best_accuracy, int(std::min<int64_t>(1000, hits * 1000 / shots)));
        stats.best_stage_technical = std::max(stats.best_stage_technical, int(stats.technical - base_technical));
        if (stage_lives_lost == 0) stats.untouched_stages++;
        dirty = true;
        printf("[achievements] stage %d cleared in %.1f s, hits %lld/%lld\n", s + 1, frames / FRAME_HZ, (long long)hits,
               (long long)shots);
    }

    void end_game(bool finished) {
        if (!in_game) return;
        in_game = false;
        int course = std::clamp(int(rd32(COURSE)), 0, STAT_COURSES - 1);
        if (boss < 0) {
            stats.best_score[course] = std::max(stats.best_score[course], int(rd32(SCORE)));
            if (finished) {
                clear_stage(stage);
                stats.games_finished++;
                stats.finished_course[course]++;
                if (game_continues == 0) stats.one_credit_finishes++;
            }
        }
        dirty = true;
    }

    void frame_tick() {
        frame++;
        int st = rd8(PLAYER_STATE);
        // START already makes the player "playing" on the route selection,
        // and PLAY presses it by itself (runtime.cpp, arcade_entry_frame): a
        // game only counts once its first stage starts, not when it is left
        // there (QUIT TO MAIN MENU)
        uint32_t route = rd32(ROUTE_SELECT);
        if ((st == 1 || st == 2) && (route == 1 || route == 2)) st = 4;
        Counters now = Counters::read();
        int lv = rd8(PLAYER_LIFE), al = int(rd32(VM_ALIVE)), sg = rd8(STAGE);
        std::lock_guard<std::mutex> lock(mutex);
        if (rebase) {   // nothing counted across the jump
            rebase = false;
            state = st, last = now, lives = lv, alive = al;
            if (boss >= 0) stage = sg;
            return;
        }
        bool was_playing = playing();
        state = st;
        if (!was_playing && playing()) {
            if (in_game) {   // back from the CONTINUE screen
                stats.continues++;
                game_continues++;
            } else if (boss < 0) {   // a new game
                end_game(false);
                in_game = true;
                game_continues = 0;
                stats.games++;
                begin_stage();
            }
            last = now, lives = lv;
        }
        if (in_game && st == 3) end_game(lives > 0);   // out: the game is over (lives left: it was finished)
        if (playing()) {
            stats.play_frames++;
            // The counters only go up during a stage; a fall is the next stage starting from zero
            auto add = [](int now, int before) { return now >= before ? now - before : now; };
            stats.shots += add(now.shots, last.shots);
            stats.hits += add(now.hits, last.hits);
            stats.technical += add(now.technical, last.technical);
            if (lv < lives) {
                stats.lives_lost += lives - lv;
                stage_lives_lost += lives - lv;
                boss_lives_lost += lives - lv;
            }
            // Kills: the count of enemies falls right after a hit
            int hits_recent = hits_this_frame + hits_last_frame;
            if (al < alive && hits_recent > 0) stats.kills += std::min(alive - al, hits_recent);
            if (sg != stage) {   // on to the next stage
                if (in_game && stage >= 0) clear_stage(stage);
                begin_stage();
            }
            dirty = true;
        }
        last = now, lives = lv, alive = al;
        hits_last_frame = hits_this_frame;
        hits_this_frame = 0;
        SurvivalView sv = survival_view();
        if (sv.mode == SurvivalMode::Playing && sv.wave > stats.best_wave) stats.best_wave = sv.wave;
        check_achievements();
        if (dirty && frame - saved_at > uint64_t(frames_of(30))) save();
    }
};

Tracker g;

}  // namespace

const std::vector<AchievementInfo>& achievement_table() { return TABLE; }

int64_t achievement_progress(const Stats& s, int index) {
    const AchievementInfo& a = TABLE[size_t(index)];
    return std::clamp<int64_t>(a.progress(s), 0, a.goal);
}

void stats_init() {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.load();
    g.on = true;
    g.rebase = true;
    rt_hook_function(FN_ADD_SCORE);
}

void stats_boss_begin(int boss) {
    if (!g.on) return;
    std::lock_guard<std::mutex> lock(g.mutex);
    g.boss = boss;
    g.boss_from = g.frame;
    g.boss_lives_lost = 0;
    g.in_game = false;
    g.rebase = true;
    g.stats.boss[boss].attempts++;
    g.dirty = true;
}

void stats_boss_end(bool won) {
    if (!g.on || g.boss < 0) return;
    std::lock_guard<std::mutex> lock(g.mutex);
    Stats::Boss& b = g.stats.boss[g.boss];
    if (won) {
        int frames = int(g.frame - g.boss_from);
        b.wins++;
        if (b.best_frames == 0 || frames < b.best_frames) b.best_frames = frames;
        if (g.boss_lives_lost == 0) b.flawless++;
        printf("[achievements] boss %d defeated in %.1f s\n", g.boss + 1, frames / FRAME_HZ);
    }
    g.boss = -1;
    g.check_achievements();
    g.save();
}

void stats_state_loaded() {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.rebase = true;
}

void stats_save() {
    if (!g.on) return;
    std::lock_guard<std::mutex> lock(g.mutex);
    g.save();
}

StatsView stats_view() {
    std::lock_guard<std::mutex> lock(g.mutex);
    StatsView v{g.stats, g.unlocked};
    if (v.unlocked.size() != TABLE.size()) v.unlocked.assign(TABLE.size(), 0);
    return v;
}

std::vector<int> stats_take_new_achievements() {
    std::lock_guard<std::mutex> lock(g.mutex);
    std::vector<int> r;
    r.swap(g.fresh);
    return r;
}

void stats_frame() {
    if (g.on) g.frame_tick();
}

void stats_enter(Ctx& c, uint32_t fn) {
    if (fn == FN_ADD_SCORE && g.on && c.r[4] == 1 && int32_t(c.r[3]) != 7 && int32_t(c.r[3]) != 8) g.hits_this_frame++;
}
