// The Lost World running inside NicoEngine (live_game.h). The game's
// emulator (src/runtime and src/audio) is compiled as it is; this file
// stands where the game's own window does (src/runtime/app.cpp): it starts
// the boards the way that file does for the cabinet's own game. The few
// things the emulator asks of whoever hosts it are in live_host.cpp.
// Compiled by the engine only (docs/NICOENGINE.md), never by the game's build.
//
// What the game's window adds for the PC is left out: the menus, the saved
// settings, the records and achievements, the survival and boss modes, the
// shuffled encounters, the weather. The game is the cabinet's: a coin and
// START. It is played with the mouse, the keyboard or gamepads, through the
// engine's light gun module (its modules/light_guns.h).
#include "live_game.h"

#include <windows.h>

#include <mmsystem.h>   // timeBeginPeriod

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "disk_writer.h"
#include "eeprom.h"
#include "inputs.h"
#include "game_ram.h"
#include "light_guns.h"
#include "ppc_rt.h"
#include "r3d_scene.h"
#include "runtime.h"   // (and with it the game's sound: src/audio)

namespace game {

using nico::gun::Guns;
using nico::gun::Seen;
using nico::gun::Shooters;
using nico::gun::Target;

namespace {

// As the game's own load_rom, but a file that is missing is told to the caller
bool rom_file(const char* name, std::vector<uint8_t>& out) { return load_rom(name, out, false) && !out.empty(); }

bool g_timer_set = false;

// (a build for where there is no ROM: CMakeLists.txt)
#ifdef NICO_LIVE_STANDIN
constexpr bool STANDIN = true;
#else
constexpr bool STANDIN = false;
#endif

nico::Devices* g_devices = nullptr;

Shooters g_shooters;
// A pad's vibration, as the game's own settings have it: off, low, medium (as it starts) or high
constexpr float VIBRATION[4] = {0.0f, 0.45f, 0.75f, 1.0f};
int g_vibration = 2;
// The sound board's gain, 0..10: 5 is the cabinet's own
int g_volume = 5;
// The skies: 0 the cabinet's own pictures, 1 drawn anew (the game's own program's SKY: PROCEDURAL, as it starts)
int g_sky = 1;
// Each player's lives at the last step (the game's own count: game_ram.h)
constexpr uint32_t LIVES[2] = {ram::P1_LIVES, ram::P2_LIVES};
uint32_t g_lives_was[2] = {0, 0};

// The settings that are not the guns' own, with where each value is kept
template <class F>
void each_setting(F&& f) {
    using nico::Setting;
    Setting volume;
    volume.id = "volume", volume.group = "Sound", volume.label = "Sound", volume.kind = Setting::Kind::Level, volume.value = g_volume, volume.initial = 5;
    volume.least = 0, volume.most = 10, volume.step = 1;
    volume.hint = "The dinosaurs, the guns and the music: 5 is as loud as the cabinet";
    f(std::move(volume), &g_volume);
    Setting sky;
    sky.id = "sky", sky.group = "Effects", sky.label = "Sky", sky.kind = Setting::Kind::Choice;
    sky.choices = {"Original", "Procedural"};
    sky.value = g_sky, sky.initial = 1;
    sky.hint = "The skies drawn anew with soft clouds that drift, sharp at any size, or the cabinet's own pictures";
    f(std::move(sky), &g_sky);
    Setting vibration;
    vibration.id = "pad_vibration", vibration.group = "Pad", vibration.label = "Vibration", vibration.kind = Setting::Kind::Choice;
    vibration.choices = {"Off", "Low", "Medium", "High"};
    vibration.value = g_vibration, vibration.initial = 2;
    vibration.hint = "Each shot and each reload, and a jolt when a dinosaur gets the player";
    f(std::move(vibration), &g_vibration);
}

// What there is to shoot at, for a pad's aim assist: a box around each enemy
// the game placed on screen in its last frame (runtime.h, enemy_box: the
// same boxes as the game's own window)
std::vector<Target> targets() {
    std::vector<Target> out;
    for (const EnemyMark& m : enemy_marks()) {
        const ScreenBox b = enemy_box(m);
        out.push_back({b.x0, b.y0, b.x1, b.y1});
    }
    return out;
}

}   // namespace

bool live_start(const std::string& data_dir, const std::string& save_dir, std::string& error) {
    if (STANDIN) {
        error = "this build has a stand-in where the game's recompiled program goes (NICO_LIVE_STANDIN): it can start no game";
        return false;
    }
    // The ROM, as the game's build left it in its data folder
    g_data_dir = data_dir;
    std::vector<uint8_t> program, samples;
    if (!rom_file("crom.bin", g_crom)) {
        error = "No crom.bin in " + data_dir + ".\nIt is one of the files the game's build extracts from your own ROM (build.bat, or the launcher).";
        return false;
    }
    rom_file("banked_crom.bin", g_banked);
    rom_file("vrom.bin", g_vrom);
    rom_file("sound_program.bin", program);
    rom_file("sound_samples.bin", samples);
    if (g_banked.empty() || g_vrom.empty() || program.empty() || samples.empty()) {
        error = "The ROM in " + data_dir + " is incomplete: it has crom.bin but not banked_crom.bin, vrom.bin,\n"
                "sound_program.bin and sound_samples.bin. Build the game again from a complete lostwsga.zip.";
        return false;
    }
    printf("ROM: %s\n", data_dir.c_str());

    // The sound board, with the game's own defaults (how loud: as set,
    // live_set). The rain and the thunder its PC edition adds are not heard
    g_sound.init(std::move(program), std::move(samples), &g_audio);
    g_sound.set_volume(float(g_volume));
    Scsp::set_cubic(true);
    g_sound.set_rear_boost(0);
    g_sound.weather().set_level(0.0f);
    g_audio.set_speakers(AudioStream::Speakers(0));

    g_ram = static_cast<uint8_t*>(calloc(RAM_SIZE + 8, 1));
    if (!g_ram) {
        error = "no memory for the game";
        return false;
    }
    // The cabinet's memory (operator settings, rankings): one of the engine's
    // own, so the game's own program keeps the one it has
    CreateDirectoryA(save_dir.c_str(), nullptr);
    g_eeprom.load(save_dir + "/eeprom.bin", false);

    timeBeginPeriod(1);
    g_timer_set = true;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_start_time);
    g_realtime = true;
    g_audio.start();
    // The cabinet's game from its SEGA logo: nothing held, its attract as it
    // is, no free play but the operator's, no subtitles, the power-on
    // screens gone through unseen and silent
    g_hold = false;
    g_skip_attract = false;
    g_free_play = false;
    g_subtitles = 0;
    g_boot_skip = g_fast_idle = true;
    g_sound.set_dsp_enabled(false);
    g_sound.set_muted(true);
    // The game's code uses the original CPU's stack as memory and nests many
    // calls: it runs on a thread with 512 MB of stack, as in the game
    HANDLE thread = CreateThread(nullptr, 512u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        error = "the game's thread could not be started";
        g_audio.stop();
        return false;
    }
    CloseHandle(thread);
    return true;
}

