#include "frontend.h"

#include "../runtime/bosses.h"
#include "../runtime/survival.h"
#include "../runtime/weather.h"
#include "crosshair_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <ctime>
#include <iterator>
#include <thread>

#include <shellapi.h>
#include "../video/image_io.h"

namespace {

// The game's letters: yellow at the top fading to orange, dark outline
constexpr uint32_t GOLD_TOP = 0xFFEA5C, GOLD_BOTTOM = 0xFF8A00, EDGE = 0x1C0A00;
constexpr uint32_t PALE_TOP = 0xE6DCC0, PALE_BOTTOM = 0xBFB294, DIM = 0x6A6559;
constexpr uint32_t AMBER = 0xFFA032, HINT = 0xC9B88E, FOOT = 0x8A8070;

// The donation link on the main menu, for whoever wants to support the port
constexpr const wchar_t* COFFEE_URL = L"https://buymeacoffee.com/snicolast8u";
constexpr const wchar_t* COFFEE_LABEL = L"BUY ME A COFFEE";
constexpr uint32_t COFFEE_INK = 0x0D0C22;   // their dark blue-black
constexpr const wchar_t* COFFEE_HINT = L"Support the port: opens buymeacoffee.com in your browser";

using ui::chan;
using ui::mix;
using ui::over;

bool load_image(const std::wstring& path, std::vector<uint32_t>& px, int& w_out, int& h_out);

}  // namespace

// ---- The settings ----

// One setting: how save/settings.ini keeps it, and its row in the options
// (the tab, how it shows and how Left / Right change it). The table is the one
// place to put a new option; app.cpp then hands it to the game
// (options_from_settings) and back (settings_from_options).
struct Frontend::Setting {
    Id id;                    // its row (NO_ROW: only kept in the file)
    int tab = GAME_TAB;
    const wchar_t* label = L"";
    const char* key = nullptr;                        // in the file (nullptr: not kept)
    bool FrontendSettings::*flag = nullptr;           // what it sets: on / off,
    int FrontendSettings::*number = nullptr;          // a number
    std::string FrontendSettings::*path = nullptr;    // or a device path ("none" in the file when empty)
    // In the file a flag is 0 / 1 and a number is itself, unless there are
    // words: then false / true or 0, 1, 2... A word not among them reads as
    // `other`, or as one of `also` (older or looser spellings)
    std::vector<const char*> words = {};
    int other = 0;
    std::vector<std::pair<const char*, int>> also = {};
    int lo = 0, hi = 0;       // a number's range
    int grain = 1;            // and the steps it is read in (REAR LEVEL: 3 dB)
    bool hundredths = false;  // kept as a decimal (GAMMA 1.00)
    // The row
    Kind kind = PLAIN;
    std::vector<const wchar_t*> shown = {};   // CHOICE: each value's name; BUTTON: what it says
    std::vector<int> steps = {};              // CHOICE or SLIDER over these numbers
    int step = 1;                             // SLIDER: how far Left / Right go
    bool levels = false;                      // CHOICE: OFF, then higher and higher
    std::wstring (*text)(int) = nullptr;      // how the number reads (instead of `shown` or the number)
    const wchar_t* hint = L"";
    std::vector<const wchar_t*> hints = {};   // a hint for each value instead

    int get(const FrontendSettings& s) const { return flag ? int(s.*flag) : number ? s.*number : 0; }
    void set(FrontendSettings& s, int v) const {
        if (flag) s.*flag = v != 0;
        else if (number) s.*number = v;
    }
    // CHOICE: how many values, and which one is on
    int count() const { return int(steps.empty() ? shown.size() : steps.size()); }
    int pos(const FrontendSettings& s) const {
        if (steps.empty()) return get(s);
        return int(std::find(steps.begin(), steps.end(), get(s)) - steps.begin()) % count();
    }

    void read(FrontendSettings& s, const std::string& v) const {
        if (path) {
            s.*path = v == "none" ? "" : v;
        } else if (!words.empty()) {
            int n = other;
            auto w = std::find_if(words.begin(), words.end(), [&](const char* x) { return v == x; });
            if (w != words.end()) n = int(w - words.begin());
            else
                for (const auto& [word, value] : also)
                    if (v == word) n = value;
            set(s, n);
        } else if (flag) {
            s.*flag = v == "1" || v == "on" || v == "true";
        } else if (hundredths) {
            s.*number = std::clamp(int(atof(v.c_str()) * 100 + 0.5), lo, hi);
        } else {
            s.*number = std::clamp(atoi(v.c_str()) / grain * grain, lo, hi);
        }
    }
    std::string write(const FrontendSettings& s) const {
        if (path) return (s.*path).empty() ? "none" : s.*path;
        if (!words.empty()) return words[std::clamp(get(s), 0, int(words.size()) - 1)];
        char out[32];
        if (hundredths) snprintf(out, sizeof(out), "%.2f", get(s) / 100.0);
        else snprintf(out, sizeof(out), "%d", get(s));
        return out;
    }

    // Left (-1) or Right (+1) on the row; `wrap`: a click on the label, which
    // goes round past the top of a slider
    void change(FrontendSettings& s, int dir, bool wrap) const {
        int v = get(s);
        if (kind == TOGGLE) {
            set(s, !v);
        } else if (kind == CHOICE) {
            int p = (pos(s) + dir + count()) % count();
            set(s, steps.empty() ? p : steps[p]);
        } else if (kind == SLIDER && !steps.empty()) {   // the next step from the nearest one at or below, round
            int n = int(steps.size()), i = 0;
            while (i < n - 1 && steps[i + 1] <= v) i++;
            set(s, steps[(i + dir + n) % n]);
        } else if (kind == SLIDER) {
            set(s, dir > 0 && v >= hi && wrap ? lo : std::clamp(v + step * dir, lo, hi));
        }
    }
};

namespace {
std::wstring percent(int v) { return std::to_wstring(v) + L"%"; }
std::wstring decimal(int v) {   // GAMMA
    wchar_t text[16];
    swprintf(text, 16, L"%.2f", v / 100.0);
    return text;
}
std::wstring decibels(int v) { return (v > 0 ? L"+" : L"") + std::to_wstring(v) + L" DB"; }
std::wstring seconds(int v) { return v ? L"AFTER " + std::to_wstring(v) + L" S" : std::wstring(L"NEVER"); }

using FS = FrontendSettings;
const std::vector<const char*> LEVEL_WORDS = {"off", "low", "medium", "high"};
const std::vector<const wchar_t*> LEVELS = {L"OFF", L"LOW", L"MEDIUM", L"HIGH"};
const std::vector<std::pair<const char*, int>> ON_IS_2 = {{"1", 2}, {"on", 2}, {"true", 2}};   // once a switch

// The WEATHER screen before RANDOM SPAWNS and survival, in its order
struct WeatherChoice {
    const wchar_t *name, *hint;
    int weather;   // runtime/weather.h
};
const WeatherChoice WEATHER_CHOICES[] = {
    {L"RANDOM", L"A new weather every stage", WEATHER_RANDOM},
    {L"SUNNY", L"Clear sky and a strong sun", WEATHER_SUNNY},
    {L"CLOUDY", L"Grey sky, soft light and a light haze", WEATHER_CLOUDY},
    {L"FOG", L"Thick fog over the jungle", WEATHER_FOG},
    {L"RAIN", L"Rain all the way", WEATHER_RAIN},
    {L"STORM", L"Dark sky, heavy rain and lightning", WEATHER_STORM},
    {L"ORIGINAL", L"The arcade game's own weather", WEATHER_NONE},
};
constexpr int WEATHER_CHOICE_COUNT = int(std::size(WEATHER_CHOICES));
}  // namespace

