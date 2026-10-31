#include "options.h"

#include <cstdlib>
#include <cstring>

#include "bosses.h"   // BOSS_COUNT
#include "weather.h"

namespace {

// One command-line option. value_name is shown in the help; with Optional
// the value may be left out (an argument starting with '-' is not a value)
struct Entry {
    const char* name;         // nullptr: a section title in the help
    const char* value_name;   // nullptr: no value
    const char* help;
    bool optional = false;    // the value may be left out
    bool test = false;        // a test run: the menu is left out
    std::function<void(Options&, const char*)> set;   // value is nullptr when left out
};

using F = float Options::*;
using B = bool Options::*;

Entry section(const char* title) { return {nullptr, nullptr, title, false, false, {}}; }
Entry flag(const char* name, B field, const char* help, bool test = false) {
    return {name, nullptr, help, false, test, [field](Options& o, const char*) { o.*field = true; }};
}
Entry off(const char* name, B field, const char* help) {
    return {name, nullptr, help, false, false, [field](Options& o, const char*) { o.*field = false; }};
}
Entry zero(const char* name, F field, const char* help) {
    return {name, nullptr, help, false, false, [field](Options& o, const char*) { o.*field = 0; }};
}
Entry num(const char* name, int Options::*field, const char* help, bool test = false) {
    return {name, "N", help, false, test, [field](Options& o, const char* v) { o.*field = atoi(v); }};
}
Entry real(const char* name, F field, const char* help) {
    return {name, "F", help, false, false, [field](Options& o, const char* v) { o.*field = float(atof(v)); }};
}
// Strength F after the name, 1 when left out
Entry strength(const char* name, F field, const char* help) {
    return {name, "[F]", help, true, false, [field](Options& o, const char* v) { o.*field = v ? float(atof(v)) : 1; }};
}
Entry text(const char* name, const char* value_name, std::string Options::*field, const char* help, bool test = false) {
    return {name, value_name, help, false, test, [field](Options& o, const char* v) { o.*field = v; }};
}
Entry custom(const char* name, const char* value_name, const char* help, bool test,
             std::function<void(Options&, const char*)> set) {
    return {name, value_name, help, false, test, std::move(set)};
}
// Parsed by the ExtraOption handler (the modes' own options): listed here for
// the help and the test runs only
Entry elsewhere(const char* name, const char* value_name, const char* help, bool test = false) {
    return {name, value_name, help, false, test, {}};
}

// The order is the order of the help
const std::vector<Entry>& table() {
    static const std::vector<Entry> t = {
        section("Run"),
        off("--no-menu", &Options::menu, "straight to the arcade game, no menu (Esc quits)"),
        flag("--headless", &Options::headless, "no window and full speed, with a report at the end", true),
        num("--frames", &Options::frame_limit, "stop after N frames (600 with --headless)", true),
        text("--rom", "ZIP", &Options::rom_zip, "path to lostwsga.zip (or LW_ROM_ZIP; by default next to the exe)"),
        text("--data", "DIR", &Options::data_dir, "folder with the ROM already extracted by tools/romtool.py (data)"),
        flag("--boot", &Options::show_boot, "show the power-on screens before the SEGA logo"),
        custom("--make-boss-state", "N", "play without a window up to boss N (1-4) and save its state", true,
               [](Options& o, const char* v) {
                   o.boss_maker = atoi(v) - 1;   // see boss_maker_frame
                   o.headless = true;
               }),
        section("Picture"),
        real("--scale", &Options::supersample, "internal 3D resolution multiplied by F (1)"),
        num("--msaa", &Options::msaa, "samples per pixel (4; 1 = no MSAA)"),
        custom("--aa", "msaa|dlaa|none", "antialiasing: msaa (default), dlaa (NVIDIA) or none", false,
               [](Options& o, const char* v) {
                   o.dlaa = strcmp(v, "dlaa") == 0;
                   if (strcmp(v, "none") == 0 || strcmp(v, "off") == 0) o.msaa = 1;
               }),
        num("--aniso", &Options::aniso, "anisotropic filtering (8; 0 = no mipmaps, original look)"),
        custom("--aspect", "A:B", "wider 3D view: 16:9 (default), 21:9, 4:3 or a number", false,
               [](Options& o, const char* v) {
                   const char* colon = strchr(v, ':');
                   o.max_aspect = colon ? float(atof(v) / atof(colon + 1)) : float(atof(v));
               }),
        real("--light-gain", &Options::light_gain, "multiply the sunlight and the ambient light (1)"),
        strength("--bloom", &Options::bloom, "glow around bright things, strength F (1)"),
        zero("--no-bloom", &Options::bloom, "no glow"),
        strength("--ao", &Options::ao, "ambient occlusion, strength F (1, the maximum)"),
        zero("--no-ao", &Options::ao, "no ambient occlusion"),
        strength("--haze", &Options::haze, "distance haze that hides the draw distance (1)"),
        zero("--no-haze", &Options::haze, "no distance haze"),
        real("--saturation", &Options::saturation, "colour saturation (0.8; 1 = original)"),
        real("--gamma", &Options::gamma, "gamma (1 = original; higher is brighter)"),
        custom("--sky", "original|procedural", "drawn sky (default) or the original picture", false,
               [](Options& o, const char* v) { o.sky_procedural = strcmp(v, "original") != 0; }),
        off("--no-sun", &Options::sun, "no sun in the drawn sky"),
        off("--no-shadows", &Options::shadows, "no sun shadows"),
        custom("--rt", "off|shadows|ao", "ray tracing: sun shadows, and ambient occlusion (ao, the default)", false,
               [](Options& o, const char* v) {
                   o.ray_tracing = strcmp(v, "off") == 0 ? 0 : strcmp(v, "shadows") == 0 ? 1 : 2;
               }),
        custom("--no-rt", nullptr, "no ray tracing (as --rt off)", false, [](Options& o, const char*) { o.ray_tracing = 0; }),
        off("--no-shafts", &Options::shafts, "no sun shafts"),
        off("--no-soft", &Options::soft, "hard edges where translucent effects meet the geometry"),
        off("--no-lightning", &Options::lightning, "no lightning in the storm fog"),
        custom("--rain", "off|light|heavy", "rain where it rains in the game (heavy by default)", false,
               [](Options& o, const char* v) { o.rain = strcmp(v, "light") == 0 ? 1 : strcmp(v, "off") == 0 ? 0 : 2; }),
        custom("--weather", "sunny|cloudy|fog|rain|storm", "this weather in every game (random in RANDOM SPAWNS and survival)",
               false, [](Options& o, const char* v) { o.weather = weather_from_name(v); }),
        off("--no-hdr", &Options::hdr, "clipped lighting like the original"),
        off("--no-rim", &Options::rim, "no rim light on dinosaurs and characters"),
        off("--no-relief", &Options::relief, "flat textures, no relief"),
        off("--no-sharpen", &Options::sharpen, "no adaptive sharpening of the 3D image"),
        custom("--torch", "modern|original", "torch in the night stages: modern (default) or original", false,
               [](Options& o, const char* v) { o.modern_torch = strcmp(v, "original") != 0; }),
        off("--no-interp", &Options::interp, "game frames as they are, not interpolated"),
        flag("--half-rate", &Options::half_rate, "above 100 Hz, draw the 3D every other refresh (faster)"),
        off("--no-vsync", &Options::vsync, "images go out as soon as they are drawn, not at the refresh (tearing)"),
        flag("--letterbox", &Options::letterbox, "keep the black bars of the cutscenes"),
        flag("--pixel2d", &Options::pixel_2d, "2D layers without xBR upscaling"),
        flag("--classic-text", &Options::classic_text, "original pixel letters instead of vector fonts"),
        flag("--hud-centre", &Options::hud_centre, "the HUD stays in the 4:3 area"),
        flag("--dump-textures", &Options::dump_textures, "write new textures to out/textures"),
        custom("--subtitles", "off|en|jp", "subtitles for the voices", false,
               [](Options& o, const char* v) {
                   o.subtitles = strcmp(v, "jp") == 0 ? 2 : strcmp(v, "en") == 0 ? 1 : 0;
               }),
        section("Sound"),
        real("--volume", &Options::volume, "output gain (5)"),
        custom("--speakers", "stereo|surround|headphones",
               "stereo (default), 4.0 surround (separate rears) or headphones", false,
               [](Options& o, const char* v) {
                   o.speakers = strcmp(v, "surround") == 0 ? 1 : strcmp(v, "headphones") == 0 ? 2 : 0;
               }),
        num("--rear-boost", &Options::rear_boost, "dB on the rear speakers, -6 to 12 (roars and effects; 0)"),
        custom("--weather-sound", "off|low|medium|high", "rain and thunder of the weather and the lightning (medium)", false,
               [](Options& o, const char* v) {
                   o.weather_sound = strcmp(v, "off") == 0 ? 0 : strcmp(v, "low") == 0 ? 1 : strcmp(v, "high") == 0 ? 3 : 2;
               }),
        off("--linear-samples", &Options::cubic_samples, "linear interpolation of the samples, like the original SCSP"),
        text("--wav", "FILE", &Options::wav_path, "record the sound to a WAV"),
        section("Players"),
        off("--one-mouse", &Options::several_mice, "every mouse moves player 1's gun (no second mouse)"),
        flag("--free-play", &Options::free_play, "start and continue with 1 / 2 alone, no coin needed"),
        num("--crosshair-hide", &Options::crosshair_hide, "hide a crosshair after N seconds unused (0 = never)"),
        section("Debugging and tests"),
        custom("--shot", "N", "save frame N to out/capture.bmp and the scene to out/scene.txt, then exit", true,
               [](Options& o, const char* v) { o.shot_frame = strtoull(v, nullptr, 10); }),
        text("--replay", "FILE", &Options::replay,
             "show a snapshot (F9) or play back a folder of them (LW_FILM)", true),
        custom("--auto", "coin@F,start@F", "press buttons on the given frames", true,
               [](Options& o, const char* v) { o.auto_scripts.push_back(v); }),
        custom("--aim", "X,Y", "gun fixed at ADC values X,Y (calibration)", true,
               [](Options& o, const char* v) {
                   o.aim_x = o.aim_y = 128;
                   sscanf(v, "%d,%d", &o.aim_x, &o.aim_y);
                   o.fixed_aim = true;
               }),
        text("--diff", "FN@FRAME", &Options::diff,
             "save the state before and after FN on that frame (tools/diff_unicorn.py)"),
        custom("--trace", "A,B,...", "count the calls to those functions (hex addresses)", false,
               [](Options& o, const char* v) { o.traces.push_back(v); }),
        section("Survival mode"),
        elsewhere("--survival", "STAGE:ZONE", "wave arena in that stage and zone (see README)"),
        elsewhere("--trace-survival", nullptr, "trace of the mode in out/survival_trace.txt", true),
        section("Random spawns"),
        elsewhere("--random-spawns", nullptr, "the arcade game with its encounters shuffled (CLASSIC > RANDOM SPAWNS)"),
    };
    return t;
}

const Entry* find(const char* name) {
    if (strcmp(name, "--luz") == 0) name = "--light-gain";   // its old name, still accepted
    for (const Entry& e : table())
        if (e.name && strcmp(e.name, name) == 0) return &e;
    return nullptr;
}

}  // namespace

