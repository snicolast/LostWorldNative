#include "frontend.h"

#include "../runtime/bosses.h"
#include "../runtime/survival.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <thread>

#include <wincodec.h>

namespace {

// The game's letters: yellow at the top fading to orange, dark outline
constexpr uint32_t GOLD_TOP = 0xFFEA5C, GOLD_BOTTOM = 0xFF8A00, EDGE = 0x1C0A00;
constexpr uint32_t PALE_TOP = 0xE6DCC0, PALE_BOTTOM = 0xBFB294, DIM = 0x6A6559;
constexpr uint32_t AMBER = 0xFFA032, HINT = 0xC9B88E, FOOT = 0x8A8070;

float chan(uint32_t rgb, int shift) { return float((rgb >> shift) & 0xFF) / 255.0f; }

uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int s = 0; s <= 16; s += 8) {
        float v = chan(a, s) + (chan(b, s) - chan(a, s)) * t;
        r |= uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f) << s;
    }
    return r;
}

std::vector<char> read_file(const std::string& path) {
    std::vector<char> data;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        data.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        fclose(f);
    }
    return data;
}

const wchar_t* const FACES[4] = {L"Saira ExtraCondensed ExCond", L"Barlow Condensed", L"Share Tech Mono",
                                 L"Jurassic Park"};

}  // namespace

// ---- Settings file ----

void FrontendSettings::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], value[64];
        if (sscanf(line, " %63[^= ] = %63s", key, value) != 2) continue;
        std::string k = key, v = value;
        bool on = v == "1" || v == "on" || v == "true";
        if (k == "fullscreen") fullscreen = on;
        else if (k == "volume") volume = std::clamp(atoi(value), 0, 10);
        else if (k == "speakers") speakers = v == "surround" ? 1 : v == "headphones" ? 2 : 0;
        else if (k == "rear_boost") rear_boost = std::clamp(atoi(value) / 3 * 3, 0, 12);
        else if (k == "sample_interpolation") cubic_samples = v != "linear";
        else if (k == "aspect") aspect = v == "4:3" ? 0 : v == "21:9" ? 2 : 1;
        else if (k == "aa") aa = v == "dlaa" ? 1 : (v == "none" || v == "off") ? 2 : 0;
        else if (k == "haze") haze = on;
        else if (k == "bloom") bloom = v == "low" ? 1 : v == "medium" ? 2 : v == "high" ? 3 : on ? 2 : 0;
        else if (k == "ao") ao = on;
        else if (k == "sky") sky = v == "procedural";
        else if (k == "sun") sun = on;
        else if (k == "shadows") shadows = on;
        else if (k == "ray_tracing") ray_tracing = v == "shadows" ? 1 : v == "off" || v == "0" ? 0 : 2;
        else if (k == "light_shafts") shafts = on;
        else if (k == "soft_particles") soft = on;
        else if (k == "lightning") lightning = on;
        else if (k == "rain") rain = v == "light" ? 1 : v == "heavy" ? 2 : on ? 2 : 0;
        else if (k == "hdr") hdr = on;
        else if (k == "rim_light") rim = on;
        else if (k == "surface_detail") relief = on;
        else if (k == "sharpen") sharpen = on;
        else if (k == "flashlight") torch = v != "original";
        else if (k == "hd_text") hd_text = on;
        else if (k == "crosshair") crosshair = on;
        else if (k == "crosshair_hide") crosshair_hide = std::clamp(atoi(value), 0, 10);
        else if (k == "free_play") free_play = on;
        else if (k == "motion") smooth = v != "original";
        else if (k == "second_mouse") second_mouse = on;
        else if (k == "subtitles") subtitles = v == "japanese" ? 2 : v == "english" ? 1 : 0;
        else if (k == "saturation") saturation = std::clamp(atoi(value), 0, 200);
        else if (k == "gamma") gamma = std::clamp(int(atof(value) * 100 + 0.5), 50, 150);
        else if (k == "pad_speed") pad_speed = std::clamp(atoi(value), 1, 10);
        else if (k == "gyro") gyro = on;
        else if (k == "gyro_speed") gyro_speed = std::clamp(atoi(value), 1, 10);
        else if (k == "vibration") vibration = v == "low" ? 1 : v == "medium" ? 2 : v == "high" ? 3 : 0;
        else if (k == "aim_assist") aim_assist = v == "low" ? 1 : v == "medium" ? 2 : v == "high" ? 3 : 0;
    }
    fclose(f);
}

void FrontendSettings::save(const char* path) const {
    FILE* f = fopen(path, "w");
    if (!f) return;
    static const char* const AA[3] = {"msaa", "dlaa", "none"};
    static const char* const SUBS[3] = {"off", "english", "japanese"};
    static const char* const BLOOM[4] = {"off", "low", "medium", "high"};   // also the vibration and aim assist levels
    static const char* const RAIN[3] = {"off", "light", "heavy"};
    static const char* const SPEAKERS[3] = {"stereo", "surround", "headphones"};
    static const char* const RT_MODES[3] = {"off", "shadows", "ao"};
    static const char* const ASPECTS[3] = {"4:3", "16:9", "21:9"};
    fprintf(f, "fullscreen = %d\nvolume = %d\naa = %s\nhaze = %d\nbloom = %s\nao = %d\nsky = %s\nsun = %d\nshadows = %d\nlight_shafts = %d\nsoft_particles = %d\nhdr = %d\nrim_light = %d\nsurface_detail = %d\nsharpen = %d\nlightning = %d\nrain = %s\nhd_text = %d\ncrosshair = %d\n"
               "second_mouse = %d\nsubtitles = %s\nsaturation = %d\ngamma = %.2f\nmotion = %s\nspeakers = %s\nrear_boost = %d\nsample_interpolation = %s\n"
               "pad_speed = %d\ngyro = %d\ngyro_speed = %d\nvibration = %s\naim_assist = %s\nflashlight = %s\nray_tracing = %s\n"
               "crosshair_hide = %d\nfree_play = %d\naspect = %s\n",
            fullscreen, volume, AA[aa], haze, BLOOM[bloom], ao, sky ? "procedural" : "original", sun, shadows, shafts, soft, hdr, rim, relief, sharpen, lightning, RAIN[rain], hd_text, crosshair, second_mouse, SUBS[subtitles], saturation,
            gamma / 100.0, smooth ? "smooth" : "original", SPEAKERS[speakers], rear_boost, cubic_samples ? "cubic" : "linear",
            pad_speed, gyro, gyro_speed, BLOOM[vibration], BLOOM[aim_assist], torch ? "modern" : "original", RT_MODES[ray_tracing],
            crosshair_hide, free_play, ASPECTS[aspect]);
    fclose(f);
}

// ---- Setup ----