// In each tab the rows are shown in this order
const std::vector<Frontend::Setting>& Frontend::setting_table() {
    static const std::vector<Setting> table = {
        // DISPLAY
        {.id = OPT_QUALITY, .tab = DISPLAY_TAB, .label = L"QUALITY", .kind = CHOICE,   // FrontendSettings::quality()
         .shown = {L"LOW", L"MEDIUM", L"HIGH", L"ULTRA", L"CUSTOM"},
         .hints = {L"Laptops and integrated graphics: 75% resolution, half rate, no MSAA, 2x anisotropic, square 2D pixels, original sky, no shadows, AO or surface detail",
                   L"Mid-range cards (RTX 3060, RX 6600): no ray tracing, half rate on fast monitors, MSAA 2x, 4x anisotropic",
                   L"Fast cards: ray traced sun shadows and every effect",
                   L"Everything on: ray traced sun shadows and ambient occlusion",
                   L"Your own mix of the options.  Pick a level to set the costly ones at once"}},
        {.id = OPT_DISPLAY, .tab = DISPLAY_TAB, .label = L"DISPLAY", .key = "fullscreen", .flag = &FS::fullscreen,
         .kind = CHOICE, .shown = {L"WINDOW", L"FULLSCREEN"}, .hint = L"Window or full screen (also F11 or Alt+Enter)"},
        {.id = OPT_ASPECT, .tab = DISPLAY_TAB, .label = L"WIDESCREEN", .key = "aspect", .number = &FS::aspect,
         .words = {"4:3", "16:9", "21:9"}, .other = 1, .kind = CHOICE, .shown = {L"4:3 (ORIGINAL)", L"16:9", L"21:9"},
         .hint = L"Widest 3D view: 21:9 fills ultrawide monitors (2560x1080, 3440x1440); 4:3 is the arcade's own"},
        {.id = OPT_SCALE, .tab = DISPLAY_TAB, .label = L"RENDER RESOLUTION", .key = "resolution_3d", .number = &FS::scale,
         .lo = FS::SCALES[0], .hi = 100, .kind = SLIDER, .steps = {std::begin(FS::SCALES), std::end(FS::SCALES)},
         .text = percent, .hint = L"Resolution of the 3D picture: lower is faster.  The HUD and the text stay sharp"},
        {.id = OPT_HALF_RATE, .tab = DISPLAY_TAB, .label = L"FRAME RATE", .key = "half_rate", .flag = &FS::half_rate,
         .kind = CHOICE, .shown = {L"FULL", L"HALF"},
         .hint = L"Half: above 100 Hz the 3D is drawn every other refresh (72 at 144 Hz); the crosshair keeps up"},
        {.id = OPT_VSYNC, .tab = DISPLAY_TAB, .label = L"VSYNC", .key = "vsync", .flag = &FS::vsync, .kind = TOGGLE,
         .hint = L"Off: each image goes out as soon as it is drawn, without waiting for the monitor (may tear)"},
        {.id = OPT_MOTION, .tab = DISPLAY_TAB, .label = L"MOTION", .key = "motion", .flag = &FS::smooth,
         .words = {"original", "smooth"}, .other = 1, .kind = CHOICE, .shown = {L"ORIGINAL", L"SMOOTH"},
         .hint = L"Smooth: the 3D is also drawn between the game's 57.5 frames, at your monitor's refresh rate"},
        {.id = OPT_AA, .tab = DISPLAY_TAB, .label = L"ANTI-ALIASING", .key = "aa", .number = &FS::aa,
         .words = {"msaa", "dlaa", "none"}, .also = {{"off", 2}}, .kind = CHOICE, .shown = {L"MSAA", L"DLAA", L"OFF"},
         .hint = L"Edge smoothing: MSAA (the samples below), NVIDIA DLAA or none.  DLAA applies on the next start"},
        {.id = OPT_MSAA, .tab = DISPLAY_TAB, .label = L"MSAA SAMPLES", .key = "msaa", .number = &FS::msaa, .lo = 2, .hi = 8,
         .kind = CHOICE, .shown = {L"2X", L"4X", L"8X"}, .steps = {2, 4, 8}, .levels = true,
         .hint = L"Samples per pixel of the edge smoothing: more is smoother and costs more, above all on integrated graphics"},
        {.id = OPT_ANISO, .tab = DISPLAY_TAB, .label = L"ANISOTROPIC FILTER", .key = "anisotropic", .number = &FS::aniso,
         .lo = 0, .hi = 16, .kind = CHOICE, .shown = {L"OFF", L"2X", L"4X", L"8X", L"16X"}, .steps = {0, 2, 4, 8, 16},
         .levels = true,
         .hint = L"Texture detail on the ground and at grazing angles, with the game's mipmaps.  OFF: the original look, no mipmaps"},
        {.id = OPT_FILTER_2D, .tab = DISPLAY_TAB, .label = L"2D FILTER", .key = "filter_2d", .flag = &FS::smooth_2d,
         .words = {"pixels", "smooth"}, .other = 1, .kind = CHOICE, .shown = {L"PIXELS", L"SMOOTH"},
         .hint = L"Smooth: the HUD and the 2D layers upscaled with xBR (edges redrawn); Pixels: square pixels, cheaper"},
        {.id = OPT_TEXT, .tab = DISPLAY_TAB, .label = L"HD TEXT", .key = "hd_text", .flag = &FS::hd_text, .kind = TOGGLE,
         .hint = L"Sharp fonts for menus, captions and score instead of the original pixels"},
        {.id = OPT_SAT, .tab = DISPLAY_TAB, .label = L"COLOR SATURATION", .key = "saturation", .number = &FS::saturation,
         .lo = 0, .hi = 200, .kind = SLIDER, .step = 10, .text = percent,
         .hint = L"How vivid the colours are (100% = original, 80% by default)"},
        {.id = OPT_GAMMA, .tab = DISPLAY_TAB, .label = L"GAMMA", .key = "gamma", .number = &FS::gamma, .lo = 50, .hi = 150,
         .hundredths = true, .kind = SLIDER, .step = 5, .text = decimal,
         .hint = L"Brightness of the mid-tones (1.00 = original)"},
        {.id = OPT_SHARPEN, .tab = DISPLAY_TAB, .label = L"SHARPEN", .key = "sharpen", .flag = &FS::sharpen, .kind = TOGGLE,
         .hint = L"Brings back texture detail after the edge smoothing, without halos"},
        // LIGHTING
        {.id = OPT_SUN, .tab = LIGHTING_TAB, .label = L"SUN", .key = "sun", .flag = &FS::sun, .kind = TOGGLE,
         .hint = L"A sun in the sky and stronger, warmer sunlight outdoors"},
        {.id = OPT_SHADOWS, .tab = LIGHTING_TAB, .label = L"SUN SHADOWS", .key = "shadows", .flag = &FS::shadows,
         .kind = TOGGLE, .hint = L"Dinosaurs, trees and buildings cast shadows away from the sun"},
        {.id = OPT_RT, .tab = LIGHTING_TAB, .label = L"RAY TRACING", .key = "ray_tracing", .number = &FS::ray_tracing,
         .words = {"off", "shadows", "ao"}, .other = 2, .also = {{"0", 0}}, .kind = CHOICE,
         .shown = {L"OFF", L"SHADOWS", L"SHADOWS + AO"}, .levels = true,
         .hints = {L"Shadows from a shadow map and occlusion from the depth, as without ray tracing",
                   L"Sun shadows traced through the world: every tree and dinosaur, soft with distance",
                   L"Also the ambient occlusion: corners and the ground under things, even off screen"}},
        {.id = OPT_SHAFTS, .tab = LIGHTING_TAB, .label = L"LIGHT SHAFTS", .key = "light_shafts", .flag = &FS::shafts,
         .kind = TOGGLE, .hint = L"Rays of sunlight streaming between trees and silhouettes"},
        {.id = OPT_HDR, .tab = LIGHTING_TAB, .label = L"HDR LIGHTING", .key = "hdr", .flag = &FS::hdr, .kind = TOGGLE,
         .hint = L"Bright light keeps its detail instead of clipping to white; exposure adapts gently"},
        {.id = OPT_AO, .tab = LIGHTING_TAB, .label = L"AMBIENT OCCLUSION", .key = "ao", .flag = &FS::ao, .kind = TOGGLE,
         .hint = L"Soft shadows in corners, creases and on the ground around things"},
        {.id = OPT_BLOOM, .tab = LIGHTING_TAB, .label = L"BLOOM", .key = "bloom", .number = &FS::bloom,
         .words = LEVEL_WORDS, .also = ON_IS_2, .kind = CHOICE, .shown = LEVELS, .levels = true,
         .hint = L"Soft glow around fire, lights, the sky and sunlit surfaces"},
        {.id = OPT_RIM, .tab = LIGHTING_TAB, .label = L"RIM LIGHT", .key = "rim_light", .flag = &FS::rim, .kind = TOGGLE,
         .hint = L"A soft edge of light around dinosaurs and people, lifting them off the background"},
        {.id = OPT_RELIEF, .tab = LIGHTING_TAB, .label = L"SURFACE DETAIL", .key = "surface_detail", .flag = &FS::relief,
         .kind = TOGGLE, .hint = L"Light and shadow bring out the grain of stone, bark, metal and skin"},
        {.id = OPT_TORCH, .tab = LIGHTING_TAB, .label = L"FLASHLIGHT", .key = "flashlight", .flag = &FS::torch,
         .words = {"original", "modern"}, .other = 1, .kind = CHOICE, .shown = {L"ORIGINAL", L"MODERN"},
         .hint = L"At night: a soft beam with real shading and a glow in the air, or the original spot"},
        // ATMOSPHERE
        {.id = OPT_SKY, .tab = ATMOSPHERE_TAB, .label = L"SKY", .key = "sky", .flag = &FS::sky,
         .words = {"original", "procedural"}, .kind = CHOICE, .shown = {L"ORIGINAL", L"PROCEDURAL"},
         .hint = L"Smooth drifting clouds drawn at full resolution, or the original picture"},
        {.id = OPT_HAZE, .tab = ATMOSPHERE_TAB, .label = L"DISTANCE HAZE", .key = "haze", .flag = &FS::haze, .kind = TOGGLE,
         .hint = L"Far scenery fades into the sky instead of appearing suddenly"},
        {.id = OPT_LIGHTNING, .tab = ATMOSPHERE_TAB, .label = L"LIGHTNING", .key = "lightning", .flag = &FS::lightning,
         .kind = TOGGLE, .hint = L"Flashes of lightning in stormy fog, with the trees standing out against it"},
        {.id = OPT_RAIN, .tab = ATMOSPHERE_TAB, .label = L"RAIN", .key = "rain", .number = &FS::rain,
         .words = {"off", "light", "heavy"}, .also = ON_IS_2, .kind = CHOICE, .shown = {L"OFF", L"LIGHT", L"HEAVY"},
         .levels = true, .hint = L"Where it rains in the game: a dense fall of drops, wet ground, puddles and ripples"},
        {.id = OPT_SOFT, .tab = ATMOSPHERE_TAB, .label = L"SOFT PARTICLES", .key = "soft_particles", .flag = &FS::soft,
         .kind = TOGGLE, .hint = L"Smoke, fire and explosions blend softly where they touch the ground and walls"},
        // AUDIO
        {.id = OPT_VOLUME, .tab = AUDIO_TAB, .label = L"VOLUME", .key = "volume", .number = &FS::volume, .lo = 0, .hi = 10,
         .kind = SLIDER, .hint = L"Sound volume"},
        {.id = OPT_MUSIC, .tab = AUDIO_TAB, .label = L"MENU MUSIC", .key = "menu_music", .flag = &FS::menu_music,
         .kind = TOGGLE, .hint = L"Music in the menus (third_party/menu/music.mp3), at the sound volume"},
        {.id = OPT_SPEAKERS, .tab = AUDIO_TAB, .label = L"SPEAKERS", .key = "speakers", .number = &FS::speakers,
         .words = {"stereo", "surround", "headphones"}, .kind = CHOICE,
         .shown = {L"STEREO", L"SURROUND 4.0", L"HEADPHONES"},
         .hints = {L"Front and rear sound mixed on two speakers",
                   L"Rear sound on your rear speakers, as on the cabinet (4.0, 5.1 or 7.1)",
                   L"Two ears: gentle crossfeed, and the rear sound placed behind you"}},
        {.id = OPT_REAR, .tab = AUDIO_TAB, .label = L"REAR LEVEL", .key = "rear_boost", .number = &FS::rear_boost, .lo = -6,
         .hi = 12, .grain = 3, .kind = CHOICE, .steps = {-6, -3, 0, 3, 6, 9, 12}, .text = decibels,
         .hint = L"Rear speakers quieter or louder, as the cabinet's seat speakers: T-Rex roars and many effects"},
        {.id = OPT_WEATHER_SOUND, .tab = AUDIO_TAB, .label = L"WEATHER SOUND", .key = "weather_sound",
         .number = &FS::weather_sound, .words = LEVEL_WORDS, .also = ON_IS_2, .kind = CHOICE, .shown = LEVELS,
         .levels = true, .hint = L"Rain and thunder when the weather brings them, and thunder after every lightning flash"},
        {.id = OPT_SAMPLES, .tab = AUDIO_TAB, .label = L"SAMPLE QUALITY", .key = "sample_interpolation",
         .flag = &FS::cubic_samples, .words = {"linear", "cubic"}, .other = 1, .kind = CHOICE,
         .shown = {L"ORIGINAL", L"SMOOTH"},
         .hint = L"Smooth interpolation of the instrument and voice samples, or the original chip's"},
        // CONTROLS
        {.id = OPT_PAD_SPEED, .tab = CONTROLS_TAB, .label = L"STICK SPEED", .key = "pad_speed", .number = &FS::pad_speed,
         .lo = 1, .hi = 10, .kind = SLIDER, .hint = L"How fast the gamepad's sticks move the sight.  "},   // + the pads
        {.id = OPT_GYRO, .tab = CONTROLS_TAB, .label = L"GYRO AIM", .key = "gyro", .flag = &FS::gyro, .kind = TOGGLE,
         .hint = L"Point the gamepad to aim, together with the sticks"},   // option_items: without a gyroscope
        {.id = OPT_GYRO_SPEED, .tab = CONTROLS_TAB, .label = L"GYRO SPEED", .key = "gyro_speed", .number = &FS::gyro_speed,
         .lo = 1, .hi = 10, .kind = SLIDER, .hint = L"How far the sight moves when you turn the gamepad"},
        {.id = OPT_VIBRATION, .tab = CONTROLS_TAB, .label = L"VIBRATION", .key = "vibration", .number = &FS::vibration,
         .words = LEVEL_WORDS, .kind = CHOICE, .shown = LEVELS, .levels = true,
         .hint = L"The gamepad rumbles when you shoot and when you are hit"},
        {.id = OPT_ASSIST, .tab = CONTROLS_TAB, .label = L"AIM ASSIST", .key = "aim_assist", .number = &FS::aim_assist,
         .words = LEVEL_WORDS, .kind = CHOICE, .shown = LEVELS, .levels = true,
         .hint = L"With a gamepad the sight slows down over the dinosaurs and follows them a little"},
        {.id = OPT_CROSS, .tab = CONTROLS_TAB, .label = L"CROSSHAIR", .key = "crosshair", .flag = &FS::crosshair,
         .kind = TOGGLE, .hint = L"Extra crosshair where the mouse points"},
        {.id = OPT_CROSS_STYLE, .tab = CONTROLS_TAB, .label = L"CROSSHAIR STYLE", .key = "crosshair_style",
         .number = &FS::crosshair_style, .words = {"default", "custom"}, .other = 1, .kind = CHOICE,
         .shown = {L"DEFAULT", L"CUSTOM"},
         .hint = L"The drawn rings, or your own pictures (crosshair.png and crosshair_p2.png beside the game)"},
        {.id = OPT_CROSS_P1, .tab = CONTROLS_TAB, .label = L"PLAYER 1 PICTURE", .kind = BUTTON, .shown = {L"CHOOSE PNG..."},
         .hint = L"Pick a PNG for player 1's crosshair.  It is copied beside the game as crosshair.png, at its own size in pixels at 1080p"},
        {.id = OPT_CROSS_P2, .tab = CONTROLS_TAB, .label = L"PLAYER 2 PICTURE", .kind = BUTTON, .shown = {L"CHOOSE PNG..."},
         .hint = L"Pick a PNG for player 2's crosshair (crosshair_p2.png).  Without one, player 2 uses player 1's picture"},
        {.id = OPT_CROSS_HIDE, .tab = CONTROLS_TAB, .label = L"HIDE IDLE CROSSHAIR", .key = "crosshair_hide",
         .number = &FS::crosshair_hide, .lo = 0, .hi = 10, .kind = CHOICE, .steps = {0, 3, 5, 10}, .text = seconds,
         .hint = L"A crosshair whose gun has not moved or fired for a while disappears until it moves again"},
        {.id = OPT_MICE, .tab = CONTROLS_TAB, .label = L"SECOND MOUSE", .key = "second_mouse", .flag = &FS::second_mouse,
         .kind = CHOICE, .shown = {L"OFF", L"PLAYER 2"},
         .hint = L"A second mouse joins as player 2 with a click while player 1 plays"},
        {.id = OPT_GUN_TEST, .tab = CONTROLS_TAB, .label = L"GUN TEST", .kind = BUTTON, .shown = {L"OPEN..."},
         .hint = L"Every mouse and light gun Windows sees, what each one is doing and which player it is.  "
                 L"Fire each gun to make it player 1's or player 2's for good"},
        // GAME
        {.id = OPT_FREE_PLAY, .tab = GAME_TAB, .label = L"FREE PLAY", .key = "free_play", .flag = &FS::free_play,
         .kind = TOGGLE, .hint = L"Start and continue with 1 / 2 or a light gun's start button, no coin needed"},
        {.id = OPT_SUBS, .tab = GAME_TAB, .label = L"SUBTITLES", .key = "subtitles", .number = &FS::subtitles,
         .words = {"off", "english", "japanese"}, .kind = CHOICE, .shown = {L"OFF", L"ENGLISH", L"JAPANESE"},
         .hint = L"Captions of what the characters say"},
        // Only in the file: the WEATHER last picked before a run (in WEATHER_CHOICES' order)
        {.id = NO_ROW, .key = "run_weather", .number = &FS::run_weather,
         .words = {"random", "sunny", "cloudy", "fog", "rain", "storm", "original"}},
        // and the guns picked in GUN TEST
        {.id = NO_ROW, .key = "gun_p1", .path = &FS::gun_p1},
        {.id = NO_ROW, .key = "gun_p2", .path = &FS::gun_p2},
    };
    return table;
}

const Frontend::Setting* Frontend::setting_of(Id id) {
    for (const Setting& d : setting_table())
        if (d.id == id) return &d;
    return nullptr;
}

// ---- Settings file ----

void FrontendSettings::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char key[64], value[400];   // device paths are long
        if (sscanf(line, " %63[^= ] = %399s", key, value) != 2) continue;
        for (const Frontend::Setting& d : Frontend::setting_table())
            if (d.key && strcmp(d.key, key) == 0) d.read(*this, value);
    }
    fclose(f);
}

void FrontendSettings::save(const char* path) const {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (const Frontend::Setting& d : Frontend::setting_table())
        if (d.key) fprintf(f, "%s = %s\n", d.key, d.write(*this).c_str());
    fclose(f);
}

// ---- Setup ----

bool Frontend::init(const std::string& font_dir) {
    gdi_ = &GdiText::acquire(font_dir);
    bool ok = gdi_->ready();
    for (GdiText::Face f : {GdiText::SAIRA, GdiText::BARLOW, GdiText::MONO})   // the others are optional
        if (!gdi_->loaded(f)) {
            fprintf(stderr, "[menu] could not load the font %s/%s\n", font_dir.c_str(), GdiText::file(f));
            ok = false;
        }
    jurassic_ = gdi_->loaded(GdiText::JURASSIC);
    for (const wchar_t* p : {L"third_party/menu/cover.png", L"third_party/menu/cover.jpg", L"third_party/menu/lw_poster.jpg"})
        if (load_poster(p)) {
            printf("[menu] poster %ls (%dx%d)\n", p, poster_w_, poster_h_);
            break;
        }
    load_image(L"third_party/menu/sega.png", sega_.px, sega_.w, sega_.h);
    // logo and background: your own in third_party/menu/ first, else the ones in res/menu/
    for (const wchar_t* p : {L"third_party/menu/logo.png", L"res/menu/logo.png"})
        if (load_image(p, logo_.px, logo_.w, logo_.h)) {
            printf("[menu] logo %ls (%dx%d)\n", p, logo_.w, logo_.h);
            break;
        }
    for (const wchar_t* p : {L"third_party/menu/background.png", L"third_party/menu/background.jpg", L"res/menu/background.png"})
        if (load_image(p, backdrop_.px, backdrop_.w, backdrop_.h)) {
            printf("[menu] background %ls (%dx%d)\n", p, backdrop_.w, backdrop_.h);
            break;
        }
    return ok;
}

namespace {

// Any picture format Windows can read (WIC), as BGRA (nothing when it fails)
bool load_image(const std::wstring& path, std::vector<uint32_t>& px, int& w, int& h) {
    if (image_load_bgra(path, w, h, px)) return true;
    px.clear();
    w = h = 0;
    return false;
}

}  // namespace

bool Frontend::load_poster(const std::wstring& path) { return load_image(path, poster_, poster_w_, poster_h_); }

void Frontend::shutdown() {
    if (gdi_) gdi_->release();
    gdi_ = nullptr;
}

// The picture of an option, if there is one
const Frontend::Picture& Frontend::preview(Id id) {
    auto it = previews_.find(int(id));
    if (it != previews_.end()) return it->second;
    static const struct { Id id; const wchar_t* name; } FILES[] = {
        {OPT_MOTION, L"motion"}, {OPT_AA, L"anti_aliasing"}, {OPT_HAZE, L"haze"}, {OPT_BLOOM, L"bloom"},
        {OPT_AO, L"ambient_occlusion"}, {OPT_SKY, L"sky"}, {OPT_SUN, L"sun"}, {OPT_SHADOWS, L"sun_shadows"},
        {OPT_RT, L"ray_tracing"}, {OPT_SHAFTS, L"light_shafts"}, {OPT_SOFT, L"soft_particles"}, {OPT_HDR, L"hdr"},
        {OPT_RIM, L"rim_light"}, {OPT_RELIEF, L"surface_detail"}, {OPT_TORCH, L"flashlight"},
        {OPT_LIGHTNING, L"lightning"}, {OPT_RAIN, L"rain"}, {OPT_SAT, L"saturation"}, {OPT_SHARPEN, L"sharpen"},
        {OPT_TEXT, L"hd_text"}, {OPT_CROSS, L"crosshair_backdrop"}, {OPT_SUBS, L"subtitles"}};
    Picture& p = previews_[int(id)];
    for (const auto& f : FILES)
        if (f.id == id)
            for (const wchar_t* ext : {L".jpg", L".png"})
                if (load_image(L"res/preview/" + std::wstring(f.name) + ext, p.px, p.w, p.h)) break;
    return p;
}

const Frontend::Picture& Frontend::pad_icon(const std::wstring& button) {
    auto it = pad_icons_.find(button);
    if (it != pad_icons_.end()) return it->second;
    std::wstring name = button;
    for (wchar_t& c : name) c = towlower(c);
    Picture& p = pad_icons_[button];
    load_image(L"res/pad/" + name + L".png", p.px, p.w, p.h);
    return p;
}

// ---- Menu contents and navigation ----

const wchar_t* const Frontend::TAB_NAMES[Frontend::TAB_COUNT] = {L"DISPLAY", L"LIGHTING", L"ATMOSPHERE", L"AUDIO",
                                                                  L"CONTROLS", L"GAME"};

const wchar_t* const Frontend::RECORD_TAB_NAMES[Frontend::RECORD_TAB_COUNT] = {L"ACHIEVEMENTS", L"STATS"};

void Frontend::set_tab(int t) {
    bool on_back = selected_ > 0 && selected_ == int(items().size()) - 1;
    tab_ = (t + tab_count()) % tab_count();
    scroll_ = 0;
    record_scroll_ = 0;
    int n = int(items().size());   // on an option: the same row, or the last one of a shorter tab
    selected_ = on_back ? n - 1 : std::min(selected_, std::max(0, n - 2));
    hovered_ = -1;
    dirty_ = true;
}