void print_usage(FILE* out) {
    fprintf(out,
            "Usage: LostWorld.exe [options]\n"
            "  no options: main menu and a Vulkan window at real speed (Esc pauses,\n"
            "              F11 or Alt+Enter full screen)\n");
    for (const Entry& e : table()) {
        if (!e.name) {
            fprintf(out, "%s:\n", e.help);
            continue;
        }
        std::string left = e.name;
        if (e.value_name) left += std::string(" ") + e.value_name;
        fprintf(out, "  %-28s %s\n", left.c_str(), e.help);
    }
}

bool options_disable_menu(int argc, char** argv, std::initializer_list<const char*> more) {
    // LW_BOSS starts a boss fight, which the menu code loads: it has to stay on
    // even with --frames or --wav (e.g. to record a fight's sound)
    if (getenv("LW_BOSS")) return false;
    for (int i = 1; i < argc; i++) {
        const Entry* e = find(argv[i]);
        if (e && e->test) return true;
        for (const char* m : more)
            if (strcmp(argv[i], m) == 0) return true;
    }
    return false;
}

bool parse_options(Options& o, int argc, char** argv, const ExtraOption& extra, int& exit_code) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "/?") == 0) {
            print_usage(stdout);
            exit_code = 0;
            return false;
        }
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (const Entry* e = find(a); e && e->set) {
            if (!e->value_name) {
                e->set(o, nullptr);
            } else if (e->optional) {
                bool given = next && next[0] != '-';
                e->set(o, given ? next : nullptr);
                if (given) i++;
            } else if (next) {
                e->set(o, next);
                i++;
            } else {
                fprintf(stderr, "Missing value for %s\n", a);
                print_usage(stdout);
                exit_code = 1;
                return false;
            }
            continue;
        }
        int used = extra ? extra(o, a, next) : 0;
        if (used > 0) {
            i += used - 1;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", a);
        print_usage(stdout);
        exit_code = 1;
        return false;
    }
    if (o.boss_maker != -1 && (o.boss_maker < 0 || o.boss_maker >= BOSS_COUNT)) {
        fprintf(stderr, "--make-boss-state: the boss goes from 1 to %d\n", BOSS_COUNT);
        exit_code = 1;
        return false;
    }
    return true;
}