void live_hold(bool on) { g_hold = on; }

bool live_shown() { return !g_boot_skip; }

void live_devices(nico::Devices* all) { g_devices = all; }

// The cabinet's controls (the engine's modules/light_guns.h has what does what), and what a pad
// feels: a jolt for each shot and each reload, and a longer one when its
// player loses a life, as in the game's own window
void live_controls(const std::function<bool(int)>* held, const float* pointer) {
    const bool playing = held != nullptr;
    Inputs& in = g_inputs;
    Seen seen;
    const std::vector<Target> boxes = playing && g_devices && g_shooters.controls.assist ? targets() : std::vector<Target>{};
    seen.playing[0] = in.p[0].playing, seen.playing[1] = in.p[1].playing;
    seen.reach = gun_reach();
    seen.targets = &boxes;
    const Guns& d = g_shooters.step(held, playing ? g_devices : nullptr, pointer, seen, now_seconds());
    for (int p = 0; p < 2; p++) {
        const Guns::Gun& gun = d.gun[p];
        in.p[p].coin = d.coin[p], in.p[p].start = d.start[p];
        in.p[p].trigger = gun.trigger, in.p[p].offscreen = gun.reload;
        if (gun.aims) set_player_aim(p, gun.x, gun.y);
        if (gun.join) player_join(p);
    }
    in.test = d.test, in.service = d.service;
    if (playing) join_update();   // (a credit and then START, each held for a moment)

    const float strength = VIBRATION[std::clamp(g_vibration, 0, 3)];
    bool hit[2];
    for (int p = 0; p < 2; p++) {
        const uint32_t lives = rd32(LIVES[p]);
        hit[p] = in.p[p].playing && lives < g_lives_was[p];
        g_lives_was[p] = lives;
    }
    for (int i = 0; g_devices && strength > 0 && i < nico::Devices::PADS; i++) {
        const Guns::PadSays& pad = d.pad[i];
        if (pad.player < 0) continue;
        if (pad.fired) {
            g_devices->rumble(i, strength * 0.2f, strength * 0.5f, 70);
            g_devices->rumble_triggers(i, 0.0f, strength * 0.6f, 60);
        } else if (pad.reloaded) {
            g_devices->rumble(i, strength * 0.35f, 0.0f, 50);
        }
        if (hit[pad.player] && pad.used) g_devices->rumble(i, strength * 0.9f, strength * 0.7f, 320);
    }
}

