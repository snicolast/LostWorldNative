// CLASSIC > RANDOM SPAWNS (see random_spawns.h). Addresses: docs/game_ram.md.
//
// The changes are made in place and keep every word count: nothing outside
// the encounter points into it, the boss mode's start points (bosses.h) stay
// where they are, and the machine runs the same operations in the same order
// apart from the reordered sections. Before each encounter the whole script
// is put back from the ROM, so only the encounter being played is ever
// changed, and with the mode off (or in the attract, survival or boss mode)
// it is the arcade's own.
#include "random_spawns.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#include "../common/random_seed.h"
#include "encounter_script.h"
#include "game_ram.h"
#include "ppc_rt.h"
#include "weather.h"

extern int g_boss_maker;   // runtime.cpp: --make-boss-state plays by itself
bool survival_enabled();

namespace {

using script::FN_ENCOUNTER_START, script::FN_RAPTOR, script::FN_PLACED, script::FN_PLACED_XZ;
using namespace ram;   // game RAM (game_ram.h)
constexpr uint32_t FN_ADD_SCORE = 0x0003DC60;   // (type, player from 1, points): every scoring hit
constexpr float NEAR_LOOK = 120;             // raptor groups swap only between camera turns this close
constexpr int TYPE_CHANCE = 30;              // % of the raptors that take another type of the zone's
constexpr int LIFE_MIN = 70, LIFE_MAX = 150; // a boss's life, % of the arcade's

// The bosses' life: a word in the boss object that each hit lowers (by 1, or 2
// with flag +0xC8 clear; the Deinosuchus's weak spot by 14) and the boss's
// code sets when the fight starts (the T-Rex female 150 or 250 in
// fn_000CC48C, the Deinosuchus 170, the Carnotaurus 530, the T-Rex 620).
// Seen from the score call of each hit: the return address says which boss,
// and the caller's frame (r31 at the call) holds the object
struct BossHit {
    uint32_t lr;     // return address of the fn_0003DC60 call
    uint32_t slot;   // the object pointer, at this offset of the caller's frame
    uint32_t life;   // the life word in the object
};
constexpr BossHit BOSS_HITS[] = {
    {0x000CCB74, 0x08, 0x98},   // T-Rex female (stage 1)
    {0x0008E86C, 0x10, 0x4C}, {0x0008E888, 0x10, 0x4C},   // Deinosuchus, body (10 / 200 points)
    {0x0008F14C, 0x0C, 0x4C},   // Deinosuchus, its weak spot
    {0x000E86EC, 0x18, 0x98},   // Carnotaurus (stage 4)
    {0x000FE9D4, 0x08, 0x98},   // T-Rex (stage 5)
};
// The boss being fought: its object and its share of the arcade's life
struct Boss {
    uint32_t obj = 0;
    int pct = 100;
};
Boss g_boss;

std::atomic<bool> g_on{false};
std::atomic<bool> g_playing{false};   // a shuffled arcade game is being played
uint32_t g_rng = 1;
bool g_patched = true;    // the script in RAM may not be the ROM's (a game state loaded)

uint32_t rnd(uint32_t n) { return script::rand_below(g_rng, n); }
int32_t rnd_between(int32_t lo, int32_t hi) { return lo + int32_t(rnd(uint32_t(hi - lo + 1))); }

int32_t rom_word(uint32_t pc) { return int32_t(rd32(script::ROM + 4 * pc)); }

// One operation or call of an encounter, with its words
using Tok = script::Item;
bool is_raptor(const Tok& t) { return t.fn() == FN_RAPTOR && t.w.size() >= 6 && t.w[3] >= 1 && t.w[3] <= 5; }

// Encounter `id` as the ROM has it, from its header to its end (included); empty if not found
std::vector<Tok> parse(int id) {
    std::vector<Tok> out;
    uint32_t h = script::find(rom_word, id);
    if (h == script::NOT_FOUND || !script::walk(rom_word, h, [&](const Tok& t) { out.push_back(t); })) return {};
    return out;
}

// The encounters started by the current zone's triggers (its stops)
std::vector<int> zone_encounters() {
    std::vector<int> ids;
    uint32_t index = rd32(ZONE_INFO + 12 * (rd32(ZONE) & 15));
    if (index > 64) return ids;
    uint32_t rec = rd32(TRIGGER_TABLES + 28 * index + 8);
    for (int k = 0; rec && rec < RAM_SIZE && k < 128; k++, rec += 24) {
        if (int32_t(rd32(rec)) == -1) break;
        if (rd32(rec) == FN_ENCOUNTER_START) ids.push_back(int(rd32(rec + 16)));
    }
    return ids;
}

// A raptor the zone's scripts bring in: where the camera looks then, its type and group
struct Spawn {
    float x, z;
    int32_t type, group;
};

void gather(const std::vector<Tok>& toks, std::vector<Spawn>& out) {
    bool look = false;
    float x = 0, z = 0;
    for (const Tok& t : toks) {
        if (t.op() == -9) look = true, x = float(t.w[1]), z = float(t.w[3]);
        if (look && is_raptor(t)) out.push_back({x, z, t.w[4], t.w[5]});
    }
}

// A plain section: camera turn, maybe its setup and waits, enemy calls, wait for them all
bool plain_section(const std::vector<Tok>& toks, size_t a, size_t& end) {
    if (a >= toks.size() || toks[a].op() != -9) return false;
    int enemies = 0;
    for (size_t i = a + 1; i < toks.size(); i++) {
        const Tok& t = toks[i];
        if (t.op() == -1) {
            end = i + 1;
            return enemies > 0;
        }
        if (t.enemy()) enemies++;
        else if (!(t.op() == -25 || t.op() == -26 || t.op() == -21)) return false;
    }
    return false;
}

struct Changes {
    int moved = 0, types = 0, placed = 0, timed = 0, reordered = 0;
};

// Runs of two or more plain sections in a row, followed by another camera turn
// or the end: their order is shuffled
void reorder(std::vector<Tok>& toks, Changes& ch) {
    for (size_t a = 0; a < toks.size();) {
        std::vector<std::pair<size_t, size_t>> run;
        size_t b = a, e;
        while (plain_section(toks, b, e)) run.push_back({b, e}), b = e;
        if (run.size() < 2) {
            a++;
            continue;
        }
        int32_t next = b < toks.size() ? toks[b].op() : -999;
        if (next == -9 || next == -999 || next == -998) {
            std::vector<std::vector<Tok>> parts;
            for (auto& [s, f] : run) parts.emplace_back(toks.begin() + long(s), toks.begin() + long(f));
            for (size_t i = parts.size() - 1; i > 0; i--) std::swap(parts[i], parts[rnd(uint32_t(i + 1))]);
            size_t k = a;
            uint32_t pc = toks[a].pc;   // the same words in all, laid out again from the first
            for (auto& p : parts)
                for (Tok& t : p) {
                    t.pc = pc, pc += uint32_t(t.w.size());
                    toks[k++] = t;
                }
            ch.reordered += int(run.size());
        }
        a = b;
    }
}

void shuffle(int id) {
    std::vector<Tok> toks = parse(id);
    if (toks.empty()) return;
    std::vector<Spawn> spawns;
    std::set<int32_t> types;
    std::vector<int> ids = zone_encounters();
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    for (int other : ids) gather(other == id ? toks : parse(other), spawns);
    for (const Spawn& s : spawns) types.insert(s.type);
    Changes ch;
    bool look = false;
    float x = 0, z = 0;
    for (Tok& t : toks) {
        if (t.op() == -9) look = true, x = float(t.w[1]), z = float(t.w[3]);
        if (!t.enemy()) continue;
        if (is_raptor(t) && look) {
            std::vector<int32_t> groups;
            for (const Spawn& s : spawns)
                if (std::hypot(s.x - x, s.z - z) < NEAR_LOOK) groups.push_back(s.group);
            int32_t g = groups.empty() ? t.w[5] : groups[rnd(uint32_t(groups.size()))];
            if (g != t.w[5]) t.w[5] = g, ch.moved++;
            if (types.size() > 1 && types.count(t.w[4]) && rnd(100) < uint32_t(TYPE_CHANCE)) {
                auto it = types.begin();
                std::advance(it, rnd(uint32_t(types.size())));
                if (*it != t.w[4]) t.w[4] = *it, ch.types++;
            }
        }
        if (t.fn() == FN_PLACED && t.w.size() >= 7) {
            t.w[4] += rnd_between(-30, 30);
            t.w[5] += rnd_between(-20, 20);
            t.w[6] = (t.w[6] + rnd_between(-1024, 1024)) & 0xFFFF;
            ch.placed++;
        }
        if (t.fn() == FN_PLACED_XZ && t.w.size() >= 5) {
            t.w[3] += rnd_between(-25, 25);
            t.w[4] += rnd_between(-15, 15);
            ch.placed++;
        }
        // When it comes: its delay 70..130 %, and now and then a little later when it had none
        int32_t d = t.w[0];
        t.w[0] = d > 0 ? std::max(1, d * rnd_between(70, 130) / 100) : rnd(3) == 0 ? rnd_between(0, 20) : 0;
        ch.timed += t.w[0] != d;
    }
    reorder(toks, ch);
    for (const Tok& t : toks)
        for (size_t k = 0; k < t.w.size(); k++) wr32(script::BASE + 4 * (t.pc + uint32_t(k)), uint32_t(t.w[k]));
    g_patched = true;
    printf("[random] encounter %d: %d raptors moved, %d other types, %d placed, %d retimed, %d sections reordered\n",
           id, ch.moved, ch.types, ch.placed, ch.timed, ch.reordered);
}

void restore() {
    for (uint32_t i = 0; i < script::WORDS; i++) wr32(script::BASE + 4 * i, rd32(script::ROM + 4 * i));
    g_patched = false;
}

bool rom_ok() {   // the script is where the boot code copies it from: encounter 2 first
    static int ok = -1;
    if (ok < 0) {
        ok = rom_word(0) == -10 && rom_word(1) == 2;
        if (!ok) printf("[random] the encounter scripts are not where they should be in the ROM: mode off\n");
    }
    return ok > 0;
}

// Research aid: LW_HIT_PROBE=FILE writes RAM 0x100000..0x200000 to FILE the
// frame after every scoring hit and every 120 frames otherwise, each block
// headed by {frame, hits so far} (to find where the bosses keep their life)
FILE* g_probe = nullptr;
uint32_t g_probe_hits = 0, g_probe_dumped = ~0u, g_probe_count = 0;
uint64_t g_probe_frame = 0;
constexpr uint32_t PROBE_FROM = 0x100000, PROBE_SIZE = 0x100000, PROBE_MAX = 600;

}  // namespace