bool Frontend::init(const std::string& font_dir) {
    // The last one is optional (free for personal use, not in the repository)
    static const char* const FILES[4] = {"SairaExtraCondensed-Bold.ttf", "BarlowCondensed-Bold.ttf",
                                         "ShareTechMono-Regular.ttf", "Jurassic_Park.ttf"};
    bool ok = true;
    for (const char* name : FILES) {
        font_data_.push_back(read_file(font_dir + "/" + name));
        auto& data = font_data_.back();
        DWORD count = 0;
        HANDLE h = data.empty() ? nullptr : AddFontMemResourceEx(data.data(), DWORD(data.size()), nullptr, &count);
        if (h) {
            font_handles_.push_back(h);
            if (name == FILES[3]) jurassic_ = true;
        } else if (name != FILES[3]) {
            fprintf(stderr, "[menu] could not load the font %s/%s\n", font_dir.c_str(), name);
            ok = false;
        }
    }
    dc_ = CreateCompatibleDC(nullptr);
    for (const wchar_t* p : {L"third_party/menu/cover.png", L"third_party/menu/cover.jpg", L"third_party/menu/lw_poster.jpg"})
        if (load_poster(p)) {
            printf("[menu] poster %ls (%dx%d)\n", p, poster_w_, poster_h_);
            break;
        }
    return ok;
}

// Any picture format Windows can read (WIC), as BGRA
bool Frontend::load_poster(const std::wstring& path) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0;
    if (ok) {
        poster_.resize(size_t(w) * h);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(poster_.size() * 4),
                                        reinterpret_cast<BYTE*>(poster_.data())));
    }
    for (IUnknown* p : std::initializer_list<IUnknown*>{conv, frame, dec, wic})
        if (p) p->Release();
    if (!ok) {
        poster_.clear();
        return false;
    }
    poster_w_ = int(w);
    poster_h_ = int(h);
    return true;
}

void Frontend::shutdown() {
    for (auto& [key, f] : fonts_) DeleteObject(f);
    fonts_.clear();
    for (HANDLE h : font_handles_) RemoveFontMemResourceEx(h);
    font_handles_.clear();
    if (dc_) DeleteDC(dc_);
    dc_ = nullptr;
}

// ---- Menu contents and navigation ----

const wchar_t* const Frontend::TAB_NAMES[Frontend::TAB_COUNT] = {L"DISPLAY", L"LIGHTING", L"ATMOSPHERE", L"AUDIO",
                                                                  L"CONTROLS", L"GAME"};

// Every settings option: its tab and, within the tab, the order shown. This is
// the one place to put a new option (besides `all` in items()); an option
// missing here goes at the end of GAME. Tabs past VISIBLE_OPTIONS rows scroll.
const Frontend::OptionTab Frontend::OPTION_TABS[] = {
    {OPT_DISPLAY, DISPLAY_TAB}, {OPT_ASPECT, DISPLAY_TAB}, {OPT_MOTION, DISPLAY_TAB}, {OPT_AA, DISPLAY_TAB}, {OPT_TEXT, DISPLAY_TAB},
    {OPT_SAT, DISPLAY_TAB}, {OPT_GAMMA, DISPLAY_TAB}, {OPT_SHARPEN, DISPLAY_TAB},
    {OPT_SUN, LIGHTING_TAB}, {OPT_SHADOWS, LIGHTING_TAB}, {OPT_RT, LIGHTING_TAB}, {OPT_SHAFTS, LIGHTING_TAB}, {OPT_HDR, LIGHTING_TAB},
    {OPT_AO, LIGHTING_TAB}, {OPT_BLOOM, LIGHTING_TAB}, {OPT_RIM, LIGHTING_TAB}, {OPT_RELIEF, LIGHTING_TAB}, {OPT_TORCH, LIGHTING_TAB},
    {OPT_SKY, ATMOSPHERE_TAB}, {OPT_HAZE, ATMOSPHERE_TAB}, {OPT_LIGHTNING, ATMOSPHERE_TAB},
    {OPT_RAIN, ATMOSPHERE_TAB}, {OPT_SOFT, ATMOSPHERE_TAB},
    {OPT_VOLUME, AUDIO_TAB}, {OPT_SPEAKERS, AUDIO_TAB}, {OPT_REAR, AUDIO_TAB}, {OPT_SAMPLES, AUDIO_TAB},
    {OPT_PAD_SPEED, CONTROLS_TAB}, {OPT_GYRO, CONTROLS_TAB}, {OPT_GYRO_SPEED, CONTROLS_TAB},
    {OPT_VIBRATION, CONTROLS_TAB}, {OPT_ASSIST, CONTROLS_TAB}, {OPT_CROSS, CONTROLS_TAB}, {OPT_CROSS_HIDE, CONTROLS_TAB},
    {OPT_MICE, CONTROLS_TAB},
    {OPT_FREE_PLAY, GAME_TAB}, {OPT_SUBS, GAME_TAB},
};

const wchar_t* const Frontend::RECORD_TAB_NAMES[Frontend::RECORD_TAB_COUNT] = {L"ACHIEVEMENTS", L"STATS"};

int Frontend::tab_of(Id id) {
    for (const OptionTab& o : OPTION_TABS)
        if (o.id == id) return o.tab;
    return GAME_TAB;
}

void Frontend::set_tab(int t) {
    tab_ = (t + tab_count()) % tab_count();
    scroll_ = 0;
    record_scroll_ = 0;
    selected_ = std::min(selected_, int(items().size()) - 1);
    hovered_ = -1;
    dirty_ = true;
}