std::vector<Frontend::Item> Frontend::option_items(const FrontendSettings& s) const {
    std::vector<Item> all;
    for (const Setting& d : setting_table()) {
        if (d.id == NO_ROW) continue;
        Item it{d.id, d.label, L"", d.hint};
        it.kind = d.kind;
        int v = d.get(s);
        switch (d.kind) {
        case TOGGLE: it.value = v ? L"ON" : L"OFF", it.pos = v ? 1 : 0, it.count = 2; break;
        case CHOICE:
            it.pos = d.pos(s), it.count = d.count(), it.levels = d.levels;
            it.value = d.text ? d.text(v) : d.shown[it.pos];
            break;
        case SLIDER:
            it.frac = float(v - d.lo) / float(d.hi - d.lo);
            it.value = d.text ? d.text(v) : std::to_wstring(v);
            break;
        case BUTTON: it.value = d.shown[0]; break;
        default: break;
        }
        if (!d.hints.empty()) it.hint = d.hints[it.pos];
        all.push_back(it);
    }
    // What also depends on the rest of the settings and on this computer
    for (Item& it : all) switch (it.id) {
        case OPT_QUALITY: {   // a dot per level, none lit for a mix of them
            int q = s.quality();
            const Setting& d = *setting_of(OPT_QUALITY);
            it.value = d.shown[q], it.hint = d.hints[q];
            it.pos = q == FrontendSettings::CUSTOM ? -1 : q, it.count = 4;
            break;
        }
        case OPT_AA:   // MSAA and off change at once; DLAA starts (or stops) with the game
            if ((s.aa == 1) != (aa_at_launch == 1)) it.hint = L"Edge smoothing.  DLAA takes effect the next time the game starts";
            break;
        case OPT_MSAA:
            if (s.aa != 0) {
                it.enabled = false;
                it.hint = L"Only with ANTI-ALIASING: MSAA";
            }
            break;
        case OPT_RT:
            if (!rt_available) {
                it.value = L"N/A";
                it.hint = L"Needs a graphics card with ray tracing (NVIDIA RTX, AMD RX 6000 or newer)";
                it.enabled = false;
            }
            break;
        case OPT_PAD_SPEED: it.hint += gamepads.empty() ? L"No gamepad connected" : L"Connected: " + gamepads; break;
        case OPT_GYRO:
            if (!gyro_pad) it.hint = L"Point the gamepad to aim (DualSense, DualShock 4, Switch Pro).  None connected";
            break;
        default: break;
        }
    return all;
}

const std::wstring& Frontend::default_value(int id) {
    if (defaults_.empty() || defaults_rt_ != rt_available) {
        defaults_ = option_items(FrontendSettings{});
        defaults_rt_ = rt_available;
    }
    static const std::wstring none;
    for (const Item& d : defaults_)
        if (d.id == id) return d.value;
    return none;
}

void Frontend::reset_tab() {
    const FrontendSettings d;
    for (const Item& it : items())
        if (const Setting* row = setting_of(it.id); row && it.id != OPT_DISPLAY)   // the window stays as it is
            row->set(settings, row->get(d));                                     // (QUALITY follows the others)
    push(Action::SettingsChanged);
    dirty_ = true;
}

std::vector<Frontend::Item> Frontend::items() const {
    switch (screen_) {
    case Screen::Main:
        return {{PLAY, L"PLAY", L"", L"Start a game"},
                {OPTIONS, L"OPTIONS", L"", L"Display, sound and graphics"},
                {RECORDS, L"RECORDS", L"", L"Achievements and your statistics"},
                {QUIT, L"QUIT", L"", L"Close the game"}};
    case Screen::Play:
        return {{CLASSIC, L"CLASSIC ARCADE", L"", L"The arcade game: as it was, or with the dinosaurs shuffled"},
                {BOSS, L"BOSS FIGHT", L"", L"Pick a boss and go straight to the fight"},
                {SURVIVAL, L"SURVIVAL", L"", L"Hold one spot against endless waves of dinosaurs"},
                {BACK, L"BACK", L"", L"Return to the main menu"}};
    case Screen::Classic:
        return {{ARCADE, L"ARCADE", L"", L"The original arcade game.  Insert a coin with 5 and press 1, or START on a gamepad"},
                {RANDOM, L"RANDOM SPAWNS", L"",
                 L"Dinosaurs come from new places and at new times, and every boss takes a different number of shots.  "
                 L"Different every game"},
                {BACK, L"BACK", L"", L"Return to the mode select"}};
    case Screen::Weather: {
        std::vector<Item> list;
        for (int i = 0; i < WEATHER_CHOICE_COUNT; i++) {
            std::wstring hint = WEATHER_CHOICES[i].hint;
            if (weather_for_survival_ && WEATHER_CHOICES[i].weather == WEATHER_RANDOM) hint = L"A new weather every run";
            if (WEATHER_CHOICES[i].weather != WEATHER_NONE)
                hint += L".  Outdoors in daylight; caves and the night keep the game's own";
            list.push_back({WEATHER_ENTRY, WEATHER_CHOICES[i].name, L"", hint, true, i});
        }
        list.push_back({BACK, L"BACK", L"", weather_for_survival_ ? L"Return to the arenas" : L"Return to CLASSIC ARCADE"});
        return list;
    }
    case Screen::Pause:
        if (boss_mode)
            return {{RESUME, L"RESUME", L"", L"Back to the fight"},
                    {RETRY, L"RETRY FIGHT", L"", L"Start this boss fight again"},
                    {BOSS_LIST, L"BOSS LIST", L"", L"Pick another boss, or go back to the main menu"},
                    {SETTINGS, L"SETTINGS", L"", L"Display, sound and graphics"},
                    {QUIT_TO_MENU, L"QUIT TO MAIN MENU", L"", L"Leave this game and go back to the main menu"}};
        return {{RESUME, L"RESUME", L"", L"Back to the game"},
                {SETTINGS, L"SETTINGS", L"", L"Display, sound and graphics"},
                {QUIT_TO_MENU, L"QUIT TO MAIN MENU", L"", L"Leave this game and go back to the main menu"}};
    case Screen::Boss: {
        std::vector<Item> list;
        bool failed = false;
        for (int i = 0; i < BOSS_COUNT; i++) {
            int st = i < int(boss_status.size()) ? boss_status[i] : BOSS_MISSING;
            failed |= st == BOSS_FAILED;
            std::wstring stage = BOSSES[i].stage;
            Item it{BOSS_ENTRY, BOSSES[i].name, stage, stage + L".  Straight to the fight", st == BOSS_READY, i};
            if (st == BOSS_PREPARING) {
                it.value = L"PREPARING";
                it.hint = L"Playing up to this boss by itself in the background.  A minute or two, only the first time";
            } else if (st == BOSS_FAILED) {
                it.value = L"NOT READY";
                it.hint = L"The game could not reach this boss by itself.  PREPARE AGAIN tries once more";
            } else if (st == BOSS_MISSING) {
                it.value = L"NOT READY";
                it.hint = L"Waiting to be prepared";
            }
            list.push_back(it);
        }
        if (failed) list.push_back({PREPARE, L"PREPARE AGAIN", L"", L"Play up to the missing bosses again, in the background"});
        if (boss_over_game_) list.push_back({LEAVE_BOSS, L"MAIN MENU", L"", L"Leave boss mode"});
        else list.push_back({BACK, L"BACK", L"", L"Return to the mode select"});
        return list;
    }
    case Screen::Survival: {
        std::vector<Item> list;
        for (int i = 0; i < SURVIVAL_ZONE_COUNT; i++) {
            const SurvivalZone& z = SURVIVAL_ZONES[i];
            std::wstring where = L"Stage " + std::to_wstring(z.stage) + L", zone " + std::to_wstring(z.zone);
            int wave = 0, score = 0;
            bool rec = survival_record(z.stage, z.zone, wave, score);
            bool open = survival_unlocked(z.stage, z.zone);
            std::wstring value = !open ? L"LOCKED" : rec ? L"WAVE " + std::to_wstring(wave) : L"NEW";
            std::wstring hint = where + (!open ? L".  Reach it in CLASSIC ARCADE to unlock it"
                                         : rec ? L".  Your record: wave " + std::to_wstring(wave) + L", " +
                                                     std::to_wstring(score) + L" points"
                                               : L".  No record yet");
            list.push_back({SURVIVAL_ENTRY, z.name, value, hint, open, i});
        }
        list.push_back({BACK, L"BACK", L"", L"Return to the mode select"});
        return list;
    }
    case Screen::Options: {
        std::vector<Item> list = {{TABS, L"", L"", L"Left / Right here, Q / E or L1 / R1 change the tab"}};
        for (const Item& it : option_items(settings))   // the tab's options, in the table's order
            if (setting_of(it.id)->tab == tab_) list.push_back(it);
        list.push_back(
            {BACK, L"BACK", L"", options_from_pause_ ? L"Return to the pause menu" : L"Return to the main menu"});
        return list;
    }
    case Screen::Records: {
        int got = 0;
        for (int64_t t : records.unlocked) got += t != 0;
        std::wstring summary = std::to_wstring(got) + L" of " + std::to_wstring(achievement_table().size()) +
                               L" achievements unlocked.  Up / Down scrolls, Q / E changes the tab";
        return {{TABS, L"", L"", summary}, {BACK, L"BACK", L"", L"Return to the main menu"}};
    }
    default:
        return {};
    }
}

void Frontend::open(Screen s) {
    locked_notice_ = false;
    screen_ = s;
    selected_ = 0;
    coffee_focus_ = false;
    scroll_ = 0;
    hovered_ = -1;
    hit_.clear();
    dirty_ = true;
}

void Frontend::open_boss_list(bool over_game) {
    boss_over_game_ = over_game;
    open(Screen::Boss);
    auto ready = [&](int i) { return i < int(boss_status.size()) && boss_status[i] == BOSS_READY; };
    if (ready(boss_choice)) selected_ = boss_choice;   // onto the last fight, or else the first one ready
    else
        for (int i = BOSS_COUNT - 1; i >= 0; i--)
            if (ready(i)) selected_ = i;
}

int Frontend::run_weather() const {
    return WEATHER_CHOICES[std::clamp(settings.run_weather, 0, WEATHER_CHOICE_COUNT - 1)].weather;
}

void Frontend::select(int index) {
    if (index != selected_) dirty_ = true;
    selected_ = index;
}

void Frontend::activate(const Item& it, int dir) {
    dirty_ = true;
    switch (it.id) {
    case PLAY: open(Screen::Play); break;
    case OPTIONS: options_from_pause_ = false; tab_ = 0; open(Screen::Options); selected_ = 1; break;
    case RECORDS: tab_ = 0; record_scroll_ = 0; open(Screen::Records); break;
    case QUIT: push(Action::Quit); break;
    case QUIT_TO_MENU: push(Action::QuitToMenu); break;
    case CLASSIC: open(Screen::Classic); selected_ = random_spawns ? 1 : 0; break;
    case ARCADE: random_spawns = false; push(Action::StartArcade); break;
    case RANDOM:   // the weather first
        random_spawns = true;
        weather_for_survival_ = false;
        open(Screen::Weather);
        selected_ = std::clamp(settings.run_weather, 0, WEATHER_CHOICE_COUNT - 1);
        break;
    case BOSS: open_boss_list(false); break;
    case SURVIVAL: {
        open(Screen::Survival);
        selected_ = survival_choice;
        bool any = false;   // none unlocked yet: the notice says how
        for (int i = 0; i < SURVIVAL_ZONE_COUNT; i++) any |= survival_unlocked(SURVIVAL_ZONES[i].stage, SURVIVAL_ZONES[i].zone);
        locked_notice_ = !any;
        break;
    }
    case SURVIVAL_ENTRY:   // the weather first
        survival_choice = it.index;
        weather_for_survival_ = true;
        open(Screen::Weather);
        selected_ = std::clamp(settings.run_weather, 0, WEATHER_CHOICE_COUNT - 1);
        break;
    case WEATHER_ENTRY:   // kept for next time
        settings.run_weather = it.index;
        push(Action::SettingsChanged);
        push(weather_for_survival_ ? Action::StartSurvival : Action::StartArcade);
        break;
    case BOSS_ENTRY: boss_choice = it.index; push(Action::StartBoss); break;
    case PREPARE: push(Action::PrepareBosses); break;
    case RETRY: push(Action::StartBoss); break;
    case BOSS_LIST: open_boss_list(true); break;
    case LEAVE_BOSS: push(Action::LeaveBossMode); break;
    case BACK: back(); break;
    case RESUME: push(Action::Resume); break;
    case SETTINGS: options_from_pause_ = true; tab_ = 0; open(Screen::Options); selected_ = 1; break;
    case OPT_DISPLAY: push(Action::ToggleFullscreen); break;
    case OPT_QUALITY: {
        int q = settings.quality();
        settings.set_quality(q == FrontendSettings::CUSTOM ? (dir > 0 ? FrontendSettings::LOW : FrontendSettings::ULTRA)
                                                           : (q + dir + 4) % 4);
        push(Action::SettingsChanged);
        break;
    }
    case OPT_CROSS_P1:
    case OPT_CROSS_P2:   // the Windows file dialog; a picture picked is also shown from then on
        if (crosshair_pick(it.id == OPT_CROSS_P2 ? 1 : 0)) {
            crosshair_loaded_ = false;
            push(Action::CrosshairChanged);
            if (!settings.crosshair_style) {
                settings.crosshair_style = 1;
                push(Action::SettingsChanged);
            }
        }
        break;
    case OPT_GUN_TEST: open(Screen::GunTest); gun_step_ = 0; break;
    default:   // the other options: as the table says
        if (const Setting* d = setting_of(it.id)) {
            d->change(settings, dir, wrap_);
            push(Action::SettingsChanged);
        }
        break;
    }
}

void Frontend::back() {
    switch (screen_) {
    case Screen::Play: open(Screen::Main); break;
    case Screen::Options:
        open(options_from_pause_ ? Screen::Pause : Screen::Main);
        selected_ = options_from_pause_ && boss_mode ? 3 : 1;   // where the options were opened from
        break;
    case Screen::Boss:
        if (boss_over_game_) {
            open(Screen::Pause);
            selected_ = boss_mode ? 2 : 0;   // onto Boss list
        } else {
            open(Screen::Play);
            selected_ = 1;   // onto Boss fight
        }
        break;
    case Screen::Records: open(Screen::Main); selected_ = 2; break;   // onto Records
    case Screen::Survival: open(Screen::Play); selected_ = 2; break;   // onto Survival
    case Screen::Classic: open(Screen::Play); selected_ = 0; break;    // onto Classic arcade
    case Screen::Weather:   // onto the arena, or RANDOM SPAWNS
        if (weather_for_survival_) {
            open(Screen::Survival);
            selected_ = survival_choice;
        } else {
            open(Screen::Classic);
            selected_ = 1;
        }
        break;
    case Screen::Pause: push(Action::Resume); break;
    case Screen::Main: selected_ = 3; dirty_ = true; break;   // onto Quit
    case Screen::GunTest: {   // onto its row in CONTROLS
        tab_ = CONTROLS_TAB;
        open(Screen::Options);
        auto list = items();
        for (int i = 0; i < int(list.size()); i++)
            if (list[i].id == OPT_GUN_TEST) selected_ = i;
        break;
    }
    default: break;
    }
}