bool random_spawns_option(const std::string& name) {
    if (name != "--random-spawns") return false;
    random_spawns_set(true);
    return true;
}

void random_spawns_hooks() {
    for (uint32_t fn : {FN_ENCOUNTER_START, FN_ADD_SCORE}) rt_hook_function(fn);
    if (const char* p = getenv("LW_HIT_PROBE")) g_probe = fopen(p, "wb");
}

void random_spawns_frame() {
    g_probe_frame++;
    static uint8_t stage = 0xFF;   // a new stage, a new boss (the next one may be put where the last one was)
    bool new_stage = rd8(STAGE) != stage;
    if (new_stage) stage = rd8(STAGE), g_boss = {};
    // ... and new weather (weather.h), also when the game starts
    bool playing = g_on && rd32(COURSE_SELECT) == 0xC && !survival_enabled() && g_boss_maker < 0;
    if (playing && (new_stage || !g_playing)) weather_roll();
    g_playing = playing;
    if (!g_probe || g_probe_count >= PROBE_MAX) return;
    if (g_probe_hits == g_probe_dumped && g_probe_frame % 120) return;
    uint32_t head[2] = {uint32_t(g_probe_frame), g_probe_hits};
    fwrite(head, 4, 2, g_probe);
    fwrite(g_ram + PROBE_FROM, 1, PROBE_SIZE, g_probe);
    fflush(g_probe);
    g_probe_dumped = g_probe_hits;
    g_probe_count++;
}