std::vector<Frontend::Item> Frontend::items() const {
    auto on_off = [](bool v) { return std::wstring(v ? L"ON" : L"OFF"); };
    static const wchar_t* const AA[3] = {L"MSAA 4X", L"DLAA", L"OFF"};
    static const wchar_t* const BLOOM_LEVELS[4] = {L"OFF", L"LOW", L"MEDIUM", L"HIGH"};
    static const wchar_t* const RAIN_LEVELS[3] = {L"OFF", L"LIGHT", L"HEAVY"};
    static const wchar_t* const SUBS[3] = {L"OFF", L"ENGLISH", L"JAPANESE"};
    static const wchar_t* const SPEAKERS[3] = {L"STEREO", L"SURROUND 4.0", L"HEADPHONES"};
    static const wchar_t* const SPEAKER_HINTS[3] = {
        L"Front and rear sound mixed on two speakers",
        L"Rear sound on your rear speakers, as on the cabinet (4.0, 5.1 or 7.1)",
        L"Two ears: gentle crossfeed, and the rear sound placed behind you"};
    switch (screen_) {
    case Screen::Main:
        return {{PLAY, L"PLAY", L"", L"Start a game"},
                {OPTIONS, L"OPTIONS", L"", L"Display, sound and graphics"},
                {RECORDS, L"RECORDS", L"", L"Achievements and your statistics"},
                {QUIT, L"QUIT", L"", L"Close the game"}};
    case Screen::Play:
        return {{ARCADE, L"CLASSIC ARCADE", L"", L"The original arcade game.  Insert a coin with 5 and press 1, or START on a gamepad"},
                {BOSS, L"BOSS FIGHT", L"", L"Pick a boss and go straight to the fight"},
                {SURVIVAL, L"SURVIVAL", L"", L"Hold one spot against endless waves of dinosaurs"},
                {BACK, L"BACK", L"", L"Return to the main menu"}};
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
            std::wstring value = rec ? L"WAVE " + std::to_wstring(wave) : L"NEW";
            std::wstring hint = where + (rec ? L".  Your record: wave " + std::to_wstring(wave) + L", " +
                                                   std::to_wstring(score) + L" points"
                                             : L".  No record yet");
            list.push_back({SURVIVAL_ENTRY, z.name, value, hint, true, i});
        }
        list.push_back({BACK, L"BACK", L"", L"Return to the mode select"});
        return list;
    }
    case Screen::Options: {
        std::wstring aa_hint = settings.aa != aa_at_launch
                                   ? L"Edge smoothing.  Takes effect the next time the game starts"
                                   : L"Edge smoothing (MSAA, NVIDIA DLAA or off).  Applies on the next start";
        wchar_t gamma_text[16];
        swprintf(gamma_text, 16, L"%.2f", settings.gamma / 100.0);
        static const wchar_t* const RT_MODES[3] = {L"OFF", L"SHADOWS", L"SHADOWS + AO"};
        static const wchar_t* const ASPECTS[3] = {L"4:3 (ORIGINAL)", L"16:9", L"21:9"};
        static const wchar_t* const RT_HINTS[3] = {
            L"Shadows from a shadow map and occlusion from the depth, as without ray tracing",
            L"Sun shadows traced through the world: every tree and dinosaur, soft with distance",
            L"Also the ambient occlusion: corners and the ground under things, even off screen"};
        std::wstring pads = gamepads.empty() ? L"No gamepad connected" : L"Connected: " + gamepads;
        std::vector<Item> all = {{OPT_DISPLAY, L"DISPLAY", settings.fullscreen ? L"FULLSCREEN" : L"WINDOW",
                 L"Window or full screen (also F11 or Alt+Enter)"},
                {OPT_ASPECT, L"WIDESCREEN", ASPECTS[settings.aspect],
                 L"Widest 3D view (21:9 for ultrawide monitors).  The HUD stays in the centre"},
                {OPT_MOTION, L"MOTION", settings.smooth ? L"SMOOTH" : L"ORIGINAL",
                 L"Smooth: the 3D is also drawn between the game's 57.5 frames, at your monitor's refresh rate"},
                {OPT_VOLUME, L"VOLUME", std::to_wstring(settings.volume), L"Sound volume"},
                {OPT_SPEAKERS, L"SPEAKERS", SPEAKERS[settings.speakers], SPEAKER_HINTS[settings.speakers]},
                {OPT_REAR, L"REAR BOOST", settings.rear_boost ? L"+" + std::to_wstring(settings.rear_boost) + L" DB" : std::wstring(L"OFF"),
                 L"Louder rear speakers, as the cabinet's seat speakers: T-Rex roars and many effects"},
                {OPT_SAMPLES, L"SAMPLE QUALITY", settings.cubic_samples ? L"SMOOTH" : L"ORIGINAL",
                 L"Smooth interpolation of the instrument and voice samples, or the original chip's"},
                {OPT_AA, L"ANTI-ALIASING", AA[settings.aa], aa_hint},
                {OPT_HAZE, L"DISTANCE HAZE", on_off(settings.haze),
                 L"Far scenery fades into the sky instead of appearing suddenly"},
                {OPT_BLOOM, L"BLOOM", BLOOM_LEVELS[settings.bloom], L"Soft glow around fire, lights, the sky and sunlit surfaces"},
                {OPT_AO, L"AMBIENT OCCLUSION", on_off(settings.ao),
                 L"Soft shadows in corners, creases and on the ground around things"},
                {OPT_SKY, L"SKY", settings.sky ? L"PROCEDURAL" : L"ORIGINAL",
                 L"Smooth drifting clouds drawn at full resolution, or the original picture"},
                {OPT_SUN, L"SUN", on_off(settings.sun), L"A sun in the sky and stronger, warmer sunlight outdoors"},
                {OPT_SHADOWS, L"SUN SHADOWS", on_off(settings.shadows),
                 L"Dinosaurs, trees and buildings cast shadows away from the sun"},
                {OPT_RT, L"RAY TRACING", !rt_available ? L"N/A" : RT_MODES[settings.ray_tracing],
                 !rt_available ? L"Needs a graphics card with ray tracing (NVIDIA RTX, AMD RX 6000 or newer)"
                               : RT_HINTS[settings.ray_tracing],
                 rt_available},
                {OPT_SHAFTS, L"LIGHT SHAFTS", on_off(settings.shafts),
                 L"Rays of sunlight streaming between trees and silhouettes"},
                {OPT_SOFT, L"SOFT PARTICLES", on_off(settings.soft),
                 L"Smoke, fire and explosions blend softly where they touch the ground and walls"},
                {OPT_HDR, L"HDR LIGHTING", on_off(settings.hdr),
                 L"Bright light keeps its detail instead of clipping to white; exposure adapts gently"},
                {OPT_RIM, L"RIM LIGHT", on_off(settings.rim),
                 L"A soft edge of light around dinosaurs and people, lifting them off the background"},
                {OPT_RELIEF, L"SURFACE DETAIL", on_off(settings.relief),
                 L"Light and shadow bring out the grain of stone, bark, metal and skin"},
                {OPT_TORCH, L"FLASHLIGHT", settings.torch ? L"MODERN" : L"ORIGINAL",
                 L"At night: a soft beam with real shading and a glow in the air, or the original spot"},
                {OPT_LIGHTNING, L"LIGHTNING", on_off(settings.lightning),
                 L"Flashes of lightning in stormy fog, with the trees standing out against it"},
                {OPT_RAIN, L"RAIN", RAIN_LEVELS[settings.rain],
                 L"Where it rains in the game: a dense fall of drops, wet ground, puddles and ripples"},
                {OPT_SAT, L"COLOR SATURATION", std::to_wstring(settings.saturation) + L"%",
                 L"How vivid the colours are (100% = original, 80% by default)"},
                {OPT_GAMMA, L"GAMMA", gamma_text, L"Brightness of the mid-tones (1.00 = original)"},
                {OPT_SHARPEN, L"SHARPEN", on_off(settings.sharpen),
                 L"Brings back texture detail after the edge smoothing, without halos"},
                {OPT_TEXT, L"HD TEXT", on_off(settings.hd_text),
                 L"Sharp fonts for menus, captions and score instead of the original pixels"},
                {OPT_CROSS, L"CROSSHAIR", on_off(settings.crosshair), L"Extra crosshair where the mouse points"},
                {OPT_CROSS_HIDE, L"HIDE IDLE CROSSHAIR",
                 settings.crosshair_hide ? L"AFTER " + std::to_wstring(settings.crosshair_hide) + L" S" : std::wstring(L"NEVER"),
                 L"A crosshair whose gun has not moved or fired for a while disappears until it moves again"},
                {OPT_FREE_PLAY, L"FREE PLAY", on_off(settings.free_play),
                 L"Start and continue with 1 / 2 or a light gun's start button, no coin needed"},
                {OPT_MICE, L"SECOND MOUSE", settings.second_mouse ? L"PLAYER 2" : L"OFF",
                 L"A second mouse joins as player 2 with a click while player 1 plays"},
                {OPT_SUBS, L"SUBTITLES", SUBS[settings.subtitles], L"Captions of what the characters say"},
                {OPT_PAD_SPEED, L"STICK SPEED", std::to_wstring(settings.pad_speed),
                 L"How fast the gamepad's sticks move the sight.  " + pads},
                {OPT_GYRO, L"GYRO AIM", on_off(settings.gyro),
                 gyro_pad ? L"Point the gamepad to aim, together with the sticks"
                          : L"Point the gamepad to aim (DualSense, DualShock 4, Switch Pro).  None connected"},
                {OPT_GYRO_SPEED, L"GYRO SPEED", std::to_wstring(settings.gyro_speed),
                 L"How far the sight moves when you turn the gamepad"},
                {OPT_VIBRATION, L"VIBRATION", BLOOM_LEVELS[settings.vibration],
                 L"The gamepad rumbles when you shoot and when you are hit"},
                {OPT_ASSIST, L"AIM ASSIST", BLOOM_LEVELS[settings.aim_assist],
                 L"With a gamepad the sight slows down over the dinosaurs and follows them a little"}};
        std::vector<Item> list = {{TABS, L"", L"", L"Left / Right here, Q / E or L1 / R1 change the tab"}};
        for (const OptionTab& o : OPTION_TABS)   // the tab's options in their order, then any not listed
            for (const Item& it : all)
                if (it.id == o.id && o.tab == tab_) list.push_back(it);
        for (const Item& it : all)
            if (tab_ == GAME_TAB && std::none_of(std::begin(OPTION_TABS), std::end(OPTION_TABS),
                                                 [&](const OptionTab& o) { return o.id == it.id; }))
                list.push_back(it);
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
    screen_ = s;
    selected_ = 0;
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

void Frontend::select(int index) {
    if (index != selected_) dirty_ = true;
    selected_ = index;
}

void Frontend::activate(const Item& it, int dir) {
    dirty_ = true;
    switch (it.id) {
    case PLAY: open(Screen::Play); break;
    case OPTIONS: options_from_pause_ = false; tab_ = 0; open(Screen::Options); break;
    case RECORDS: tab_ = 0; record_scroll_ = 0; open(Screen::Records); break;
    case QUIT: push(Action::Quit); break;
    case QUIT_TO_MENU: push(Action::QuitToMenu); break;
    case ARCADE: push(Action::StartArcade); break;
    case BOSS: open_boss_list(false); break;
    case SURVIVAL: open(Screen::Survival); selected_ = survival_choice; break;
    case SURVIVAL_ENTRY: survival_choice = it.index; push(Action::StartSurvival); break;
    case BOSS_ENTRY: boss_choice = it.index; push(Action::StartBoss); break;
    case PREPARE: push(Action::PrepareBosses); break;
    case RETRY: push(Action::StartBoss); break;
    case BOSS_LIST: open_boss_list(true); break;
    case LEAVE_BOSS: push(Action::LeaveBossMode); break;
    case BACK: back(); break;
    case RESUME: push(Action::Resume); break;
    case SETTINGS: options_from_pause_ = true; tab_ = 0; open(Screen::Options); break;
    case OPT_DISPLAY: push(Action::ToggleFullscreen); break;
    case OPT_VOLUME:
        settings.volume = dir > 0 && settings.volume == 10 && wrap_ ? 0 : std::clamp(settings.volume + dir, 0, 10);
        push(Action::SettingsChanged);
        break;
    case OPT_SPEAKERS: settings.speakers = (settings.speakers + dir + 3) % 3; push(Action::SettingsChanged); break;
    case OPT_REAR: settings.rear_boost = (settings.rear_boost + 3 * dir + 15) % 15; push(Action::SettingsChanged); break;
    case OPT_SAMPLES: settings.cubic_samples = !settings.cubic_samples; push(Action::SettingsChanged); break;
    case OPT_ASPECT: settings.aspect = (settings.aspect + dir + 3) % 3; push(Action::SettingsChanged); break;
    case OPT_AA: settings.aa = (settings.aa + dir + 3) % 3; push(Action::SettingsChanged); break;
    case OPT_HAZE: settings.haze = !settings.haze; push(Action::SettingsChanged); break;
    case OPT_BLOOM: settings.bloom = (settings.bloom + dir + 4) % 4; push(Action::SettingsChanged); break;
    case OPT_AO: settings.ao = !settings.ao; push(Action::SettingsChanged); break;
    case OPT_SKY: settings.sky = !settings.sky; push(Action::SettingsChanged); break;
    case OPT_SUN: settings.sun = !settings.sun; push(Action::SettingsChanged); break;
    case OPT_SHADOWS: settings.shadows = !settings.shadows; push(Action::SettingsChanged); break;
    case OPT_RT: settings.ray_tracing = (settings.ray_tracing + dir + 3) % 3; push(Action::SettingsChanged); break;
    case OPT_SHAFTS: settings.shafts = !settings.shafts; push(Action::SettingsChanged); break;
    case OPT_SOFT: settings.soft = !settings.soft; push(Action::SettingsChanged); break;
    case OPT_HDR: settings.hdr = !settings.hdr; push(Action::SettingsChanged); break;
    case OPT_RIM: settings.rim = !settings.rim; push(Action::SettingsChanged); break;
    case OPT_RELIEF: settings.relief = !settings.relief; push(Action::SettingsChanged); break;
    case OPT_SHARPEN: settings.sharpen = !settings.sharpen; push(Action::SettingsChanged); break;
    case OPT_TORCH: settings.torch = !settings.torch; push(Action::SettingsChanged); break;
    case OPT_LIGHTNING: settings.lightning = !settings.lightning; push(Action::SettingsChanged); break;
    case OPT_RAIN: settings.rain = (settings.rain + dir + 3) % 3; push(Action::SettingsChanged); break;
    case OPT_TEXT: settings.hd_text = !settings.hd_text; push(Action::SettingsChanged); break;
    case OPT_CROSS: settings.crosshair = !settings.crosshair; push(Action::SettingsChanged); break;
    case OPT_MOTION: settings.smooth = !settings.smooth; push(Action::SettingsChanged); break;
    case OPT_CROSS_HIDE: {   // never, 3, 5, 10 seconds
        static const int STEPS[4] = {0, 3, 5, 10};
        int i = int(std::find(std::begin(STEPS), std::end(STEPS), settings.crosshair_hide) - std::begin(STEPS)) % 4;
        settings.crosshair_hide = STEPS[(i + dir + 4) % 4];
        push(Action::SettingsChanged);
        break;
    }
    case OPT_FREE_PLAY: settings.free_play = !settings.free_play; push(Action::SettingsChanged); break;
    case OPT_MICE: settings.second_mouse = !settings.second_mouse; push(Action::SettingsChanged); break;
    case OPT_SUBS: settings.subtitles = (settings.subtitles + dir + 3) % 3; push(Action::SettingsChanged); break;
    case OPT_PAD_SPEED:
        settings.pad_speed = dir > 0 && settings.pad_speed == 10 && wrap_ ? 1 : std::clamp(settings.pad_speed + dir, 1, 10);
        push(Action::SettingsChanged);
        break;
    case OPT_GYRO: settings.gyro = !settings.gyro; push(Action::SettingsChanged); break;
    case OPT_GYRO_SPEED:
        settings.gyro_speed = dir > 0 && settings.gyro_speed == 10 && wrap_ ? 1 : std::clamp(settings.gyro_speed + dir, 1, 10);
        push(Action::SettingsChanged);
        break;
    case OPT_VIBRATION: settings.vibration = (settings.vibration + dir + 4) % 4; push(Action::SettingsChanged); break;
    case OPT_ASSIST: settings.aim_assist = (settings.aim_assist + dir + 4) % 4; push(Action::SettingsChanged); break;
    case OPT_SAT:   // 0..200 % in steps of 10; a click past the top starts again from the bottom
        settings.saturation = dir > 0 && settings.saturation >= 200 && wrap_
                                  ? 0 : std::clamp(settings.saturation + 10 * dir, 0, 200);
        push(Action::SettingsChanged);
        break;
    case OPT_GAMMA:   // 0.50..1.50 in steps of 0.05
        settings.gamma = dir > 0 && settings.gamma >= 150 && wrap_
                             ? 50 : std::clamp(settings.gamma + 5 * dir, 50, 150);
        push(Action::SettingsChanged);
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
    case Screen::Pause: push(Action::Resume); break;
    case Screen::Main: selected_ = 3; dirty_ = true; break;   // onto Quit
    default: break;
    }
}

bool Frontend::key(int vk) {
    if (!active()) return false;
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
        }
    switch (vk) {
    case VK_UP: case 'W': select((selected_ + n - 1) % n); return true;
    case VK_DOWN: case 'S': select((selected_ + 1) % n); return true;
    case VK_LEFT: case 'A':
        if (tabs_row) set_tab(tab_ - 1);
        else if (option_row) activate(list[selected_], -1);
        return true;
    case VK_RIGHT: case 'D':
        if (tabs_row) set_tab(tab_ + 1);
        else if (option_row) activate(list[selected_], +1);
        return true;
    case VK_RETURN: case VK_SPACE:
        hovered_ = -1;
        if (selected_ < n && list[selected_].enabled) activate(list[selected_], +1);
        return true;
    case VK_ESCAPE: case VK_BACK: back(); return true;
    }
    return false;
}