bool Frontend::key(int vk) {
    if (!active()) return false;
    if (locked_notice_) {   // any of these closes it
        if (vk != VK_RETURN && vk != VK_SPACE && vk != VK_ESCAPE && vk != VK_BACK) return true;
        locked_notice_ = false;
        dirty_ = true;
        return true;
    }
    if (screen_ == Screen::GunTest) {
        switch (vk) {
        case VK_RETURN: case VK_SPACE:   // pick the guns again, player 1's first
            if (!gun_step_) gun_step_ = 1;
            break;
        case 'R': case VK_DELETE:   // back to the first and second gun to fire
            if (gun_step_ || (settings.gun_p1.empty() && settings.gun_p2.empty())) break;
            settings.gun_p1.clear(), settings.gun_p2.clear();
            push(Action::SettingsChanged);
            break;
        case VK_ESCAPE: case VK_BACK:
            if (gun_step_ == 2) push(Action::SettingsChanged);   // only player 1's gun
            if (gun_step_) gun_step_ = 0;
            else back();
            break;
        default: return false;
        }
        dirty_ = true;
        return true;
    }
    auto list = items();
    int n = int(list.size());
    bool option_row = selected_ < n && list[selected_].id >= OPT_DISPLAY;
    bool tabs_row = selected_ < n && list[selected_].id == TABS;
    if (screen_ == Screen::Records) switch (vk) {   // two tabs and a list to scroll
        case 'Q': case VK_PRIOR: case VK_LEFT: case 'A': set_tab(tab_ - 1); return true;
        case 'E': case VK_NEXT: case VK_RIGHT: case 'D': case VK_TAB: set_tab(tab_ + 1); return true;
        case VK_UP: case 'W': scroll_records(-1); return true;
        case VK_DOWN: case 'S': scroll_records(+1); return true;
        case VK_RETURN: case VK_SPACE: case VK_ESCAPE: case VK_BACK: back(); return true;
        default: return false;
        }
    if (screen_ == Screen::Options) switch (vk) {   // tab keys
        case 'Q': case VK_PRIOR: set_tab(tab_ - 1); return true;
        case 'E': case VK_NEXT: set_tab(tab_ + 1); return true;
        case VK_TAB: set_tab(tab_ + ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)); return true;
        case 'R': reset_tab(); return true;
        }
    if (coffee_focus_) switch (vk) {   // the donation link at the top: Enter opens it, Up / Down go back to the list
        case VK_RETURN: case VK_SPACE: open_coffee(); return true;
        case VK_UP: case 'W': coffee_focus_ = false; dirty_ = true; select(n - 1); return true;
        case VK_DOWN: case 'S': coffee_focus_ = false; dirty_ = true; select(0); return true;
        case VK_LEFT: case 'A': case VK_RIGHT: case 'D': return true;
        }
    switch (vk) {
    case VK_UP: case 'W':
        if (screen_ == Screen::Main && !over_game() && selected_ == 0) {   // above the first entry: the donation link
            coffee_focus_ = dirty_ = true;
            return true;
        }
        select((selected_ + n - 1) % n);
        return true;
    case VK_DOWN: case 'S': select((selected_ + 1) % n); return true;
    case VK_LEFT: case 'A':
        if (tabs_row) set_tab(tab_ - 1);
        else if (option_row && list[selected_].kind != BUTTON) activate(list[selected_], -1);
        return true;
    case VK_RIGHT: case 'D':
        if (tabs_row) set_tab(tab_ + 1);
        else if (option_row && list[selected_].kind != BUTTON) activate(list[selected_], +1);
        return true;
    case VK_RETURN: case VK_SPACE:
        hovered_ = -1;
        if (selected_ < n && list[selected_].enabled) activate(list[selected_], +1);
        return true;
    case VK_ESCAPE: case VK_BACK: back(); return true;
    }
    return false;
}

void Frontend::open_coffee() {   // in the default browser
    ShellExecuteW(nullptr, L"open", COFFEE_URL, nullptr, nullptr, SW_SHOWNORMAL);
}

void Frontend::mouse_move(int x, int y) {
    mouse_x_ = x;
    mouse_y_ = y;
    int hit = -1;
    for (size_t i = 0; i < hit_.size(); i++) {
        POINT p{x, y};
        if (PtInRect(&hit_[i], p)) hit = int(i);
    }
    if (hit >= 0) {
        if (coffee_focus_) dirty_ = true;
        coffee_focus_ = false;
        select(hit);
    }
    hovered_ = hit;
    bool on_coffee = PtInRect(&coffee_rect_, POINT{x, y}) != 0;
    if (on_coffee != coffee_hot_) dirty_ = true;
    coffee_hot_ = on_coffee;
}

void Frontend::click(int x, int y) {
    POINT p{x, y};
    if (locked_notice_) {
        locked_notice_ = false;
        dirty_ = true;
        return;
    }
    if (screen_ == Screen::Main && PtInRect(&coffee_rect_, p)) {
        open_coffee();
        return;
    }
    if (screen_ == Screen::GunTest) {   // the guns fire at it: only BACK, and not while picking them
        if (!gun_step_ && PtInRect(&back_rect_, p)) back();
        return;
    }
    if (screen_ == Screen::Options && PtInRect(&reset_rect_, p)) {
        reset_tab();
        return;
    }
    if (screen_ != Screen::Options && screen_ != Screen::Records && PtInRect(&back_rect_, p)) {
        back();
        return;
    }
    if (screen_ == Screen::Records && (PtInRect(&more_up_, p) || PtInRect(&more_down_, p))) {
        scroll_records(PtInRect(&more_up_, p) ? -1 : 1);
        return;
    }
    if (PtInRect(&more_up_, p) || PtInRect(&more_down_, p)) {   // the scroll arrows of a long settings tab
        int n = int(items().size());
        int k = std::clamp(selected_ - 1, 0, n - 3) + (PtInRect(&more_up_, p) ? -1 : 1);
        select(1 + std::clamp(k, 0, n - 3));
        return;
    }
    mouse_move(x, y);
    auto list = items();
    if (hovered_ < 0 || hovered_ >= int(list.size()) || !list[hovered_].enabled) return;
    if (list[hovered_].id == TABS) {   // the tab under the pointer
        for (int t = 0; t < int(tab_rects_.size()); t++)
            if (PtInRect(&tab_rects_[t], p)) set_tab(t);
        return;
    }
    // Option rows: a click on the value side goes down left of the middle of the
    // value and up right of it (the arrows); on the label it steps up and wraps around
    int dir = +1;
    if (hovered_ < int(values_.size()) && values_[hovered_].left >= 0 && x >= values_[hovered_].left)
        dir = x < values_[hovered_].middle ? -1 : +1;
    else wrap_ = true;
    activate(list[hovered_], dir);
    wrap_ = false;
}

void Frontend::wheel(int x, int y, int delta) {
    if (screen_ == Screen::GunTest) return;
    mouse_move(x, y);
    if (screen_ == Screen::Records) {
        scroll_records(delta > 0 ? -1 : 1);
        return;
    }
    auto list = items();
    bool on_value = hovered_ >= 0 && hovered_ < int(values_.size()) && values_[hovered_].left >= 0 &&
                    x >= values_[hovered_].left;
    bool scrolls = screen_ == Screen::Options && int(list.size()) - 2 > visible_opts_;
    if (hovered_ >= 0 && hovered_ < int(list.size()) && list[hovered_].id >= OPT_DISPLAY && list[hovered_].kind != BUTTON &&
        (on_value || !scrolls)) {
        int keep = hovered_;   // over a value: change it
        hovered_ = -1;   // no wrap-around from the wheel
        activate(list[keep], delta > 0 ? +1 : -1);
        hovered_ = keep;
    } else if (scrolls) {   // a long tab, away from the values: scroll the list
        int k = std::clamp(selected_ - 1, 0, int(list.size()) - 3) + (delta > 0 ? -1 : 1);
        select(1 + std::clamp(k, 0, int(list.size()) - 3));
    }
}

std::vector<Frontend::Action> Frontend::take_actions() {
    std::vector<Action> a;
    a.swap(actions_);
    return a;
}

// ---- Records ----

namespace {

std::wstring grouped(int64_t v) {   // 12,345
    std::wstring d = std::to_wstring(v < 0 ? -v : v), r;
    for (size_t i = 0; i < d.size(); i++) {
        if (i && (d.size() - i) % 3 == 0) r += L',';
        r += d[i];
    }
    return v < 0 ? L"-" + r : r;
}

std::wstring clock_time(int64_t frames) {   // 2:31.4
    double s = double(frames) / 57.524;
    wchar_t b[32];
    swprintf(b, 32, L"%d:%04.1f", int(s / 60), s - 60 * int(s / 60));
    return b;
}

std::wstring hours(int64_t frames) {   // 3 H 05 MIN
    int64_t m = int64_t(double(frames) / 57.524 / 60);
    wchar_t b[32];
    swprintf(b, 32, L"%lld H %02lld MIN", (long long)(m / 60), (long long)(m % 60));
    return b;
}

std::wstring percent(int64_t part, int64_t whole) {
    if (whole <= 0) return L"-";
    wchar_t b[16];
    swprintf(b, 16, L"%.1f%%", 100.0 * double(part) / double(whole));
    return b;
}

}  // namespace

std::vector<Frontend::RecordRow> Frontend::record_rows() const {
    std::vector<RecordRow> rows;
    const Stats& s = records.stats;
    if (tab_ == 0) {   // achievements: unlocked first (newest on top), then the rest in the table's order
        const auto& table = achievement_table();
        std::vector<int> order(table.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
        auto when = [&](int i) { return size_t(i) < records.unlocked.size() ? records.unlocked[size_t(i)] : 0; };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return (when(a) != 0) != (when(b) != 0) ? when(a) != 0 : when(a) > when(b);
        });
        for (int i : order) {
            const AchievementInfo& a = table[size_t(i)];
            RecordRow r{a.name, a.text, L"", when(i) != 0};
            if (r.lit) {
                time_t t = time_t(when(i));
                wchar_t b[32];
                wcsftime(b, 32, L"%d %b %Y", localtime(&t));
                r.value = b;
            } else if (a.goal > 1) {
                int64_t p = achievement_progress(s, i);
                r.value = std::string(a.key) == "veteran"
                              ? std::to_wstring(int(double(p) / 57.524 / 3600)) + L" / 5 H"
                              : grouped(p) + L" / " + grouped(a.goal);
            } else {
                r.value = L"LOCKED";
            }
            rows.push_back(r);
        }
        return rows;
    }
    auto row = [&](const std::wstring& label, const std::wstring& value, const std::wstring& detail = L"") {
        rows.push_back({label, detail, value, true});
    };
    row(L"GAMES PLAYED", grouped(s.games), grouped(s.games_finished) + L" finished, " + grouped(s.continues) +
                                               L" continues");
    row(L"TIME PLAYED", hours(s.play_frames));
    row(L"HIT RATE", percent(s.hits, s.shots), grouped(s.hits) + L" hits out of " + grouped(s.shots) + L" shots");
    row(L"TECHNICAL SHOTS", grouped(s.technical));
    row(L"DINOSAURS DOWN", grouped(s.kills));
    row(L"LIVES LOST", grouped(s.lives_lost));
    static const wchar_t* const COURSES[STAT_COURSES] = {L"BEGINNER", L"EXPERT", L"SHUFFLE"};
    for (int i = 0; i < STAT_COURSES; i++)
        row(L"BEST SCORE " + std::wstring(COURSES[i]), s.best_score[i] ? grouped(s.best_score[i]) : L"-",
            s.finished_course[i] ? L"Finished " + grouped(s.finished_course[i]) + L" times" : L"");
    for (int i = 0; i < STAT_STAGES; i++) {
        const Stats::Stage& st = s.stage[i];
        std::wstring detail = st.clears ? L"Best score " + grouped(st.best_score) + L",  best hit rate " +
                                              percent(st.best_accuracy, 1000)
                                        : L"Not cleared yet";
        row(L"STAGE " + std::to_wstring(i + 1), st.best_frames ? clock_time(st.best_frames) : L"-",
            grouped(st.clears) + L" clears.  " + detail);
    }
    for (int i = 0; i < STAT_BOSSES && i < BOSS_COUNT; i++) {
        const Stats::Boss& b = s.boss[i];
        row(BOSSES[i].name, b.best_frames ? clock_time(b.best_frames) : L"-",
            grouped(b.wins) + L" wins in " + grouped(b.attempts) + L" fights" +
                (b.flawless ? L",  " + grouped(b.flawless) + L" flawless" : L""));
    }
    row(L"SURVIVAL", s.best_wave ? L"WAVE " + std::to_wstring(s.best_wave) : L"-", L"Furthest wave reached");
    return rows;
}

void Frontend::scroll_records(int by) {
    int n = int(record_rows().size());
    int k = std::clamp(record_scroll_ + by, 0, std::max(0, n - visible_records_));
    if (k != record_scroll_) dirty_ = true;
    record_scroll_ = k;
}

// The rows of the open records tab between x0 and x1, from y0 down
void Frontend::draw_records(int x0, int x1, int y0, int row_h) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    auto rows = record_rows();
    record_scroll_ = std::clamp(record_scroll_, 0, std::max(0, int(rows.size()) - visible_records_));
    const Style lit{PALE_TOP, PALE_BOTTOM, 0x080806, U(2), 0.95f};
    const Style dark{DIM, DIM, 0x080806, U(2), 0.9f};
    const Style value{0xF4D27A, 0xE0A848, 0x080806, U(2), 0.95f};
    int label_px = U(32), detail_px = U(19);
    for (int k = 0; k < visible_records_ && record_scroll_ + k < int(rows.size()); k++) {
        const RecordRow& r = rows[size_t(record_scroll_ + k)];
        int y = y0 + k * row_h;
        if (k % 2 == 0) fill(x0 - U(20), y - U(4), x1 + U(20), y + row_h - U(4), 0xFFFFFF, 0.03f);
        text(ITEM, label_px, r.label, x0, y, 0, r.lit ? lit : dark);
        if (!r.detail.empty())
            text(MONO, detail_px, r.detail, x0, y + label_px + U(4), 0, {r.lit ? HINT : DIM, r.lit ? HINT : DIM});
        text(ITEM, label_px, r.value, x1, y + U(6), 2, r.lit ? value : dark);
    }
    if (record_scroll_ > 0) {   // more above / below: arrows that also scroll on a click
        triangle(x1 + U(36), y0 + U(10), U(20), -2, GOLD_BOTTOM, 1);
        more_up_ = {x1 + U(10), y0 - U(16), x1 + U(64), y0 + U(36)};
    }
    if (record_scroll_ + visible_records_ < int(rows.size())) {
        int yb = y0 + visible_records_ * row_h - U(20);
        triangle(x1 + U(36), yb, U(20), 2, GOLD_BOTTOM, 1);
        more_down_ = {x1 + U(10), yb - U(26), x1 + U(64), yb + U(26)};
    }
}

// ---- Drawing primitives (premultiplied BGRA canvas) ----

// Horizontal band that fades out towards both ends
void Frontend::band(int cx, int y0, int y1, int half_w, uint32_t rgb, float alpha) {
    for (int x = std::max(cx - half_w, 0); x < std::min(cx + half_w, w_); x++) {
        float t = 1 - std::abs(float(x - cx)) / float(half_w);
        float a = alpha * std::min(1.0f, t * 2.5f);
        for (int y = std::max(y0, 0); y < std::min(y1, h_); y++) blend(x, y, rgb, a);
    }
}

// Small solid triangle pointing left (dir -1), right (1), up (-2) or down (2), centred on (x, y)
void Frontend::triangle(int x, int y, int size, int dir, uint32_t rgb, float alpha) {
    int half = size / 2;
    for (int dy = -half; dy <= half; dy++) {
        float len = float(size) * (1 - std::abs(float(dy)) / float(half + 1)) * 0.9f;
        for (int i = 0; i < int(len + 1); i++) {
            float a = std::clamp(len - float(i), 0.0f, 1.0f) * alpha;
            if (dir == 2 || dir == -2) {   // pointing down / up
                int py = dir > 0 ? y - half + i : y + half - i;
                blend(x + dy, py, rgb, a);
                continue;
            }
            int px = dir > 0 ? x - half + i : x + half - i;
            blend(px, y + dy, rgb, a);
        }
    }
}

void Frontend::hgrad(int x0, int y0, int x1, int y1, uint32_t rgb, float a0, float a1) {
    if (x1 <= x0) return;
    for (int x = std::max(x0, 0); x < std::min(x1, w_); x++) {
        float t = float(x - x0) / float(x1 - x0);
        int a = int(std::clamp(a0 + (a1 - a0) * t, 0.0f, 1.0f) * 256.0f + 0.5f);
        for (int y = std::max(y0, 0); y < std::min(y1, h_); y++) {
            uint32_t& d = px_[size_t(y) * w_ + x];
            d = over(d, rgb, a);
        }
    }
}