void random_spawns_set(bool on) {
    if (on && !g_on) g_rng = random_seed() | 1;
    g_boss = {};
    g_on = on;
}

bool random_spawns_playing() { return g_playing; }

void random_spawns_enter(Ctx& c, uint32_t fn) {
    if (fn == FN_ADD_SCORE && g_probe) {
        g_probe_hits++;
        printf("[probe] frame %llu hit %u: type %u player %u points %u (lr %08X)\n", (unsigned long long)g_probe_frame,
               g_probe_hits, c.r[3], c.r[4], c.r[5], c.lr);
    }
    bool shuffled = g_on && rd32(COURSE_SELECT) == 0xC && !survival_enabled() && g_boss_maker < 0;
    if (fn == FN_ADD_SCORE && shuffled)
        for (const BossHit& b : BOSS_HITS) {
            if (c.lr != b.lr) continue;
            uint32_t obj = rd32(c.r[31] + b.slot);
            if (obj >= RAM_SIZE - 0x100) break;
            if (obj == g_boss.obj) break;
            // Its first hit: the fight's opening life scaled. (Later phases of the
            // Carnotaurus and the T-Rex set the word again and again, to about 100
            // and 150, as more than a life: those stay as they are)
            g_boss = {obj, LIFE_MIN + int(rnd(LIFE_MAX - LIFE_MIN + 1))};
            if (const char* e = getenv("LW_BOSS_LIFE")) g_boss.pct = std::max(1, atoi(e));   // testing aid: this %
            int32_t life = int32_t(rd32(obj + b.life));
            if (life > 10) {
                int32_t scaled = std::max(1, life * g_boss.pct / 100);
                wr32(obj + b.life, uint32_t(scaled));
                printf("[random] boss life %d -> %d (%d %%)\n", life, scaled, g_boss.pct);
            }
            break;
        }
    if (fn != FN_ENCOUNTER_START || !rom_ok()) return;
    if (g_patched || shuffled) restore();
    if (shuffled) shuffle(int(rd32(c.r[3] + 0x14)));
}