void Frontend::mouse_move(int x, int y) {
    mouse_x_ = x;
    mouse_y_ = y;
    int hit = -1;
    for (size_t i = 0; i < hit_.size(); i++) {
        POINT p{x, y};
        if (PtInRect(&hit_[i], p)) hit = int(i);
    }
    if (hit >= 0) select(hit);
    hovered_ = hit;
}

void Frontend::click(int x, int y) {
    POINT p{x, y};
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
            if (x >= tab_rects_[t].left && x < tab_rects_[t].right) set_tab(t);
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
    mouse_move(x, y);
    if (screen_ == Screen::Records) {
        scroll_records(delta > 0 ? -1 : 1);
        return;
    }
    auto list = items();
    bool on_value = hovered_ >= 0 && hovered_ < int(values_.size()) && values_[hovered_].left >= 0 &&
                    x >= values_[hovered_].left;
    bool scrolls = screen_ == Screen::Options && int(list.size()) - 2 > VISIBLE_OPTIONS;
    if (hovered_ >= 0 && hovered_ < int(list.size()) && list[hovered_].id >= OPT_DISPLAY && (on_value || !scrolls)) {
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
    int k = std::clamp(record_scroll_ + by, 0, std::max(0, n - VISIBLE_RECORDS));
    if (k != record_scroll_) dirty_ = true;
    record_scroll_ = k;
}

// The rows of the open records tab between x0 and x1, from y0 down
void Frontend::draw_records(int x0, int x1, int y0, int row_h) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    auto rows = record_rows();
    record_scroll_ = std::clamp(record_scroll_, 0, std::max(0, int(rows.size()) - VISIBLE_RECORDS));
    const Style lit{PALE_TOP, PALE_BOTTOM, 0x080806, U(2), 0.95f};
    const Style dark{DIM, DIM, 0x080806, U(2), 0.9f};
    const Style value{0xF4D27A, 0xE0A848, 0x080806, U(2), 0.95f};
    int label_px = U(32), detail_px = U(19);
    for (int k = 0; k < VISIBLE_RECORDS && record_scroll_ + k < int(rows.size()); k++) {
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
    if (record_scroll_ + VISIBLE_RECORDS < int(rows.size())) {
        int yb = y0 + VISIBLE_RECORDS * row_h - U(20);
        triangle(x1 + U(36), yb, U(20), 2, GOLD_BOTTOM, 1);
        more_down_ = {x1 + U(10), yb - U(26), x1 + U(64), yb + U(26)};
    }
}

// ---- Drawing primitives (premultiplied BGRA canvas) ----

// Integer "over" on premultiplied BGRA (it runs for every pixel of panels and text)
static inline uint32_t over(uint32_t d, uint32_t rgb, int a) {   // a: 0..256
    int k = 256 - a;
    uint32_t rb = (((rgb & 0xFF00FFu) * a + (d & 0xFF00FFu) * k) >> 8) & 0xFF00FFu;
    uint32_t g = (((rgb & 0x00FF00u) * a + (d & 0x00FF00u) * k) >> 8) & 0x00FF00u;
    uint32_t da = ((255u * a + (d >> 24) * k) >> 8);
    return rb | g | (da << 24);
}

void Frontend::blend(int x, int y, uint32_t rgb, float a) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_ || a <= 0) return;
    uint32_t& d = px_[size_t(y) * w_ + x];
    d = over(d, rgb, int(std::min(a, 1.0f) * 256.0f + 0.5f));
}