void Frontend::vgrad(int x0, int y0, int x1, int y1, uint32_t rgb, float a0, float a1) {
    if (y1 <= y0) return;
    for (int y = std::max(y0, 0); y < std::min(y1, h_); y++)
        fill(x0, y, x1, y + 1, rgb, a0 + (a1 - a0) * float(y - y0) / float(y1 - y0));
}

void Frontend::frame(int x0, int y0, int x1, int y1, int t, uint32_t rgb, float alpha) {
    fill(x0, y0, x1, y0 + t, rgb, alpha);
    fill(x0, y1 - t, x1, y1, rgb, alpha);
    fill(x0, y0 + t, x0 + t, y1 - t, rgb, alpha);
    fill(x1 - t, y0 + t, x1, y1 - t, rgb, alpha);
}

// A box with half-round ends, smoothed: switches, sliders, the gamepad's buttons
void Frontend::pill(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha) {
    float r = (y1 - y0) * 0.5f, cy = (y0 + y1) * 0.5f;
    float lx = x0 + r, rx = x1 - r;
    if (rx < lx) lx = rx = (x0 + x1) * 0.5f;
    for (int y = std::max(y0, 0); y < std::min(y1, h_); y++)
        for (int x = std::max(x0, 0); x < std::min(x1, w_); x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float dx = px < lx ? lx - px : px > rx ? px - rx : 0.0f, dy = py - cy;
            float cov = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            if (cov > 0) blend(x, y, rgb, cov * alpha);
        }
}

// A picture scaled to cover the box (bilinear), what overhangs cut off
void Frontend::picture(const Picture& p, int x0, int y0, int x1, int y1) {
    if (p.w <= 0 || x1 <= x0 || y1 <= y0) return;
    float sc = std::max(float(x1 - x0) / p.w, float(y1 - y0) / p.h);
    float ox = (p.w * sc - (x1 - x0)) * 0.5f, oy = (p.h * sc - (y1 - y0)) * 0.5f;
    for (int y = std::max(y0, 0); y < std::min(y1, h_); y++) {
        float v = std::clamp((y - y0 + oy + 0.5f) / sc - 0.5f, 0.0f, p.h - 1.001f);
        int iv = int(v);
        float fv = v - iv;
        for (int x = std::max(x0, 0); x < std::min(x1, w_); x++) {
            float u = std::clamp((x - x0 + ox + 0.5f) / sc - 0.5f, 0.0f, p.w - 1.001f);
            int iu = int(u);
            float fu = u - iu;
            const uint32_t* q = &p.px[size_t(iv) * p.w + iu];
            uint32_t c[4] = {q[0], q[1], q[p.w], q[p.w + 1]};
            uint32_t out = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                float a = chan(c[0], sh) + (chan(c[1], sh) - chan(c[0], sh)) * fu;
                float b = chan(c[2], sh) + (chan(c[3], sh) - chan(c[2], sh)) * fu;
                out |= uint32_t(std::clamp(a + (b - a) * fv, 0.0f, 1.0f) * 255.0f + 0.5f) << sh;
            }
            px_[size_t(y) * w_ + x] = out;
        }
    }
}

// A picture with transparency, area-averaged down to w x h at x0, y0
void Frontend::sprite(const Picture& p, int x0, int y0, int w, int h) {
    if (p.w <= 0 || w <= 0 || h <= 0) return;
    float sx = float(p.w) / w, sy = float(p.h) / h;
    for (int y = std::max(0, y0); y < std::min(h_, y0 + h); y++)
        for (int x = std::max(0, x0); x < std::min(w_, x0 + w); x++) {
            int u0 = int((x - x0) * sx), u1 = std::max(u0 + 1, std::min(p.w, int((x - x0 + 1) * sx)));
            int v0 = int((y - y0) * sy), v1 = std::max(v0 + 1, std::min(p.h, int((y - y0 + 1) * sy)));
            float rgb[3] = {0, 0, 0}, a = 0;   // colours weighted by their alpha
            for (int v = v0; v < v1; v++)
                for (int u = u0; u < u1; u++) {
                    uint32_t c = p.px[size_t(v) * p.w + u];
                    float ca = chan(c, 24);
                    for (int m = 0; m < 3; m++) rgb[m] += chan(c, 16 - 8 * m) * ca;
                    a += ca;
                }
            if (a <= 0) continue;
            uint32_t c = 0;
            for (int m = 0; m < 3; m++) c |= uint32_t(std::clamp(rgb[m] / a, 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * m);
            blend(x, y, c, a / ((u1 - u0) * (v1 - v0)));
        }
}

int Frontend::measure(Family f, int px, const std::wstring& s) {
    if (f == JURASSIC) px = int(px * 1.34f + 0.5f);   // as text() draws it
    return mask(f, px, s, 0).text_w;
}

int Frontend::extent(Family f, int px, const std::wstring& s) { return gdi_->extent(GdiText::Face(f), px, s); }

// Lines of at most max_w pixels, broken between words
std::vector<std::wstring> Frontend::wrap(Family f, int px, const std::wstring& s, int max_w) {
    std::vector<std::wstring> lines;
    std::wstring line, word;
    auto put = [&] {
        if (word.empty()) return;
        std::wstring longer = line.empty() ? word : line + L" " + word;
        if (!line.empty() && extent(f, px, longer) > max_w) {
            lines.push_back(line);
            line = word;
        } else {
            line = longer;
        }
        word.clear();
    };
    for (wchar_t c : s) {
        if (c == L' ') put();
        else word += c;
    }
    put();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Draws text with its box's top at y; align 0 = left of x, 1 = centred, 2 = right
int Frontend::text(Family f, int px, const std::wstring& s, int x, int y, int align, const Style& st) {
    if (f == JURASSIC) {   // same capital height and position as Saira at this size
        y += int(px * 0.42f + 0.5f);
        px = int(px * 1.34f + 0.5f);
    }
    const Mask& m = mask(f, px, s, st.edge_px);
    int x0 = x - (align == 1 ? m.text_w / 2 : align == 2 ? m.text_w : 0) - m.pad;
    int y0 = y - m.pad;
    auto paint = [&](const std::vector<uint8_t>& cov, int ox, int oy, bool gradient, uint32_t rgb, float alpha) {
        for (int j = 0; j < m.h; j++) {
            uint32_t row = rgb;
            if (gradient) {
                float t = std::clamp(float(j - m.grad0) / float(std::max(1, m.grad1 - m.grad0)), 0.0f, 1.0f);
                row = mix(st.top, st.bottom, t);
            }
            for (int i = 0; i < m.w; i++) {
                uint8_t c = cov[size_t(j) * m.w + i];
                if (c) blend(x0 + ox + i, y0 + oy + j, row, float(c) / 255.0f * alpha);
            }
        }
    };
    const auto& outer = st.edge_px > 0 ? m.e : m.a;
    if (st.shadow) paint(outer, st.shadow / 2, st.shadow, false, 0x000000, 0.55f * st.alpha);
    if (st.edge_px > 0) paint(m.e, 0, 0, false, st.edge, st.alpha);
    paint(m.a, 0, 0, true, 0, st.alpha);
    return m.text_w;
}

// Poster placement (fractions of the poster's height): where its logo starts
// and ends, and where its plain stone ends (credits below). The logo is drawn
// LOGO_SCREEN_H of the window high, starting LOGO_SCREEN_TOP from the top,
// centred LOGO_CENTER_X layout units from the left (above the menu's column).
constexpr float POSTER_LOGO_TOP = 0.16f, POSTER_LOGO_END = 0.53f, POSTER_STONE_END = 0.88f;
constexpr float LOGO_SCREEN_TOP = 0.08f, LOGO_SCREEN_H = 0.36f, LOGO_CENTER_X = 450;
constexpr float LOGO_PICTURE_X = 350;   // logo.png's centre from the right edge: lined up with the menu's column

void Frontend::poster_layout(float& scale, float& x0, float& y0) const {
    scale = LOGO_SCREEN_H * h_ / ((POSTER_LOGO_END - POSTER_LOGO_TOP) * poster_h_);   // window px per poster px
    float cx = has_backdrop() ? w_ - LOGO_CENTER_X * u_ : LOGO_CENTER_X * u_;   // above the menu's column
    x0 = cx - poster_w_ * scale * 0.5f;
    y0 = LOGO_SCREEN_TOP * h_ - POSTER_LOGO_TOP * poster_h_ * scale;
}

// The poster at the top left; around and below it, its plain stone repeated in
// mirror image with the brightness of the poster's edges at that height,
// cross-faded into the poster and darkened away from it. stone gets the same
// without the poster: both come from one pass, as they share the stone samples
void Frontend::poster_background(std::vector<uint32_t>& dst, std::vector<uint32_t>& stone) {
    dst.resize(size_t(w_) * h_);
    stone.resize(size_t(w_) * h_);
    float s, x0, y0;
    poster_layout(s, x0, y0);
    float pw = poster_w_ * s;
    float stone0 = poster_h_ * 0.60f, stone_h = poster_h_ * (POSTER_STONE_END - 0.60f);
    auto texel = [&](float u, float v, float* rgb) {   // bilinear, poster pixels
        u = std::clamp(u, 0.0f, poster_w_ - 1.001f);
        v = std::clamp(v, 0.0f, poster_h_ - 1.001f);
        int iu = int(u), iv = int(v);
        float fu = u - iu, fv = v - iv;
        const uint32_t* p = &poster_[size_t(iv) * poster_w_ + iu];
        uint32_t c[4] = {p[0], p[1], p[poster_w_], p[poster_w_ + 1]};
        for (int k = 0; k < 3; k++) {
            int sh = 16 - 8 * k;
            float a = chan(c[0], sh) + (chan(c[1], sh) - chan(c[0], sh)) * fu;
            float b = chan(c[2], sh) + (chan(c[3], sh) - chan(c[2], sh)) * fu;
            rgb[k] += a + (b - a) * fv;
        }
    };
    auto pingpong = [](float t, float len) {
        float m = std::fmod(std::abs(t), 2 * len);
        return m < len ? m : 2 * len - m;
    };
    // Brightness of the poster's outer columns per row (the stone is lighter at
    // the top), against that of the stone used for the sides
    std::vector<float> edge(poster_h_);
    int band_w = std::max(1, poster_w_ / 20);
    for (int y = 0; y < poster_h_; y++) {
        float sum = 0;
        for (int x = 0; x < band_w; x++)
            for (uint32_t c : {poster_[size_t(y) * poster_w_ + x], poster_[size_t(y) * poster_w_ + poster_w_ - 1 - x]})
                sum += chan(c, 16) + chan(c, 8) + chan(c, 0);
        edge[y] = sum / (band_w * 6);
    }
    std::vector<float> smooth(poster_h_);
    int r = std::max(1, poster_h_ / 30);
    for (int y = 0; y < poster_h_; y++) {
        float sum = 0;
        int n = 0;
        for (int k = std::max(0, y - r); k <= std::min(poster_h_ - 1, y + r); k++) sum += edge[k], n++;
        smooth[y] = sum / n;
    }
    float stone_lum = 0;
    for (int y = int(stone0); y < int(stone0 + stone_h); y++) stone_lum += smooth[y];
    stone_lum = std::max(stone_lum / std::max(1, int(stone_h)), 0.01f);

    float step = 0.5f / s;   // 2x2 samples per window pixel
    float seam = pw * 0.2f, fade_h = poster_h_ * 0.05f;
    // Rows are split between threads: it is a lot of samples at 4K
    auto rows_job = [&](int ya, int yb) {
    for (int y = ya; y < yb; y++) {
        float py = (y - y0) / s;
        // the poster's own picture stops before its credits
        float rows = std::clamp((POSTER_STONE_END * poster_h_ - py) / fade_h, 0.0f, 1.0f);
        float gain = std::clamp(smooth[std::clamp(int(std::min(py, stone0 + stone_h)), 0, poster_h_ - 1)] / stone_lum,
                                0.6f, 1.8f);
        for (int x = 0; x < w_; x++) {
            float poster_rgb[3] = {0, 0, 0}, stone_rgb[3] = {0, 0, 0};
            const float none[3] = {0, 0, 0};
            float px = (x - x0) / s;
            float inside = std::clamp(std::min(x - x0, x0 + pw - x) / seam, 0.0f, 1.0f) * rows;
            if (inside > 0)
                for (int k = 0; k < 4; k++) texel(px + (k & 1) * step, py + (k >> 1) * step, poster_rgb);
            for (int k = 0; k < 4; k++)
                texel(pingpong(px + (k & 1) * step, float(poster_w_ - 1)),
                      stone0 + pingpong(py + (k >> 1) * step, stone_h), stone_rgb);
            float side = std::min(1.0f, std::abs(x - (x0 + pw * 0.5f)) / float(w_));
            float dark = 1 - 0.4f * side * side;
            auto pixel = [&](const float* poster, float in) {
                uint32_t out = 0xFF000000u;
                for (int k = 0; k < 3; k++) {
                    float v = (poster[k] * in + stone_rgb[k] * gain * (1 - in)) * 0.25f * dark;
                    out |= uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f) << (16 - 8 * k);
                }
                return out;
            };
            dst[size_t(y) * w_ + x] = pixel(poster_rgb, inside);
            stone[size_t(y) * w_ + x] = pixel(none, 0.0f);
        }
    }
    };
    int n = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int i = 0; i < n; i++) pool.emplace_back(rows_job, h_ * i / n, h_ * (i + 1) / n);
    for (auto& t : pool) t.join();
}