std::vector<std::string> live_keys() { return g_shooters.lines(); }

// (no page of its own)
std::vector<std::string> live_pages() { return {}; }
std::vector<nico::Row> live_rows(int) { return {}; }
nico::Prompt live_prompt() { return {}; }
void live_ask(const nico::Ask&) {}
std::vector<nico::Menu::Key> live_in_menu(bool) { return {}; }

// As the game's own window for each picture: the guns cover the whole of it,
// the sides of a wide one too, and the game is told how far beyond its own
// screen that is (it then aims its shots and draws its X from where the gun
// points, not from the eight bits the cabinet's gun gives)
void live_before_build(R3DSceneBuilder&, float wide, nico::Lighting) { g_aim_reach = std::max(0.0f, (wide - 1.0f) / 2.0f); }

// The stage select screen marks the gun with a small textured dot in its top
// view (in a stage the game draws an X with no texture): the game maps the
// gun another way there, and keeps its dot on its own 4:3 picture
void live_builder(R3DSceneBuilder&, TexturePack&) {}
bool live_skies() { return g_sky != 0; }
std::vector<nico::WorldObject> live_objects() { return {}; }
std::string live_report(const std::vector<uint32_t>&) { return {}; }

void live_after_build(const R3DScene& scene) { g_select_aim = select_dot_on_screen(scene); }

// The sound first, then the keyboard's page, the mouse's and the pad's, its vibration before the rest
std::vector<nico::Setting> live_settings() {
    std::vector<nico::Setting> out, pad;
    each_setting([&](nico::Setting s, const int*) { (s.group == "Pad" ? pad : out).push_back(std::move(s)); });
    for (nico::Setting& s : g_shooters.settings()) {
        if (s.group == "Pad" && !pad.empty()) {
            for (nico::Setting& p : pad) out.push_back(std::move(p));
            pad.clear();
        }
        out.push_back(std::move(s));
    }
    return out;
}

bool live_set(const std::string& id, int value) {
    if (g_shooters.set(id, value)) return true;
    bool done = false;
    each_setting([&](const nico::Setting& s, int* where) {
        if (s.id != id || !s.takes(value)) return;
        *where = value;
        done = true;
        if (id == "volume") g_sound.set_volume(float(value));
    });
    return done;
}

void live_stop() {
    // (the game's thread cannot be unwound: it parks itself at its next frame)
    g_quit = true;
    for (int i = 0; i < 100 && !g_parked; i++) Sleep(10);
    g_devices = nullptr;
    g_audio.stop();
    disk_flush();   // the cabinet's memory, if the game wrote to it
    if (g_timer_set) timeEndPeriod(1);
    g_timer_set = false;
}

}   // namespace game