void Frontend::fill(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha) {
    x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, w_), y1 = std::min(y1, h_);
    int a = int(std::clamp(alpha, 0.0f, 1.0f) * 256.0f + 0.5f);
    for (int y = y0; y < y1; y++) {
        uint32_t* row = px_ + size_t(y) * w_;
        for (int x = x0; x < x1; x++) row[x] = over(row[x], rgb, a);
    }
}

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

HFONT Frontend::font(Family f, int px) {
    auto key = std::make_pair(int(f), px);
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second;
    HFONT h = CreateFontW(-px, 0, 0, 0, f == MONO || f == JURASSIC ? FW_NORMAL : FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, FACES[f]);
    fonts_[key] = h;
    return h;
}

const Frontend::Mask& Frontend::mask(Family f, int px, const std::wstring& s, int edge_px) {
    std::wstring key = std::to_wstring(int(f)) + L"|" + std::to_wstring(px) + L"|" + std::to_wstring(edge_px) + L"|" + s;
    auto it = masks_.find(key);
    if (it != masks_.end()) return it->second;
    if (masks_.size() > 400) masks_.clear();   // window resized many times
    Mask& m = masks_[key];
    SelectObject(dc_, font(f, px));
    std::wstring t = s;   // the Jurassic font's space is very narrow: words ran together
    if (f == JURASSIC) {
        t.clear();
        for (wchar_t c : s) t += c == L' ' ? std::wstring(L"   ") : std::wstring(1, c);
    }
    SIZE sz{};
    GetTextExtentPoint32W(dc_, t.c_str(), int(t.size()), &sz);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc_, &tm);
    m.pad = edge_px + 2 + px / 6;   // room for the outline and overhanging letters
    m.text_w = sz.cx;
    m.text_h = sz.cy;
    m.w = sz.cx + 2 * m.pad;
    m.h = sz.cy + 2 * m.pad;
    m.grad0 = m.pad + tm.tmInternalLeading + (tm.tmAscent - tm.tmInternalLeading) / 8;
    m.grad1 = m.pad + tm.tmAscent;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = m.w;
    bi.bmiHeader.biHeight = -m.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return m;
    HGDIOBJ old = SelectObject(dc_, bmp);
    memset(bits, 0, size_t(m.w) * m.h * 4);
    SetBkMode(dc_, TRANSPARENT);
    SetTextColor(dc_, RGB(255, 255, 255));
    TextOutW(dc_, m.pad, m.pad, t.c_str(), int(t.size()));
    GdiFlush();
    m.a.resize(size_t(m.w) * m.h);
    const uint8_t* p = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < m.a.size(); i++) m.a[i] = p[i * 4 + 1];
    SelectObject(dc_, old);
    DeleteObject(bmp);
    if (edge_px > 0) {   // outline: the letters grown by a round brush
        m.e.assign(m.a.size(), 0);
        float r = float(edge_px);
        for (int dy = -edge_px - 1; dy <= edge_px + 1; dy++)
            for (int dx = -edge_px - 1; dx <= edge_px + 1; dx++) {
                float wgt = std::clamp(r + 1 - std::sqrt(float(dx * dx + dy * dy)), 0.0f, 1.0f);
                if (wgt <= 0) continue;
                for (int y = std::max(0, -dy); y < std::min(m.h, m.h - dy); y++) {
                    const uint8_t* src = &m.a[size_t(y + dy) * m.w];
                    uint8_t* dst = &m.e[size_t(y) * m.w];
                    for (int x = std::max(0, -dx); x < std::min(m.w, m.w - dx); x++) {
                        uint8_t v = uint8_t(float(src[x + dx]) * wgt);
                        if (v > dst[x]) dst[x] = v;
                    }
                }
            }
    }
    return m;
}