// The background picture: covering the window, darker towards the right where
// the menu goes, with the poster's logo above it; blurred for the options
void Frontend::backdrop_background() {
    uint32_t* keep = px_;
    bg_.assign(size_t(w_) * h_, 0xFF000000u);
    px_ = bg_.data();
    picture(backdrop_, 0, 0, w_, h_);
    // The blur: the picture averaged down by 16 in each direction, smoothed a
    // few times and stretched back
    const int F = 16;
    Picture blur;
    blur.w = std::max(4, w_ / F);
    blur.h = std::max(4, h_ / F);
    std::vector<float> acc(size_t(blur.w) * blur.h * 3, 0.0f);
    for (int y = 0; y < blur.h * F && y < h_; y++)
        for (int x = 0; x < blur.w * F && x < w_; x++) {
            uint32_t c = bg_[size_t(y) * w_ + x];
            float* d = &acc[(size_t(y / F) * blur.w + x / F) * 3];
            for (int m = 0; m < 3; m++) d[m] += chan(c, 16 - 8 * m) / (F * F);
        }
    for (int pass = 0; pass < 3; pass++) {   // 3x3 box, three times
        std::vector<float> src = acc;
        for (int y = 0; y < blur.h; y++)
            for (int x = 0; x < blur.w; x++)
                for (int m = 0; m < 3; m++) {
                    float sum = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = std::clamp(x + dx, 0, blur.w - 1), yy = std::clamp(y + dy, 0, blur.h - 1);
                            sum += src[(size_t(yy) * blur.w + xx) * 3 + m];
                        }
                    acc[(size_t(y) * blur.w + x) * 3 + m] = sum / 9;
                }
    }
    blur.px.resize(size_t(blur.w) * blur.h);
    for (size_t i = 0; i < blur.px.size(); i++) {
        uint32_t out = 0xFF000000u;
        for (int m = 0; m < 3; m++)
            out |= uint32_t(std::clamp(acc[i * 3 + m], 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * m);
        blur.px[i] = out;
    }
    bg_stone_.assign(size_t(w_) * h_, 0xFF000000u);
    px_ = bg_stone_.data();
    picture(blur, 0, 0, w_, h_);
    fill(0, 0, w_, h_, 0x000000, 0.3f);
    px_ = bg_.data();
    hgrad(int(w_ * 0.40f), 0, int(w_ * 0.64f), h_, 0x040508, 0.0f, 0.6f);
    fill(int(w_ * 0.64f), 0, w_, h_, 0x040508, 0.6f);
    hgrad(int(w_ * 0.64f), 0, w_, h_, 0x040508, 0.0f, 0.6f);
    vgrad(0, int(h_ * 0.72f), w_, h_, 0x000000, 0.0f, 0.6f);
    if (has_logo()) {   // the logo picture, area-averaged down to LOGO_SCREEN_H of the window
        int lh = int(LOGO_SCREEN_H * h_), lw = lh * logo_.w / logo_.h;
        sprite(logo_, int(w_ - LOGO_PICTURE_X * u_) - lw / 2, int(LOGO_SCREEN_TOP * h_), lw, lh);
    } else if (has_poster()) {   // the logo cut out of the poster: its disc and its banner (cover.png's shapes)
        float s, x0, y0;
        poster_layout(s, x0, y0);
        const float CX = 0.497f, CY = 0.331f, R = 0.203f;          // the disc, fractions of the poster's width / height
        const float BU0 = 0.092f, BU1 = 0.903f, BV0 = 0.333f, BV1 = 0.437f;   // the banner across it
        float cx = CX * poster_w_, cy = CY * poster_h_, r = R * poster_h_;
        int sx0 = std::max(0, int(x0 + std::min(cx - r, BU0 * poster_w_) * s) - 2);
        int sx1 = std::min(w_, int(x0 + std::max(cx + r, BU1 * poster_w_) * s) + 2);
        int sy0 = std::max(0, int(y0 + (cy - r) * s) - 2), sy1 = std::min(h_, int(y0 + (cy + r) * s) + 2);
        float feather = 2.5f;   // poster pixels
        for (int y = sy0; y < sy1; y++)
            for (int x = sx0; x < sx1; x++) {
                float u = (x + 0.5f - x0) / s, v = (y + 0.5f - y0) / s;
                float in_disc = r - std::sqrt((u - cx) * (u - cx) + (v - cy) * (v - cy));
                float in_banner = std::min(std::min(u - BU0 * poster_w_, BU1 * poster_w_ - u),
                                           std::min(v - BV0 * poster_h_, BV1 * poster_h_ - v));
                float a = std::clamp(std::max(in_disc, in_banner) / feather + 0.5f, 0.0f, 1.0f);
                if (a <= 0) continue;
                float rgb[3] = {0, 0, 0};   // 2x2 samples per window pixel
                for (int k = 0; k < 4; k++) {
                    int pu = std::clamp(int((x + 0.25f + 0.5f * (k & 1) - x0) / s), 0, poster_w_ - 1);
                    int pv = std::clamp(int((y + 0.25f + 0.5f * (k >> 1) - y0) / s), 0, poster_h_ - 1);
                    uint32_t c = poster_[size_t(pv) * poster_w_ + pu];
                    for (int m = 0; m < 3; m++) rgb[m] += chan(c, 16 - 8 * m) * 0.25f;
                }
                blend(x, y, uint32_t(rgb[0] * 255) << 16 | uint32_t(rgb[1] * 255) << 8 | uint32_t(rgb[2] * 255), a);
            }
    }
    px_ = keep;
}

// Main menu backdrop: dark jungle green, a warm glow behind the title and a
// faint scanline pattern
void Frontend::background() {
    bg_.resize(size_t(w_) * h_);
    if (has_backdrop()) {
        backdrop_background();
        return;
    }
    if (has_poster()) {   // both versions at once: switching screens must not stall
        poster_background(bg_, bg_stone_);   // the long options list goes on plain stone
        return;
    }
    float cx = LOGO_CENTER_X * u_, gy = 250 * u_;   // the glow behind the title
    for (int y = 0; y < h_; y++) {
        float t = std::pow(float(y) / float(h_), 0.8f);
        uint32_t base = mix(0x13291A, 0x040806, t);
        float br = chan(base, 16), bgc = chan(base, 8), bb = chan(base, 0);
        float scan = (y % 3 == 0) ? 0.93f : 1.0f;
        for (int x = 0; x < w_; x++) {
            float dx = (x - cx) / (950 * u_), dy = (y - gy) / (430 * u_);
            float glow = std::exp(-(dx * dx + dy * dy) * 1.6f) * 0.17f;
            float vx = (x - cx) / (w_ * 0.62f), vy = (y - h_ * 0.5f) / (h_ * 0.8f);
            float vig = 1 - 0.6f * std::min(1.0f, vx * vx + vy * vy);
            float r = (br + glow * 1.0f) * vig * scan, g = (bgc + glow * 0.6f) * vig * scan,
                  b = (bb + glow * 0.22f) * vig * scan;
            bg_[size_t(y) * w_ + x] = 0xFF000000u | uint32_t(std::min(r, 1.0f) * 255) << 16 |
                                      uint32_t(std::min(g, 1.0f) * 255) << 8 | uint32_t(std::min(b, 1.0f) * 255);
        }
    }
}


// ---- Screens ----

namespace {
constexpr uint32_t WHITE_TOP = 0xFFF6E0, WHITE_BOTTOM = 0xFFE2A8, INK = 0x080806, PANEL = 0x090D0B;
constexpr uint32_t PAD_A = 0x6FD17A, PAD_B = 0xE8655A, PAD_Y = 0xE8C85A, PAD_X = 0x5AA8E8, KEYCAP = 0xD8D2C0;
}  // namespace

// One entry of the key help: the gamepad's button (its picture from res/pad,
// else a coloured disc, or a grey pill for the shoulders) or the key ("<>": the left / right arrows), then
// what it does. x moves past it; the rectangle is for clicks
RECT Frontend::prompt(int& x, int y, const wchar_t* pad_button, const wchar_t* key, const wchar_t* what) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int h = U(38), x0 = x;
    if (pad_prompts && pad_button && pad_icon(pad_button).w > 0) {
        const Picture& icon = pad_icon(pad_button);
        int iw = h * icon.w / icon.h;
        sprite(icon, x, y, iw, h);
        x += iw + U(10);
    } else if (pad_prompts && pad_button) {
        std::wstring b = pad_button;
        uint32_t c = b == L"A" ? PAD_A : b == L"B" ? PAD_B : b == L"Y" ? PAD_Y : b == L"X" ? PAD_X : KEYCAP;
        int bw = std::max(h, extent(ITEM, U(20), b) + U(20));
        pill(x, y, x + bw, y + h, c, 1);
        text(ITEM, U(20), b, x + bw / 2, y + h / 2 - U(11), 1, {0x111111, 0x111111});
        x += bw + U(10);
    } else if (key) {
        std::wstring k = key;
        bool arrows = k == L"<>";
        int kw = arrows ? U(70) : std::max(h, extent(MONO, U(18), k) + U(22));
        fill(x, y, x + kw, y + h, 0x000000, 0.45f);
        frame(x, y, x + kw, y + h, std::max(1, U(1.5f)), KEYCAP, 0.75f);
        if (arrows) {
            triangle(x + kw / 2 - U(14), y + h / 2, U(14), -1, 0xE8E0CC, 1);
            triangle(x + kw / 2 + U(14), y + h / 2, U(14), +1, 0xE8E0CC, 1);
        } else {
            text(MONO, U(18), k, x + kw / 2, y + h / 2 - U(9), 1, {0xE8E0CC, 0xE8E0CC});
        }
        x += kw + U(10);
    }
    int right = x - U(10);
    if (what && *what) {
        right = x + text(ITEM, U(22), what, x, y + h / 2 - U(13), 0, {0xCFC8B4, 0xCFC8B4});
        x = right + U(44);
    }
    return {x0, y, right, y + h};
}

int Frontend::prompt_width(const wchar_t* pad_button, const wchar_t* key, const wchar_t* what) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int h = U(38), wd = 0;
    const Picture* icon = pad_prompts && pad_button ? &pad_icon(pad_button) : nullptr;
    if (icon && icon->w > 0) wd = h * icon->w / icon->h + U(10);
    else if (icon) wd = std::max(h, extent(ITEM, U(20), pad_button) + U(20)) + U(10);
    else if (key) wd = (std::wstring(key) == L"<>" ? U(70) : std::max(h, extent(MONO, U(18), key) + U(22))) + U(10);
    if (what && *what) wd += measure(ITEM, U(22), what) + U(44);
    return wd;
}

// The value side of an option row, between vx0 and vx1
void Frontend::draw_value(const Item& it, bool sel, int vx0, int vx1, int y, int row_h) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int cy = y + row_h / 2, vcx = (vx0 + vx1) / 2, vpx = U(27);
    const Style vs = !it.enabled ? Style{DIM, DIM, INK, U(2), 0.9f}
                     : sel       ? Style{WHITE_TOP, WHITE_BOTTOM, INK, U(2)}
                                 : Style{0xF4D27A, 0xE0A848, INK, U(2), 0.95f};
    int text_y = cy - vpx * 55 / 100;   // the capitals centred on the row
    switch (it.enabled ? it.kind : PLAIN) {
    case TOGGLE: {
        int tw = U(72), th = U(34), x0 = vcx - tw / 2, y0 = cy - th / 2;
        bool on = it.pos != 0;
        if (on) {
            pill(x0, y0, x0 + tw, y0 + th, AMBER, sel ? 1.0f : 0.85f);
        } else {
            pill(x0, y0, x0 + tw, y0 + th, 0x4A4A40, 1);
            pill(x0 + U(2), y0 + U(2), x0 + tw - U(2), y0 + th - U(2), 0x22231E, 1);
        }
        int r = U(12), kx = on ? x0 + tw - th / 2 : x0 + th / 2;
        pill(kx - r, cy - r, kx + r, cy + r, on ? WHITE_TOP : 0x8A8A80, 1);
        text(ITEM, U(20), on ? L"ON" : L"OFF", x0 - U(16), cy - U(11), 2, {on ? HINT : FOOT, on ? HINT : FOOT});
        break;
    }
    case SLIDER: {
        int x0 = vx0 + U(26), x1 = vx1 - U(90), th = std::max(2, U(6));
        int kx = x0 + int(std::clamp(it.frac, 0.0f, 1.0f) * float(x1 - x0) + 0.5f);
        pill(x0, cy - th / 2, x1, cy - th / 2 + th, 0x3D3B33, 1);
        if (kx - x0 >= th) pill(x0, cy - th / 2, kx, cy - th / 2 + th, AMBER, 1);
        int r = U(11), ring = std::max(2, U(3));
        pill(kx - r - ring, cy - r - ring, kx + r + ring, cy + r + ring, AMBER, 1);
        pill(kx - r, cy - r, kx + r, cy + r, WHITE_TOP, 1);
        text(ITEM, vpx, it.value, vx1 - U(14), text_y, 2, vs);
        if (sel) {
            triangle(x0 - U(18), cy, U(16), -1, GOLD_BOTTOM, 1);
            triangle(vx1 + U(2), cy, U(16), +1, GOLD_BOTTOM, 1);
        }
        break;
    }
    case CHOICE: {
        text(ITEM, vpx, it.value, vcx, text_y - U(8), 1, vs);
        float a = sel ? 1.0f : 0.7f;
        triangle(vx0 + U(14), cy - U(4), U(18), -1, GOLD_BOTTOM, a);
        triangle(vx1 - U(14), cy - U(4), U(18), +1, GOLD_BOTTOM, a);
        int n = it.levels ? it.count - 1 : it.count;   // levels: a dot per step above OFF
        int pw = U(22), gap = U(6), ph = std::max(2, U(4));
        int px0 = vcx - (n * pw + (n - 1) * gap) / 2, py = cy + U(20);
        for (int k = 0; k < n; k++) {
            bool lit = it.levels ? k < it.pos : k == it.pos;
            fill(px0 + k * (pw + gap), py, px0 + k * (pw + gap) + pw, py + ph, lit ? AMBER : 0x4A473D, 1);
        }
        break;
    }
    case BUTTON: {
        int bw = std::min(vx1 - vx0, extent(ITEM, U(22), it.value) + U(40)), bh = U(38);
        int x0 = vcx - bw / 2, y0 = cy - bh / 2;
        fill(x0, y0, x0 + bw, y0 + bh, sel ? AMBER : 0x2A2B25, sel ? 0.9f : 1.0f);
        frame(x0, y0, x0 + bw, y0 + bh, std::max(1, U(1.5f)), AMBER, sel ? 1.0f : 0.6f);
        text(ITEM, U(22), it.value, vcx, cy - U(13), 1, sel ? Style{0x1A1205, 0x1A1205} : Style{HINT, HINT});
        break;
    }
    default: text(ITEM, vpx, it.value, vcx, text_y, 1, vs); break;
    }
}

// Main, Play, Boss and Survival on the backdrop, and Pause (or the boss list)
// over the game: the entries in a column at the left, under the logo or the title
void Frontend::draw_menu(bool over_game, const std::vector<Item>& list) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int w = w_, h = h_;
    bool right = has_backdrop() && !over_game;   // the column at the right, clear of the picture's subject
    int left = U(110), col_w = U(720);
    int tx = right ? w - left : left, talign = right ? 2 : 0;   // where the column's text lines up
    int band0 = right ? w - left - col_w : 0, band1 = right ? w : left + col_w;
    bool boss = screen_ == Screen::Boss || screen_ == Screen::Survival;
    int y0;
    if (over_game) {
        fill(0, 0, w, h, 0x000000, 0.5f);   // the frozen game shows through, darkened
        hgrad(0, 0, U(1100), h, 0x000000, 0.75f, 0.0f);
        text(display(), U(96), screen_ == Screen::Boss ? L"BOSSES" : L"PAUSED", left, U(70), 0,
             {GOLD_TOP, GOLD_BOTTOM, EDGE, U(5), 1, U(5)});
        y0 = U(300);
    } else {
        if (!right) hgrad(0, 0, U(1100), h, 0x030504, 0.5f, 0.0f);   // the column a little darker than the stone
        if (has_logo()) {
            y0 = int((LOGO_SCREEN_TOP + LOGO_SCREEN_H) * h_) + U(40);
        } else if (has_poster()) {   // the poster is the title
            float ps, px0, py0;
            poster_layout(ps, px0, py0);
            y0 = int(py0 + POSTER_LOGO_END * poster_h_ * ps) + U(40);
        } else {
            if (jurassic_) text(JURASSIC, U(54), L"JURASSIC PARK", tx, U(84), talign, {0xD8C49A, 0xB09A70, EDGE, U(2)});
            else text(MONO, U(30), L"J U R A S S I C    P A R K", tx, U(104), talign, {0xD8C49A, 0xB09A70});
            text(display(), jurassic_ ? U(150) : U(180), L"THE LOST WORLD", tx, jurassic_ ? U(122) : U(112), talign,
                 {GOLD_TOP, GOLD_BOTTOM, EDGE, U(7), 1, U(8)});
            y0 = U(430);
        }
        const wchar_t* sub = screen_ == Screen::Play ? L"SELECT MODE" : screen_ == Screen::Survival ? L"SELECT ARENA"
                             : screen_ == Screen::Classic ? L"CLASSIC ARCADE"
                             : screen_ == Screen::Weather ? L"WEATHER"
                             : boss ? L"SELECT BOSS" : nullptr;
        if (sub) {
            text(MONO, U(24), sub, tx, y0, talign, {0xF0C070, 0xE0A050});
            y0 += U(50);
        }
    }
    if (screen_ == Screen::Main && !over_game) {   // how far the player has got, in the top corner away from the logo
        int got = 0;
        for (int64_t t : records.unlocked) got += t != 0;
        int best = *std::max_element(std::begin(records.stats.best_score), std::end(records.stats.best_score));
        std::vector<std::pair<std::wstring, std::wstring>> chips = {
            {L"ACHIEVEMENTS", std::to_wstring(got) + L"/" + std::to_wstring(achievement_table().size())}};
        if (best > 0) chips.push_back({L"BEST SCORE", grouped(best)});
        int x1 = w - U(70), cy0 = U(56), ch = U(56);
        int cup = U(28);   // the donation link, leftmost of the row: Buy Me a Coffee's yellow button and its cup
        int coffee_w = U(24) + cup + U(12) + extent(ITEM, U(21), COFFEE_LABEL) + U(28);
        if (right) {   // from the left edge: the total width first
            x1 = U(70) - U(16) + coffee_w + U(16);
            for (const auto& c : chips)
                x1 += U(22) + extent(ITEM, U(21), c.first) + U(12) + extent(ITEM, U(26), c.second) + U(22) + U(16);
        }
        for (int i = int(chips.size()) - 1; i >= 0; i--) {
            int lw = extent(ITEM, U(21), chips[i].first), vw = extent(ITEM, U(26), chips[i].second);
            int cw = U(22) + lw + U(12) + vw + U(22), x0 = x1 - cw;
            fill(x0, cy0, x1, cy0 + ch, PANEL, 0.8f);
            frame(x0, cy0, x1, cy0 + ch, std::max(1, U(1.5f)), AMBER, 0.3f);
            text(ITEM, U(21), chips[i].first, x0 + U(22), cy0 + ch / 2 - U(12), 0, {PALE_TOP, PALE_BOTTOM});
            text(ITEM, U(26), chips[i].second, x1 - U(22), cy0 + ch / 2 - U(15), 2, {GOLD_TOP, GOLD_BOTTOM});
            x1 = x0 - U(16);
        }
        int x0 = x1 - coffee_w;
        coffee_rect_ = {x0, cy0, x1, cy0 + ch};   // brighter under the pointer, a click opens it
        bool lit = coffee_hot_ || coffee_focus_;
        if (coffee_focus_)   // chosen with the keys or a pad: an amber ring round it, like a selected entry's bar
            pill(x0 - U(5), cy0 - U(5), x1 + U(5), cy0 + ch + U(5), AMBER, 1);
        pill(x0, cy0, x1, cy0 + ch, lit ? 0xFFE94D : 0xFFDD00, lit ? 1.0f : 0.9f);
        // A takeaway cup: the lid, the body narrowing to the bottom, a yellow sleeve round its middle
        int cx = x0 + U(24) + cup / 2, cy = cy0 + (ch - cup) / 2;
        pill(cx - cup / 2, cy, cx + cup / 2, cy + cup * 22 / 100, COFFEE_INK, 1);
        for (int y = cy + cup * 26 / 100; y < cy + cup; y++) {
            int half = cup * 42 / 100 - (y - cy) * 12 / 100, edge = std::max(1, U(2));
            fill(cx - half, y, cx + half, y + 1, COFFEE_INK, 1);
            if (y >= cy + cup * 48 / 100 && y < cy + cup * 72 / 100)
                fill(cx - half + edge, y, cx + half - edge, y + 1, 0xFFDD00, 1);
        }
        text(ITEM, U(21), COFFEE_LABEL, x0 + U(24) + cup + U(12), cy0 + ch / 2 - U(12), 0, {COFFEE_INK, COFFEE_INK});
    }

    // The entries share what is left of the height, the description under them
    int n = int(list.size());
    int spacing = std::clamp((h - U(190) - y0) / std::max(1, n), U(46), U(86));
    int item_px = std::min(U(60), spacing * 72 / 100);
    int y = y0;
    for (int i = 0; i < n; i++) {
        const Item& it = list[i];
        bool sel = i == selected_ && !coffee_focus_;
        if (sel) {
            float a = it.enabled ? 0.34f : 0.16f;
            hgrad(band0, y, band1, y + spacing, AMBER, right ? 0.0f : a, right ? a : 0.0f);
            int edge = std::max(3, U(8));
            fill(right ? w - edge : 0, y, right ? w : edge, y + spacing, AMBER, it.enabled ? 1.0f : 0.5f);
        }
        const Style st = !it.enabled ? Style{DIM, DIM, INK, U(2), 0.9f, U(3)}
                         : sel       ? Style{WHITE_TOP, WHITE_BOTTOM, INK, U(2), 1, U(3)}
                                     : Style{PALE_TOP, PALE_BOTTOM, INK, U(2), 0.95f, U(3)};
        int ty = y + spacing / 2 - item_px * 55 / 100;
        text(TITLE, item_px, it.label, tx, ty, talign, st);
        if (!it.value.empty())   // e.g. the stage of a boss, "NOT READY" or a record
            text(ITEM, U(24), it.value, right ? tx - measure(TITLE, item_px, it.label) - U(30) : left + col_w - U(60),
                 y + spacing / 2 - U(14), 2, {0xF4D27A, 0xE0A848});
        hit_.push_back({band0, y, band1, y + spacing});
        values_.push_back({-1, 0});
        y += spacing;
    }
    if (selected_ < n)
        for (const std::wstring& line : wrap(BODY, U(23), coffee_focus_ ? COFFEE_HINT : list[selected_].hint, col_w - U(20))) {
            y += U(32);
            text(BODY, U(23), line, tx, y - U(16), talign, {0xE8C890, 0xE8C890});
        }

    vgrad(0, h - U(120), w, h, 0x000000, 0.0f, 0.85f);
    const wchar_t* back_what = screen_ == Screen::Main ? nullptr : screen_ == Screen::Pause ? L"RESUME" : L"BACK";
    int py = h - U(64), x = U(80);
    if (!over_game) {   // the key help at the bottom right; at the left, the credit to the makers of the game
        x = w - U(80) + U(44) - prompt_width(L"A", L"ENTER", L"SELECT") - (back_what ? prompt_width(L"B", L"ESC", back_what) : 0);
        if (sega_.w > 0) {   // their logo, its own transparency kept
            int lh = U(52), lw = lh * sega_.w / sega_.h, lx = U(80), ly = h - U(46) - lh;
            for (int j = 0; j < lh; j++)
                for (int i = 0; i < lw; i++) {
                    uint32_t q = sega_.px[size_t(j * sega_.h / lh) * sega_.w + size_t(i * sega_.w / lw)];
                    blend(lx + i, ly + j, q & 0xFFFFFF, float(q >> 24) / 255.0f);
                }
            text(ITEM, U(18), L"\u00A9 1997 SEGA", lx + lw + U(16), ly + lh / 2 - U(10), 0, {FOOT, FOOT});
        } else {
            text(ITEM, U(22), L"ORIGINAL ARCADE GAME \u00A9 1997 SEGA", U(80), py + U(6), 0, {0xB8B09A, 0xA09884});
        }
    }
    prompt(x, py, L"A", L"ENTER", L"SELECT");
    if (back_what) back_rect_ = prompt(x, py, L"B", L"ESC", back_what);
}

// Options and Records: the categories down the left, the rows in the middle and
// a panel at the right about the selected row (its picture, what it does, its default)
void Frontend::draw_options(bool over_game, const std::vector<Item>& list) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int w = w_, h = h_;
    bool rec_page = screen_ == Screen::Records;
    fill(0, 0, w, h, 0x000000, over_game ? 0.72f : 0.35f);
    bool wide = w / u_ >= 1800;   // 16:9 and wider; 4:3 gets narrower columns
    int margin = U(wide ? 70.0f : 50.0f);
    int rail0 = margin, rail1 = rail0 + U(wide ? 300.0f : 260.0f);
    int side1 = w - margin, side0 = side1 - U(wide ? 600.0f : 420.0f);
    int list0 = rail1 + U(30), list1 = side0 - U(30);
    int top = U(170), bottom = h - U(110);
    text(display(), U(72), rec_page ? L"RECORDS" : over_game ? L"SETTINGS" : L"OPTIONS", margin + U(10), U(36), 0,
         {GOLD_TOP, GOLD_BOTTOM, EDGE, U(4), 1, U(4)});

    // Categories (the TABS entry); Left / Right on it, Q / E or L1 / R1 anywhere change them
    int cat_h = U(64), cat_y = top + U(20);
    bool rail_focus = selected_ == 0;
    tab_rects_.clear();
    for (int t = 0; t < tab_count(); t++) {
        int y = cat_y + t * cat_h;
        bool open_tab = t == tab_;
        if (open_tab) {
            hgrad(rail0, y, rail1, y + cat_h, AMBER, rail_focus ? 0.42f : 0.25f, 0.0f);
            fill(rail0, y, rail0 + std::max(3, U(5)), y + cat_h, AMBER, 1);
            if (rail_focus) frame(rail0, y, rail1, y + cat_h, std::max(1, U(1.5f)), AMBER, 0.6f);
        }
        const Style cs = open_tab ? Style{WHITE_TOP, WHITE_BOTTOM, INK, U(2)} : Style{0xA9A390, 0x8F8978, INK, U(2), 0.95f};
        text(TITLE, U(30), tab_name(t), rail0 + U(26), y + cat_h / 2 - U(17), 0, cs);
        tab_rects_.push_back({rail0, y, rail1, y + cat_h});
    }
    hit_.push_back({rail0, cat_y, rail1, cat_y + tab_count() * cat_h});
    values_.push_back({-1, 0});
    {
        int x = rail0 + U(14), y = cat_y + tab_count() * cat_h + U(26);
        prompt(x, y, L"LB", L"Q", L"");
        prompt(x, y, L"RB", L"E", L"CATEGORY");
    }

    int n_opts = int(list.size()) - 2;
    if (rec_page) {
        int row_h = U(78);
        visible_records_ = std::max(3, (bottom - top - U(30)) / row_h);
        draw_records(list0 + U(22), list1 - U(22), top + U(30), row_h);
    } else {
        int row_h = U(58), y_rows = top + U(20);
        visible_opts_ = std::max(3, (bottom - y_rows) / row_h);
        int shown = std::min(n_opts, visible_opts_);
        scroll_ = std::clamp(scroll_, 0, std::max(0, n_opts - shown));
        int k = selected_ - 1;   // keep the selected option in view
        if (k >= 0 && k < n_opts) {
            if (k < scroll_) scroll_ = k;
            if (k >= scroll_ + shown) scroll_ = k - shown + 1;
        }
        int vw = U(wide ? 300.0f : 250.0f);
        for (int i = 1; i <= n_opts; i++) {
            int slot = i - 1 - scroll_;
            if (slot < 0 || slot >= shown) {   // scrolled out of view: no hit area
                hit_.push_back(RECT{});
                values_.push_back({-1, 0});
                continue;
            }
            const Item& it = list[i];
            bool sel = i == selected_;
            int y = y_rows + slot * row_h;
            if (sel) {
                hgrad(list0, y, list1, y + row_h, AMBER, it.enabled ? 0.30f : 0.15f, it.enabled ? 0.12f : 0.06f);
                frame(list0, y, list1, y + row_h, std::max(1, U(1.5f)), AMBER, 0.6f);
            } else {
                fill(list0 + U(10), y + row_h - std::max(1, U(1)), list1 - U(10), y + row_h, 0xFFFFFF, 0.06f);
            }
            const Style ls = !it.enabled ? Style{DIM, DIM, INK, U(2), 0.9f}
                             : sel       ? Style{WHITE_TOP, WHITE_BOTTOM, INK, U(2)}
                                         : Style{PALE_TOP, PALE_BOTTOM, INK, U(2), 0.95f};
            text(ITEM, U(29), it.label, list0 + U(22), y + row_h / 2 - U(16), 0, ls);
            int vx1 = list1 - U(22), vx0 = vx1 - vw;
            draw_value(it, sel, vx0, vx1, y, row_h);
            hit_.push_back({list0, y, list1, y + row_h});
            values_.push_back({vx0, (vx0 + vx1) / 2});
        }
        if (scroll_ > 0) {   // more rows above / below: arrows past the right edge, clickable
            triangle(list1 + U(15), y_rows + U(12), U(18), -2, GOLD_BOTTOM, 1);
            more_up_ = {list1, y_rows - U(10), list1 + U(30), y_rows + U(34)};
        }
        if (scroll_ + shown < n_opts) {
            int yb = y_rows + shown * row_h - U(12);
            triangle(list1 + U(15), yb, U(18), 2, GOLD_BOTTOM, 1);
            more_down_ = {list1, yb - U(22), list1 + U(30), yb + U(10)};
        }
    }
    hit_.push_back(RECT{});   // BACK: its key help at the bottom (below)
    values_.push_back({-1, 0});

    // The panel about the selected row
    fill(side0, top, side1, bottom, PANEL, 0.8f);
    frame(side0, top, side1, bottom, std::max(1, U(1.5f)), AMBER, 0.28f);
    int px0 = side0 + U(28), px1 = side1 - U(28), y = top + U(24);
    const Item* cur = selected_ < int(list.size()) ? &list[selected_] : nullptr;
    bool option = cur && cur->id >= OPT_DISPLAY;
    std::wstring title = option ? cur->label : cur && cur->id == BACK ? cur->label : std::wstring(tab_name(tab_));
    text(TITLE, U(40), title, px0, y, 0, {WHITE_TOP, WHITE_BOTTOM, INK, U(2)});
    y += U(70);
    bool crosshair_row = option && (cur->id == OPT_CROSS || cur->id == OPT_CROSS_STYLE || cur->id == OPT_CROSS_P1 ||
                                    cur->id == OPT_CROSS_P2);
    if (crosshair_row) {
        int ph = (px1 - px0) * 9 / 16;
        crosshair_preview(px0, y, px1, y + ph);
        frame(px0, y, px1, y + ph, std::max(1, U(1)), 0x3A3A34, 1);
        y += ph + U(22);
    } else if (option) {
        const Picture& pic = preview(cur->id);
        if (pic.w > 0) {
            int ph = (px1 - px0) * 9 / 16;
            picture(pic, px0, y, px1, y + ph);
            frame(px0, y, px1, y + ph, std::max(1, U(1)), 0x3A3A34, 1);
            y += ph + U(22);
        }
    }
    if (cur)
        for (const std::wstring& line : wrap(BODY, U(23), rec_page && cur->id == TABS ? list[0].hint : cur->hint,
                                             px1 - px0)) {
            text(BODY, U(23), line, px0, y, 0, {0xD8D1BD, 0xD8D1BD});
            y += U(34);
        }
    if (option && cur->enabled && cur->id != OPT_DISPLAY && cur->kind != BUTTON) {   // its default and when it takes effect
        y += U(18);
        const std::wstring& def = default_value(cur->id);
        auto meta = [&](const wchar_t* label, const std::wstring& value) {
            text(ITEM, U(21), label, px0, y, 0, {FOOT, FOOT});
            text(ITEM, U(21), value, px0 + U(130), y, 0, {PALE_TOP, PALE_BOTTOM});
            y += U(36);
        };
        meta(L"DEFAULT", def);
        meta(L"APPLIES", cur->id == OPT_AA ? L"NEXT START" : L"AT ONCE");
    }

    // Key help
    vgrad(0, h - U(120), w, h, 0x000000, 0.0f, 0.85f);
    int x = U(80), py = h - U(64);
    if (rec_page) {
        prompt(x, py, L"LB", L"Q", L"");
        prompt(x, py, L"RB", L"E", L"TAB");
    } else {
        prompt(x, py, L"A", L"<>", L"CHANGE");
        reset_rect_ = prompt(x, py, L"Y", L"R", L"RESET TO DEFAULTS");
    }
    back_rect_ = prompt(x, py, L"B", L"ESC", L"BACK");
    hit_.back() = {back_rect_.left - U(8), back_rect_.top - U(8), back_rect_.right + U(8), back_rect_.bottom + U(8)};
    if (selected_ == int(list.size()) - 1) frame(hit_.back().left, hit_.back().top, hit_.back().right,
                                                 hit_.back().bottom, std::max(1, U(2)), AMBER, 0.9f);
}