int Frontend::measure(Family f, int px, const std::wstring& s) {
    if (f == JURASSIC) px = int(px * 1.34f + 0.5f);   // as text() draws it
    return mask(f, px, s, 0).text_w;
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
// LOGO_SCREEN_H of the window high, starting LOGO_SCREEN_TOP from the top.
constexpr float POSTER_LOGO_TOP = 0.16f, POSTER_LOGO_END = 0.53f, POSTER_STONE_END = 0.88f;
constexpr float LOGO_SCREEN_TOP = 0.05f, LOGO_SCREEN_H = 0.42f;

void Frontend::poster_layout(float& scale, float& y0) const {
    scale = LOGO_SCREEN_H * h_ / ((POSTER_LOGO_END - POSTER_LOGO_TOP) * poster_h_);   // window px per poster px
    y0 = LOGO_SCREEN_TOP * h_ - POSTER_LOGO_TOP * poster_h_ * scale;
}

// The poster in the middle; beside and below it, its plain stone repeated in
// mirror image with the brightness of the poster's edges at that height,
// cross-faded into the poster and darkened towards the sides of the window
void Frontend::poster_background(bool stone_only, std::vector<uint32_t>& dst) {
    dst.resize(size_t(w_) * h_);
    float s, y0;
    poster_layout(s, y0);
    float pw = poster_w_ * s, x0 = (w_ - pw) * 0.5f;
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
            float px = (x - x0) / s;
            float inside = stone_only ? 0.0f : std::clamp(std::min(x - x0, x0 + pw - x) / seam, 0.0f, 1.0f) * rows;
            if (inside > 0)
                for (int k = 0; k < 4; k++) texel(px + (k & 1) * step, py + (k >> 1) * step, poster_rgb);
            if (inside < 1)
                for (int k = 0; k < 4; k++)
                    texel(pingpong(px + (k & 1) * step, float(poster_w_ - 1)),
                          stone0 + pingpong(py + (k >> 1) * step, stone_h), stone_rgb);
            float side = std::abs(x - w_ * 0.5f) / (w_ * 0.5f);
            float dark = 1 - 0.4f * side * side;
            uint32_t out = 0xFF000000u;
            for (int k = 0; k < 3; k++) {
                float v = (poster_rgb[k] * inside + stone_rgb[k] * gain * (1 - inside)) * 0.25f * dark;
                out |= uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f) << (16 - 8 * k);
            }
            dst[size_t(y) * w_ + x] = out;
        }
    }
    };
    int n = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int i = 0; i < n; i++) pool.emplace_back(rows_job, h_ * i / n, h_ * (i + 1) / n);
    for (auto& t : pool) t.join();
}

// Main menu backdrop: dark jungle green, a warm glow behind the title and a
// faint scanline pattern
void Frontend::background() {
    bg_.resize(size_t(w_) * h_);
    if (has_poster()) {   // both versions at once: switching screens must not stall
        poster_background(false, bg_);
        poster_background(true, bg_stone_);   // the long options list goes on plain stone
        return;
    }
    float cx = w_ * 0.5f, gy = 250 * u_;
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
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    bool over_game = screen_ == Screen::Pause || (screen_ == Screen::Options && options_from_pause_) ||
                     (screen_ == Screen::Boss && boss_over_game_);
    bool boss = screen_ == Screen::Boss || screen_ == Screen::Survival;   // a list of entries
    if (over_game) out.assign(size_t(w) * h, 0);
    else {
        if (bg_.empty()) background();
        out = has_poster() && screen_ == Screen::Options ? bg_stone_ : bg_;
    }
    px_ = out.data();
    hit_.clear();
    values_.clear();
    auto list = items();
    int cx = w / 2;
    bool records = screen_ == Screen::Records;
    bool options = screen_ == Screen::Options || records;   // records use the settings' card and tab row
    const Style gold{GOLD_TOP, GOLD_BOTTOM, EDGE, U(3)};
    const Style pale{PALE_TOP, PALE_BOTTOM, 0x080806, U(2), 0.95f};
    const Style dim{DIM, DIM, 0x080806, U(2), 0.9f};

    int y_items, spacing, item_px, hint_y, foot_y;
    if (!over_game && has_poster() && !options) {   // the poster is the title; the menu goes on the stone below it
        float ps, py0;
        poster_layout(ps, py0);
        int logo_end = int(py0 + POSTER_LOGO_END * poster_h_ * ps);
        const wchar_t* sub = screen_ == Screen::Play ? L"SELECT MODE" : screen_ == Screen::Survival ? L"SELECT ARENA" : boss ? L"SELECT BOSS" : options ? L"OPTIONS" : nullptr;
        if (sub) text(MONO, U(28), sub, cx, options ? U(390) : logo_end + U(20), 1, {0xF0C070, 0xE0A050});
        y_items = options ? U(432) : logo_end + U(80);
        spacing = options ? U(56) : screen_ == Screen::Survival ? U(52) : boss ? U(66) : U(84);
        item_px = options ? U(42) : screen_ == Screen::Survival ? U(42) : boss ? U(48) : U(60);
        hint_y = h - U(options ? 110.0f : 150.0f);
        foot_y = h - U(64);
    } else if (!over_game && options) {   // a card over the stone, no big title
        int top = U(70), bottom = h - U(70), half = U(580);
        fill(cx - half, top, cx + half, bottom, 0x06100A, 0.74f);
        band(cx, top, top + std::max(2, U(3)), half, AMBER, 0.9f);
        band(cx, bottom - std::max(2, U(3)), bottom, half, AMBER, 0.9f);
        text(display(), U(96), records ? L"RECORDS" : L"OPTIONS", cx, top + U(34), 1,
             {GOLD_TOP, GOLD_BOTTOM, EDGE, U(5), 1, U(5)});
        y_items = top + U(196);
        spacing = U(70);
        item_px = U(46);
        hint_y = bottom - U(116);
        foot_y = bottom - U(62);
    } else if (!over_game) {
        if (jurassic_) text(JURASSIC, U(54), L"JURASSIC PARK", cx, U(84), 1, {0xD8C49A, 0xB09A70, EDGE, U(2)});
        else text(MONO, U(30), L"J U R A S S I C    P A R K", cx, U(104), 1, {0xD8C49A, 0xB09A70});
        text(display(), jurassic_ ? U(176) : U(212), L"THE LOST WORLD", cx, jurassic_ ? U(122) : U(112), 1,
             {GOLD_TOP, GOLD_BOTTOM, EDGE, U(8), 1, U(9)});
        band(cx, U(388), U(388) + std::max(2, U(3)), U(330), AMBER, 0.85f);
        const wchar_t* sub = screen_ == Screen::Play ? L"SELECT MODE" : screen_ == Screen::Survival ? L"SELECT ARENA" : boss ? L"SELECT BOSS" : options ? L"OPTIONS" : nullptr;
        if (sub) text(MONO, U(28), sub, cx, U(408), 1, {0xF0C070, 0xE0A050});
        y_items = options ? U(452) : U(490);
        spacing = options ? U(56) : screen_ == Screen::Survival ? U(52) : boss ? U(66) : U(84);
        item_px = options ? U(42) : screen_ == Screen::Survival ? U(42) : boss ? U(48) : U(60);
        hint_y = h - U(options ? 110.0f : 150.0f);
        foot_y = h - U(64);
    } else {
        fill(0, 0, w, h, 0x000000, 0.62f);   // the frozen game shows through, darkened
        int n = options ? VISIBLE_OPTIONS + 2 : int(list.size());   // same panel for every settings tab
        spacing = options ? U(56) : U(80);
        item_px = options ? U(44) : U(58);
        int panel_h = U(170) + n * spacing + U(130);   // title, items, hint and key help
        int top = (h - panel_h) / 2;
        int half = options ? U(560) : U(460);
        fill(cx - half, top, cx + half, top + panel_h, 0x06100A, 0.92f);
        band(cx, top, top + std::max(2, U(3)), half, AMBER, 0.9f);
        band(cx, top + panel_h - std::max(2, U(3)), top + panel_h, half, AMBER, 0.9f);
        text(display(), U(110), options ? L"SETTINGS" : boss ? L"BOSSES" : L"PAUSED", cx, top + U(22), 1,
             {GOLD_TOP, GOLD_BOTTOM, EDGE, U(5), 1, U(5)});
        y_items = top + U(170);
        hint_y = top + panel_h - U(96);
        foot_y = top + panel_h - U(46);
    }
    // Settings tabs show at most VISIBLE_OPTIONS rows; longer ones scroll (the
    // tab row and BACK stay put), keeping the selected row in view
    int n_opts = options ? int(list.size()) - 2 : 0, shown_opts = n_opts;
    more_up_ = more_down_ = RECT{};
    if (options) {
        shown_opts = std::min(n_opts, VISIBLE_OPTIONS);
        scroll_ = std::clamp(scroll_, 0, std::max(0, n_opts - shown_opts));
        int k = selected_ - 1;   // selected option (0-based), if it is one
        if (k >= 0 && k < n_opts) {
            if (k < scroll_) scroll_ = k;
            if (k >= scroll_ + shown_opts) scroll_ = k - shown_opts + 1;
        }
    }
    if (records) {   // the list of records fills the rows of the options
        shown_opts = VISIBLE_OPTIONS;
        draw_records(cx - U(420), cx + U(400), y_items + spacing + U(24), VISIBLE_OPTIONS * spacing / VISIBLE_RECORDS);
    }
    auto slot_of = [&](int i) {   // screen row of list entry i, -1 when scrolled out of view
        if (!options || i == 0) return i;
        if (i == int(list.size()) - 1) return shown_opts + 1;   // BACK
        int k = i - 1 - scroll_;
        return k >= 0 && k < shown_opts ? k + 1 : -1;
    };

    for (int i = 0; i < int(list.size()); i++) {
        const Item& it = list[i];
        bool sel = i == selected_;
        int slot = slot_of(i);
        int y = y_items + slot * spacing + (options && slot > 0 ? U(24) : 0);   // room under the tabs
        int row_h = spacing;
        int half_w = options ? U(470) : U(320);
        if (slot < 0) {   // scrolled out of view: no hit area
            hit_.push_back(RECT{});
            values_.push_back({-1, 0});
            continue;
        }
        hit_.push_back({cx - half_w, y - (row_h - item_px) / 2 - U(6), cx + half_w, y + item_px + (row_h - item_px) / 2});
        values_.push_back({-1, 0});
        if (options && slot == 1 && scroll_ > 0) {   // more rows above / below: arrows at the right edge
            triangle(cx + half_w + U(16), y - U(4), U(20), -2, GOLD_BOTTOM, 1);
            more_up_ = {cx + half_w - U(10), y - U(30), cx + half_w + U(44), y + U(16)};
        }
        if (options && slot == shown_opts && scroll_ + shown_opts < n_opts) {
            triangle(cx + half_w + U(16), y + item_px + U(8), U(20), 2, GOLD_BOTTOM, 1);
            more_down_ = {cx + half_w - U(10), y + item_px - U(8), cx + half_w + U(44), y + item_px + U(34)};
        }
        if (sel) band(cx, y - U(8), y + item_px + U(14), half_w, 0xFF9A30, it.enabled ? 0.20f : 0.10f);
        const Style& st = !it.enabled ? dim : sel ? gold : pale;
        if (it.id == TABS) {   // tab names side by side, the open one underlined
            int gap = U(44), total = 0, tab_px = item_px * 17 / 20;
            for (int t = 0; t < tab_count(); t++) total += measure(display(), tab_px, tab_name(t)) + gap;
            if (total - gap > 2 * half_w - U(80)) {   // more tabs than fit: smaller names
                tab_px = tab_px * (2 * half_w - U(80)) / (total - gap);
                gap = gap * (2 * half_w - U(80)) / (total - gap);
                total = 0;
                for (int t = 0; t < tab_count(); t++) total += measure(display(), tab_px, tab_name(t)) + gap;
            }
            int x = cx - (total - gap) / 2;
            tab_rects_.clear();
            for (int t = 0; t < tab_count(); t++) {
                bool open_tab = t == tab_;
                const Style& ts = open_tab ? gold : Style{0x9A927E, 0x7E7766, 0x080806, U(2), 0.95f};
                int ty0 = y + (item_px - tab_px) / 2;
                int tw = text(display(), tab_px, tab_name(t), x, ty0, 0, ts);
                if (open_tab) fill(x, ty0 + tab_px + U(6), x + tw, ty0 + tab_px + U(6) + std::max(2, U(4)), GOLD_BOTTOM, 1);
                tab_rects_.push_back({x - gap / 2, y, x + tw + gap / 2, y + item_px});
                x += tw + gap;
            }
            if (sel) {
                int ty = y + item_px * 6 / 10;
                triangle(tab_rects_.front().left - U(10), ty, U(18), -1, GOLD_BOTTOM, 1);
                triangle(tab_rects_.back().right + U(10), ty, U(18), +1, GOLD_BOTTOM, 1);
            }
            continue;
        }
        if (!options || it.id == BACK) {
            int tw = text(display(), item_px, it.label, cx, y, 1, st);
            int ty = y + item_px * 6 / 10;
            if (sel && it.enabled) {
                triangle(cx - tw / 2 - U(34), ty, U(20), +1, GOLD_BOTTOM, 1);
                triangle(cx + tw / 2 + U(34), ty, U(20), -1, GOLD_BOTTOM, 1);
            }
            if (!it.value.empty())   // e.g. the stage of a boss, or "NOT READY"
                text(MONO, U(20), it.value, cx + tw / 2 + U(it.enabled ? 62.0f : 18.0f), y + item_px / 2 - U(6), 0,
                     {0xB89050, 0xA07838});   // past the arrow of a selected entry
            continue;
        }
        int left = cx - U(410), right = cx + U(410);   // symmetric about the centre
        text(display(), item_px, it.label, left, y, 0, st);
        int ty = y + item_px * 6 / 10;
        int vx0;
        if (it.id == OPT_VOLUME) {   // ten segments
            int seg_w = U(20), gap = U(6), seg_h = U(26);
            vx0 = right - 10 * seg_w - 9 * gap;
            for (int s = 0; s < 10; s++) {
                int x0 = vx0 + s * (seg_w + gap);
                bool lit = s < settings.volume;
                uint32_t c = lit ? mix(GOLD_TOP, GOLD_BOTTOM, float(s) / 9) : 0x3A3A30;
                fill(x0 - 1, ty - seg_h / 2 - 1, x0 + seg_w + 1, ty + seg_h / 2 + 1, 0x000000, 0.8f);
                fill(x0, ty - seg_h / 2, x0 + seg_w, ty + seg_h / 2, c, sel || !lit ? 1.0f : 0.8f);
            }
        } else {
            const Style& vs = sel ? gold : Style{0xF4D27A, 0xE0A848, 0x080806, U(2), 0.95f};
            vx0 = right - text(ITEM, item_px, it.value, right, y, 2, vs);
        }
        values_.back() = {vx0 - U(48), (vx0 + right) / 2};
        if (sel) {
            triangle(vx0 - U(24), ty, U(18), -1, GOLD_BOTTOM, 1);
            triangle(right + U(24), ty, U(18), +1, GOLD_BOTTOM, 1);
        }
    }

    if (selected_ < int(list.size()))
        text(MONO, U(26), list[selected_].hint, cx, hint_y, 1, {HINT, HINT});
    const wchar_t* foot = records   ? L"ARROWS  SCROLL / TAB      WHEEL  SCROLL      ESC  BACK"
                          : options ? L"ARROWS  SELECT / CHANGE      Q / E  TAB      ESC  BACK"
                                  : L"ARROWS  SELECT      ENTER / CLICK  CONFIRM      ESC  BACK";
    text(MONO, U(21), foot, cx, foot_y, 1, {FOOT, FOOT});
    dirty_ = false;
    return true;
}