// Both players' crosshairs at the size the game draws them (pictures at their
// pixels at 1080p, or the rings), side by side over a stretch of the game
void Frontend::crosshair_preview(int x0, int y0, int x1, int y1) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    if (!crosshair_loaded_) {
        for (int p = 0; p < 2; p++) {
            Picture& c = crosshair_[p];
            std::vector<uint8_t> rgba;
            c = Picture{};
            if (!crosshair_load(p, c.w, c.h, rgba) || rgba.size() < size_t(c.w) * c.h * 4) {
                c.w = c.h = 0;
                continue;
            }
            c.px.resize(size_t(c.w) * c.h);
            for (size_t i = 0; i < c.px.size(); i++)
                c.px[i] = uint32_t(rgba[i * 4 + 3]) << 24 | uint32_t(rgba[i * 4]) << 16 | uint32_t(rgba[i * 4 + 1]) << 8 |
                          rgba[i * 4 + 2];
        }
        crosshair_loaded_ = true;
    }
    const Picture& back = preview(OPT_CROSS);
    if (back.w > 0) picture(back, x0, y0, x1, y1);
    else fill(x0, y0, x1, y1, 0x1A2016, 1);
    float k = h_ / 1080.0f;
    for (int p = 0; p < 2; p++) {
        int cx = x0 + (x1 - x0) * (p + 1) / 3, cy = (y0 + y1) / 2;
        const Picture& c = crosshair_[p];
        if (settings.crosshair_style && c.w > 0) {   // its own picture, centred, nearest pixel
            int dw = std::max(1, int(c.w * k + 0.5f)), dh = std::max(1, int(c.h * k + 0.5f));
            for (int j = 0; j < dh; j++)
                for (int i = 0; i < dw; i++) {
                    uint32_t q = c.px[size_t(j * c.h / dh) * c.w + size_t(i * c.w / dw)];
                    int x = cx - dw / 2 + i, y = cy - dh / 2 + j;
                    if (x >= x0 && x < x1 && y >= y0 && y < y1) blend(x, y, q & 0xFFFFFF, float(q >> 24) / 255.0f);
                }
        } else {   // the game's ring: yellow for player 1, cyan for player 2
            float r = std::max(10.0f, h_ / 60.0f), t = std::max(2.0f, r / 5);
            uint32_t col = p ? 0x33E6FF : 0xFFF233;
            int b = int(r + t) + 2;
            for (int y = cy - b; y <= cy + b; y++)
                for (int x = cx - b; x <= cx + b; x++) {
                    float d = std::sqrt(float((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                    float cov = std::clamp(t * 0.5f - std::abs(d - r) + 0.5f, 0.0f, 1.0f);
                    if (cov > 0) blend(x, y, col, cov);
                }
        }
        text(ITEM, U(20), p ? L"PLAYER 2" : L"PLAYER 1", cx, y1 - U(34), 1, {WHITE_TOP, WHITE_BOTTOM, INK, U(2)});
    }
}

bool Frontend::draw(int w, int h, std::vector<uint32_t>& out) {
    if (!active() || w <= 0 || h <= 0) return false;
    if (!dirty_ && w == w_ && h == h_ && out.size() == size_t(w) * h) return false;
    if (w != w_ || h != h_) {
        w_ = w;
        h_ = h;
        u_ = std::min(h / 1080.0f, w / 1440.0f);
        bg_.clear();
        bg_stone_.clear();
    }
    partial_ = false;
    if (screen_ != Screen::GunTest) gun_page_.clear();
    else if (gun_page_.size() == size_t(w) * h && out.size() == gun_page_.size() && gun_page_key_ == gun_key()) {
        // Only the marks moved: put the page back where they were, draw them where they are
        px_ = out.data();
        int y0 = h, y1 = 0;
        auto restore = [&](const RECT& r) {
            int x0 = std::max(0, int(r.left)), x1 = std::min(w, int(r.right));
            int r0 = std::max(0, int(r.top)), r1 = std::min(h, int(r.bottom));
            if (x0 >= x1 || r0 >= r1) return;
            for (int y = r0; y < r1; y++)
                memcpy(&out[size_t(y) * w + x0], &gun_page_[size_t(y) * w + x0], size_t(x1 - x0) * 4);
            y0 = std::min(y0, r0), y1 = std::max(y1, r1);
        };
        for (const RECT& r : gun_marks_) restore(r);
        draw_gun_marks();
        for (const RECT& r : gun_marks_) y0 = std::min(y0, std::max(0, int(r.top))), y1 = std::max(y1, std::min(h, int(r.bottom)));
        partial_ = true;
        partial_y0_ = y0, partial_y1_ = std::max(y0, y1);
        dirty_ = false;
        return true;
    }
    bool over_game = this->over_game();
    bool page = screen_ == Screen::Options || screen_ == Screen::Records || screen_ == Screen::GunTest;
    if (over_game) out.assign(size_t(w) * h, 0);
    else {
        if (bg_.empty()) background();
        out = has_poster() && page ? bg_stone_ : bg_;
    }
    px_ = out.data();
    hit_.clear();
    values_.clear();
    more_up_ = more_down_ = back_rect_ = reset_rect_ = coffee_rect_ = RECT{};
    auto list = items();
    if (screen_ == Screen::GunTest) {
        draw_gun_test(over_game);
        gun_page_ = out;   // without the marks
        gun_page_key_ = gun_key();
        draw_gun_marks();
    } else if (page) draw_options(over_game, list);
    else draw_menu(over_game, list);
    if (locked_notice_ && screen_ == Screen::Survival) draw_locked_notice();
    dirty_ = false;
    return true;
}

// SURVIVAL with every arena locked: how they unlock, over the list
void Frontend::draw_locked_notice() {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    fill(0, 0, w_, h_, 0x000000, 0.55f);
    int pw = U(900), ph = U(400), x0 = (w_ - pw) / 2, y0 = (h_ - ph) / 2, cx = w_ / 2;
    fill(x0, y0, x0 + pw, y0 + ph, PANEL, 0.96f);
    frame(x0, y0, x0 + pw, y0 + ph, std::max(1, U(2)), AMBER, 0.9f);
    text(display(), U(64), L"ARENAS LOCKED", cx, y0 + U(36), 1, {GOLD_TOP, GOLD_BOTTOM, EDGE, U(3), 1, U(4)});
    int y = y0 + U(140);
    for (const std::wstring& line : wrap(BODY, U(26),
             L"Each survival arena opens when you reach it in the arcade game. Play CLASSIC ARCADE: "
             L"when you get to one, ARENA UNLOCKED shows up and it is ready here.", pw - U(100))) {
        text(BODY, U(26), line, cx, y, 1, {0xE8C890, 0xE8C890});
        y += U(38);
    }
    text(ITEM, U(22), L"PRESS ENTER OR CLICK", cx, y0 + ph - U(56), 1, {HINT, HINT});
}

// ---- GUN TEST ----

namespace {
constexpr uint32_t P1_COLOUR = 0xFFF233, P2_COLOUR = 0x33E6FF, NO_PLAYER = 0x9A9484;   // the game's rings
}  // namespace

int Frontend::gun_player(const GunView& g, bool& picked) const {
    picked = !g.path.empty() && (g.path == settings.gun_p1 || g.path == settings.gun_p2);
    if (picked) return g.path == settings.gun_p1 ? 0 : 1;
    return g.player;
}

std::string Frontend::gun_key() const {
    std::string k = std::to_string(gun_step_) + "|" + settings.gun_p1 + "|" + settings.gun_p2 + "|" +
                    std::to_string(quiet_mice) + (settings.second_mouse ? "M" : "m") + (pad_prompts ? "P" : "p") +
                    (over_game() ? "G" : "g");
    for (const GunView& g : guns)
        k += "|" + g.path + char('0' + g.absolute) + char('0' + g.active) + char('0' + g.has_pos) + char('0' + g.trigger) +
             char('0' + g.reload) + std::to_string(g.player);
    return k;
}

// Each device's mark where it points: a ring in its player's colour (filled
// while it fires) and its number
void Frontend::draw_gun_marks() {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int w = w_, h = h_;
    gun_marks_.clear();
    for (int i = 0; i < int(guns.size()); i++) {
        const GunView& g = guns[i];
        if (!g.has_pos || g.x < 0 || g.y < 0 || g.x >= w || g.y >= h) continue;
        bool picked = false;
        int pl = gun_player(g, picked);
        uint32_t col = pl == 0 ? P1_COLOUR : pl == 1 ? P2_COLOUR : 0xFFFFFF;
        float r = std::max(12.0f, h / 45.0f), t = std::max(2.0f, r / 5);
        int b = int(r + t) + 2;
        for (int py = g.y - b; py <= g.y + b; py++)
            for (int px = g.x - b; px <= g.x + b; px++) {
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                float d = std::sqrt(float((px - g.x) * (px - g.x) + (py - g.y) * (py - g.y)));
                float cov = std::clamp(t * 0.5f - std::abs(d - r) + 0.5f, 0.0f, 1.0f);
                if (g.trigger) cov = std::max(cov, std::clamp(r - d, 0.0f, 1.0f) * 0.55f);   // filled while it fires
                if (cov > 0) blend(px, py, col, cov);
            }
        text(ITEM, U(24), std::to_wstring(i + 1), g.x + b + U(4), g.y - b - U(10), 0, {col, col, INK, U(2)});
        gun_marks_.push_back({g.x - b - 2, g.y - b - U(30), g.x + b + U(60), g.y + b + 2});
    }
}

void Frontend::gun_fired(int index) {
    if (screen_ != Screen::GunTest || index < 0 || index >= int(guns.size()) || guns[index].path.empty()) return;
    const std::string& path = guns[index].path;
    if (gun_step_ == 1) {
        settings.gun_p1 = path;
        settings.gun_p2.clear();
        gun_step_ = 2;
    } else if (gun_step_ == 2 && path != settings.gun_p1) {
        settings.gun_p2 = path;
        gun_step_ = 0;
        push(Action::SettingsChanged);
    }
    dirty_ = true;
}

// Each device a row (its number, name, kind, buttons and player) and a mark
// where it points; ENTER picks the players' guns by firing them
void Frontend::draw_gun_test(bool over_game) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int w = w_, h = h_;
    fill(0, 0, w, h, 0x000000, over_game ? 0.72f : 0.35f);
    bool wide = w / u_ >= 1800;
    int margin = U(wide ? 70.0f : 50.0f);
    text(display(), U(72), L"GUN TEST", margin + U(10), U(36), 0, {GOLD_TOP, GOLD_BOTTOM, EDGE, U(4), 1, U(4)});

    // What to do now
    int y = U(150);
    if (gun_step_) {
        text(TITLE, U(48), gun_step_ == 1 ? L"FIRE PLAYER 1'S GUN" : L"FIRE PLAYER 2'S GUN", margin + U(10), y, 0,
             {gun_step_ == 1 ? P1_COLOUR : P2_COLOUR, gun_step_ == 1 ? 0xE0C820u : 0x20B8D8u, INK, U(3)});
        y += U(64);
        text(BODY, U(23), gun_step_ == 1 ? L"Pull its trigger, wherever it points.  ESC: leave the guns as they were"
                                         : L"Now the other gun.  ESC: only one gun, player 2 joins with the next mouse to fire",
             margin + U(10), y, 0, {0xD8D1BD, 0xD8D1BD});
    } else {
        text(BODY, U(23), L"Move and fire each gun: its mark follows it, and its row lights up.  "
                          L"Each gun should move only its own mark.", margin + U(10), y + U(10), 0, {0xD8D1BD, 0xD8D1BD});
    }

    // The devices
    int list0 = margin, list1 = std::min(w - margin, list0 + U(1100));
    int row_h = U(86), top = U(270), bottom = h - U(250);
    int shown = std::max(1, (bottom - top) / row_h);
    for (int i = 0; i < int(guns.size()) && i < shown; i++) {
        const GunView& g = guns[i];
        bool picked = false;
        int pl = gun_player(g, picked);
        uint32_t col = pl == 0 ? P1_COLOUR : pl == 1 ? P2_COLOUR : NO_PLAYER;
        int ry = top + i * row_h;
        if (g.active) hgrad(list0, ry, list1, ry + row_h - U(8), col, 0.22f, 0.04f);
        else fill(list0, ry, list1, ry + row_h - U(8), PANEL, 0.6f);
        fill(list0, ry, list0 + std::max(3, U(5)), ry + row_h - U(8), col, g.active ? 1.0f : 0.5f);
        int cy = ry + (row_h - U(8)) / 2;
        // its number, as on its mark
        pill(list0 + U(22), cy - U(22), list0 + U(66), cy + U(22), col, 1);
        text(ITEM, U(26), std::to_wstring(i + 1), list0 + U(44), cy - U(15), 1, {0x111111, 0x111111});
        std::wstring name = g.name.size() > 40 ? g.name.substr(0, 39) + L"..." : g.name;
        text(ITEM, U(28), name, list0 + U(86), ry + U(8), 0,
             g.active ? Style{WHITE_TOP, WHITE_BOTTOM, INK, U(2)} : Style{PALE_TOP, PALE_BOTTOM, INK, U(2), 0.9f});
        std::wstring kind = g.absolute ? L"POINTS AT THE SCREEN (LIGHT GUN)" : L"MOVES THE POINTER (MOUSE)";
        if (!g.active) kind += L"  ·  IDLE";
        text(BODY, U(19), kind, list0 + U(86), ry + U(46), 0, {HINT, HINT});
        // its buttons and its player, at the right
        int x = list1 - U(24);
        std::wstring who = pl < 0 ? L"NO PLAYER" : std::wstring(pl ? L"PLAYER 2" : L"PLAYER 1") + (picked ? L"" : L" (AUTO)");
        int ww = measure(ITEM, U(24), who);
        text(ITEM, U(24), who, x, cy - U(14), 2, pl < 0 ? Style{DIM, DIM} : Style{col, col, INK, U(2)});
        x -= ww + U(36);
        for (int b = 1; b >= 0; b--) {   // TRIGGER, then RELOAD at its right
            const wchar_t* label = b ? L"RELOAD" : L"TRIGGER";
            bool on = b ? g.reload : g.trigger;
            int bw = measure(ITEM, U(19), label) + U(28);
            if (on) pill(x - bw, cy - U(17), x, cy + U(17), b ? 0xE8655A : 0x6FD17A, 1);
            else frame(x - bw, cy - U(17), x, cy + U(17), std::max(1, U(1.5f)), 0x5A574E, 1);
            text(ITEM, U(19), label, x - bw / 2, cy - U(11), 1, on ? Style{0x111111, 0x111111} : Style{DIM, DIM});
            x -= bw + U(12);
        }
    }
    if (guns.size() > size_t(shown))
        text(BODY, U(20), L"+ " + std::to_wstring(guns.size() - shown) + L" more", list0 + U(22), top + shown * row_h, 0,
             {HINT, HINT});

    // What it means: a single mouse, the option off, and how player 2 joins otherwise
    std::vector<std::wstring> notes;
    if (!settings.second_mouse) notes.push_back(L"SECOND MOUSE is OFF: every mouse and gun moves player 1's sight.");
    int count = int(guns.size());
    if (count == 0)
        notes.push_back(L"Nothing has moved yet: move a mouse or point a gun at the screen.");
    else if (count == 1)
        notes.push_back(L"Only one mouse has moved so far.  If two guns move this same mark, they reach the game as one: "
                        L"set each gun as a mouse of its own in its software.");
    else if (settings.gun_p1.empty() && settings.gun_p2.empty())
        notes.push_back(L"Nothing picked: in a game, the second mouse to fire becomes player 2's gun.  "
                        L"ENTER picks them here once and for all.");
    if (quiet_mice)
        notes.push_back(std::to_wstring(quiet_mice) + (quiet_mice == 1 ? L" more device Windows lists as a mouse has" : L" more devices Windows lists as mice have") +
                        (quiet_mice == 1 ? L" sent nothing (often a keyboard or a receiver): it shows up here once it moves."
                                         : L" sent nothing (often a keyboard or a receiver): they show up here once they move."));
    int ny = std::max(top + std::min(count, shown) * row_h + U(20), bottom - U(10));
    for (const std::wstring& note : notes)
        for (const std::wstring& line : wrap(BODY, U(22), note, list1 - list0 - U(20))) {
            text(BODY, U(22), line, list0 + U(10), ny, 0, {0xE8C890, 0xE8C890});
            ny += U(32);
        }

    // Key help
    vgrad(0, h - U(120), w, h, 0x000000, 0.0f, 0.85f);
    int x = U(80), py = h - U(64);
    if (!gun_step_) {
        prompt(x, py, L"A", L"ENTER", L"PICK THE GUNS");
        if (!settings.gun_p1.empty() || !settings.gun_p2.empty()) prompt(x, py, L"Y", L"R", L"AUTOMATIC");
        back_rect_ = prompt(x, py, L"B", L"ESC", L"BACK");
    } else {
        prompt(x, py, L"B", L"ESC", gun_step_ == 1 ? L"CANCEL" : L"ONLY ONE GUN");
    }
}
