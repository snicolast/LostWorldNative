#include "frontend.h"
#include "frontend_rows.h"
#include "frontend_style.h"

#include "../runtime/bosses.h"
#include "../runtime/survival.h"
#include "../runtime/weather.h"
#include "crosshair_file.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <ctime>
#include <iterator>
#include <thread>

#include "../common/clock.h"
#include "../common/game_hz.h"
#include "../video/image_io.h"

// ---- The settings: the table is in frontend_table.cpp, the file in frontend_settings.cpp ----

using namespace setting_text;

namespace {
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

// ---- Setup ----

bool Frontend::init(const std::string& font_dir) {
    gdi_ = &GdiText::acquire(font_dir);
    bool ok = gdi_->ready();
    for (GdiText::Face f : {GdiText::SAIRA, GdiText::BARLOW, GdiText::MONO, GdiText::BARLOW_SEMIBOLD, GdiText::BARLOW_MEDIUM,
                            GdiText::BARLOW_REGULAR})   // the others are optional
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
    // the splash video, else the splash screens in order: your own in third_party/menu/ first, else the ones in res/menu/
    for (const wchar_t* p : {L"third_party/menu/splash_snicolast.mp4", L"res/menu/splash_snicolast.mp4"})
        if (video_.open(p)) {
            printf("[menu] splash video %ls\n", p);
            break;
        }
    for (const wchar_t* name : {L"splash_snicolast.png"})
        for (const wchar_t* dir : {L"third_party/menu/", L"res/menu/"}) {
            Picture p;
            if (!load_image(std::wstring(dir) + name, p.px, p.w, p.h)) continue;
            printf("[menu] splash %ls%ls (%dx%d)\n", dir, name, p.w, p.h);
            splash_.push_back(std::move(p));
            break;
        }
    // logo and background: your own in third_party/menu/ first, else the ones in res/menu/
    for (const wchar_t* p : {L"third_party/menu/logo.png", L"res/menu/logo.png"})
        if (load_image(p, logo_.px, logo_.w, logo_.h)) {
            printf("[menu] logo %ls (%dx%d)\n", p, logo_.w, logo_.h);
            break;
        }
    for (const wchar_t* p : {L"third_party/menu/snicolast.png", L"res/menu/snicolast.png"})
        if (load_image(p, credit_.px, credit_.w, credit_.h)) {
            printf("[menu] credit %ls (%dx%d)\n", p, credit_.w, credit_.h);
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
bool nvidia_aa(int aa) { return aa == 1 || (aa >= 3 && aa <= 6); }   // DLAA or DLSS: chosen when the renderer starts
}  // namespace

// Any picture format Windows can read (WIC), as BGRA (nothing when it fails)
bool fe::load_image(const std::wstring& path, std::vector<uint32_t>& px, int& w, int& h) {
    if (image_load_bgra(path, w, h, px)) return true;
    px.clear();
    w = h = 0;
    return false;
}

bool Frontend::load_poster(const std::wstring& path) { return load_image(path, poster_, poster_w_, poster_h_); }

void Frontend::shutdown() {
    video_.close();
    if (gdi_) gdi_->release();
    gdi_ = nullptr;
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

const wchar_t* const Frontend::TAB_NAMES[Frontend::TAB_COUNT] = {L"EXPERIENCE", L"DISPLAY", L"GRAPHICS", L"AUDIO", L"CONTROLS", L"GAME"};


// The heading over the first row of each group of a section (setting_table
// has the rows in the order shown)
const wchar_t* Frontend::group_of(Id id) {
    static const struct { Id id; const wchar_t* name; } GROUPS[] = {
        {OPT_DISPLAY, L"SCREEN"}, {OPT_VSYNC, L"FRAME RATE"}, {OPT_AA, L"SMOOTHING"}, {OPT_SAT, L"PICTURE"},
        {OPT_SUN, L"LIGHTING"}, {OPT_RT, L"RAY TRACING"}, {OPT_SKY, L"SKY AND WEATHER"},
        {OPT_VOLUME, L"VOLUME"}, {OPT_SPEAKERS, L"SOUND"},
        {OPT_PAD_SPEED, L"GAMEPAD"}, {OPT_CROSS, L"SIGHT"}, {OPT_MICE, L"GUNS"},
        {OPT_EXPERIENCE, L"EDITION"}, {OPT_FREE_PLAY, L"ARCADE"}};
    for (const auto& g : GROUPS)
        if (g.id == id) return g.name;
    return nullptr;
}

const wchar_t* const Frontend::RECORD_TAB_NAMES[Frontend::RECORD_TAB_COUNT] = {L"ACHIEVEMENTS", L"STATS"};

void Frontend::set_tab(int t) {
    tab_ = (t + tab_count()) % tab_count();
    scroll_ = 0;
    // the settings page and the records: their first row (the column of sections is not a row)
    selected_ = screen_ == Screen::Options || screen_ == Screen::Records ? 1 : std::min(selected_, int(items().size()) - 1);
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
    auto size_text = [](int w, int h) { return std::to_wstring(w) + L" x " + std::to_wstring(h); };
    const int mode = std::clamp(s.display_mode, 0, 2);
    for (Item& it : all) switch (it.id) {
        case OPT_MAX_FPS:
            it.value = s.max_fps < 0    ? std::wstring(L"UNLIMITED")
                       : s.max_fps == 0 ? L"MONITOR" + (refresh_hz > 0 ? L" (" + std::to_wstring(refresh_hz) + L")" : std::wstring())
                                        : std::to_wstring(s.max_fps);
            it.enabled = s.smooth;
            it.hint = !s.smooth ? L"With MOTION ORIGINAL the game's own 57.5 frames are shown: nothing to set"
                      : s.vsync ? L"Images a second at most; with VSYNC in whole refreshes (60 on a 120 Hz monitor: every other one, "
                                  L"the crosshair still at each).  Lower: less work for the GPU"
                                : L"Images a second at most (UNLIMITED: as many as the GPU can).  Any number can go in settings.ini, "
                                  L"e.g. max_fps = 117 for G-SYNC";
            break;
        case OPT_RESOLUTION:   // the size chosen (none yet: the window's inside, or the desktop's)
            it.value = mode == 1 ? size_text(desktop_w, desktop_h)
                       : s.res_w > 0 ? size_text(s.res_w, s.res_h)
                       : mode == 0 ? size_text(client_w, client_h) : size_text(desktop_w, desktop_h);
            it.enabled = mode != 1 && !resolutions.empty();
            if (mode == 1) it.hint = L"BORDERLESS always uses the desktop's resolution";
            else if (resolutions.empty()) it.hint = L"The monitor's resolutions could not be read";
            else if (mode == 2) it.hint = L"The mode the monitor switches to.  Enter: the list";
            else if (s.res_w > 0 && (std::abs(client_w - s.res_w) > 2 || std::abs(client_h - s.res_h) > 2))
                it.hint = L"The size of the window's picture: shown at " + size_text(client_w, client_h) +
                          L", the same shape, to fit on the desktop.  Enter: the list";
            else it.hint = L"The size of the window's picture.  Enter: the list";
            break;
        case OPT_AA:   // off, FXAA and MSAA change at once; DLAA and DLSS start (stop, change) with the game
            if (nvidia_aa(s.aa) || nvidia_aa(aa_at_launch))
                it.hint += s.aa != aa_at_launch ? L".  Applies after a restart (offered when you leave the settings)"
                                                : L".  Applies after a restart";
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
        case OPT_EXPERIENCE:
            if (s.experience != (arcade ? 1 : 0)) it.hint += L".  Applies after a restart (offered when you leave the settings)";
            break;
        case OPT_PAD_SPEED: it.hint += gamepads.empty() ? L"No gamepad connected" : L"Connected: " + gamepads; break;
        case OPT_GYRO:
            if (!gyro_pad) it.hint = L"Point the gamepad to aim (DualSense, DualShock 4, Switch Pro).  None connected";
            break;
        default: break;
        }
    return all;
}

FrontendSettings Frontend::base_defaults() const {
    FrontendSettings d;
    if (arcade) d.arcade_base();
    return d;
}

void Frontend::reset_tab() {
    const FrontendSettings d = base_defaults();
    for (const Item& it : items())   // the window stays as it is, and the experience
        if (const Setting* row = setting_of(it.id); row && it.id != OPT_DISPLAY && it.id != OPT_EXPERIENCE) {
            row->set(settings, row->get(d));
            if (it.id == OPT_MAX_FPS) settings.max_fps = d.max_fps;   // (kept apart from the table)
        }
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
        if (arcade)   // ARCADE ORIGINAL: no main menu to go back to; its own entries after QUIT GAME
            return {{RESUME, L"RESUME", L"", L"Back to the game"},
                    {SETTINGS, L"SETTINGS", L"", L"Display, sound, controls and the experience"},
                    {QUIT, L"QUIT GAME", L"", L"Close the game"},
                    {PAUSE_CREDIT, L"CREDIT", settings.free_play ? L"FREE PLAY" : L"COIN",
                     settings.free_play ? L"Start and continue with START alone.  Enter: a coin per game (5 inserts one)"
                                        : L"Insert a coin with 5 (6 for player 2), then START.  Enter: FREE PLAY"},
                    {PAUSE_RESTORE, L"RESTORE ORIGINAL", L"", L"The arcade's original look and sound (not the screen or the controls)"},
                    {PC_EDITION, L"BACK TO PC EDITION", L"", L"The main menu and the PC settings come back on the next start"}};
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
        std::vector<Item> list = {{TABS, L"", L"", pad_prompts ? L"LB / RB change the section" : L"Q / E or Tab change the section, or click one"}};
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
                               L" achievements unlocked";
        std::vector<Item> list = {{TABS, L"", L"", summary}};
        for (const RecordRow& r : record_rows()) list.push_back({REC_ROW, r.label, r.value, r.detail, r.lit, r.group});
        list.push_back({BACK, L"BACK", L"", L"Return to the main menu"});
        return list;
    }
    default:
        return {};
    }
}

void Frontend::open_splash() {
    if (video_.is_open() && settings.splash) {
        open(Screen::Splash);
        video_on_ = true;
        splash_from_ = now_seconds();
        skip_from_ = -1;
        video_end_ = -1;
        video_level_ = 0;
        video_.set_volume(float(settings.volume) / 10.0f);   // half at the default 5 (the menu music now sits a little higher)
        video_.play();
        return;
    }
    video_.close();
    if (splash_.empty() || !settings.splash) {
        open(after_splash);
        return;
    }
    open(Screen::Splash);
    splash_on_ = 0;
    splash_from_ = now_seconds();
    skip_from_ = -1;
    skip_next_ = false;
}

void Frontend::open(Screen s) {
    const Screen was = screen_;
    const bool was_dark = was == Screen::None || was == Screen::Splash || over_game();   // the game or the logos before
    if (screen_ == Screen::Splash && s != Screen::Splash) {   // not needed again
        splash_px_ = {}, splash_px_on_ = -1;
        video_.close();
        video_on_ = false;
        video_px_ = {};
    }
    locked_notice_ = false;
    drop_open_ = false;
    restart_open_ = restore_ask_ = false;   // (not left open behind another screen)
    restart_note_.clear();
    screen_ = s;
    selected_ = 0;
    scroll_ = 0;
    hovered_ = -1;
    hit_.clear();
    dirty_ = true;
    // The lightning: the main menu coming up after the logos or a game, or
    // one side menu after another (not back from OPTIONS or RECORDS)
    strike_from_ = next_idle_strike_ = -1;
    if (side_menu(s) && !over_game()) {
        if (was_dark) start_strike(STRIKE_INTRO);
        else if (side_menu(was) && was != s) start_strike(STRIKE_SWAP);
    }
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
    case RECORDS: tab_ = 0; open(Screen::Records); selected_ = 1; break;
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
    case SURVIVAL_ENTRY:   // the weather first (not indoors: straight in)
        survival_choice = it.index;
        weather_for_survival_ = true;
        if (SURVIVAL_ZONES[it.index].indoor) {
            push(Action::StartSurvival);
            break;
        }
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
    case PAUSE_CREDIT:   // (GAME > FREE PLAY too)
        settings.free_play = !settings.free_play;
        push(Action::SettingsChanged);
        break;
    case PAUSE_RESTORE:   // RESTORE / CANCEL first (restart_choose)
        restore_ask_ = restart_open_ = true;
        restart_button_ = 0;
        restart_hot_ = -1;
        break;
    case PC_EDITION:   // saved at once; RESTART NOW, or LATER (back to the pause)
        settings.experience = 0;
        push(Action::SettingsChanged);
        restart_note_ = L"PC EDITION: the main menu comes back on the next start";
        restart_open_ = true;
        restart_button_ = 0;
        restart_hot_ = -1;
        break;
    case SETTINGS: options_from_pause_ = true; tab_ = 0; open(Screen::Options); selected_ = 1; break;
    case OPT_DISPLAY:   // applied at once (app.cpp)
        settings.display_mode = (std::clamp(settings.display_mode, 0, 2) + dir + 3) % 3;
        push(Action::DisplayChanged);
        break;
    case OPT_MAX_FPS: {   // MONITOR first, UNLIMITED last
        if (!settings.smooth) break;
        static const int STEPS[] = {0, 30, 60, 90, 120, 144, 165, 240, -1};
        int n = int(std::size(STEPS)), i = -1;
        for (int k = 0; k < n; k++)
            if (STEPS[k] == settings.max_fps) i = k;
        if (i < 0)   // a number set in settings.ini: from the step above it
            for (i = 1; i < n - 1 && STEPS[i] < settings.max_fps; i++) {}
        settings.max_fps = STEPS[(i + dir + n) % n];
        push(Action::SettingsChanged);
        break;
    }
    case OPT_RESOLUTION: {   // the monitor's next mode from the chosen one
        if (settings.display_mode == 1 || resolutions.empty()) break;
        int n = int(resolutions.size()), i = (resolution_index() + dir + n) % n;
        settings.res_w = resolutions[size_t(i)].first;   // applied at once
        settings.res_h = resolutions[size_t(i)].second;
        push(Action::DisplayChanged);
        break;
    }
    case OPT_CROSS_P1:
    case OPT_CROSS_P2:   // the Windows file dialog; a picture picked is also shown from then on
        if (crosshair_pick(it.id == OPT_CROSS_P2 ? 1 : 0)) {
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
        if (!restart_open_ && !restart_list().empty()) {   // RESTART NEEDED first (LATER: back as here)
            restart_open_ = dirty_ = true;
            restart_button_ = 0;
            restart_hot_ = -1;
            return;
        }
        restart_open_ = false;
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
    case Screen::Records: open(Screen::Main); selected_ = index_of(RECORDS); break;
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
    case Screen::Main: selected_ = index_of(QUIT); dirty_ = true; break;
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
    if (screen_ == Screen::Splash) {   // these hurry it on, the rest do nothing
        if (vk == VK_RETURN || vk == VK_SPACE || vk == VK_ESCAPE || vk == VK_BACK) skip_splash();
        return true;
    }
    end_strike();
    if (restart_shown()) return restart_key(vk);
    if (drop_open_ && screen_ == Screen::Options) return drop_key(vk);
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
    if (screen_ == Screen::Records) switch (vk) {   // sections and rows as on the settings page; nothing to change
        case 'Q': case VK_PRIOR: case VK_LEFT: case 'A': set_tab(tab_ - 1); return true;
        case 'E': case VK_NEXT: case VK_RIGHT: case 'D': set_tab(tab_ + 1); return true;
        case VK_TAB: set_tab(tab_ + ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)); return true;
        case VK_UP: case 'W': if (n > 1) select(selected_ <= 1 ? n - 1 : selected_ - 1); return true;
        case VK_DOWN: case 'S': if (n > 1) select(selected_ >= n - 1 ? 1 : selected_ + 1); return true;
        case VK_RETURN: case VK_SPACE: if (selected_ == n - 1) back(); return true;
        case VK_ESCAPE: case VK_BACK: back(); return true;
        default: return false;
        }
    if (screen_ == Screen::Options) switch (vk) {   // section keys
        case 'Q': case VK_PRIOR: set_tab(tab_ - 1); return true;
        case 'E': case VK_NEXT: set_tab(tab_ + 1); return true;
        case VK_TAB: set_tab(tab_ + ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)); return true;
        case 'R': reset_tab(); return true;
        case VK_UP: case 'W':   // the rows and BACK: the column of sections is not one of them
            if (n > 1) select(selected_ <= 1 ? n - 1 : selected_ - 1);
            return true;
        case VK_DOWN: case 'S':
            if (n > 1) select(selected_ >= n - 1 ? 1 : selected_ + 1);
            return true;
        }
    switch (vk) {
    case VK_UP: case 'W': select((selected_ + n - 1) % n); return true;
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
        if (option_row && list[selected_].enabled && open_drop(list[selected_].id)) return true;   // its list
        if (selected_ < n && list[selected_].enabled) activate(list[selected_], +1);
        return true;
    case VK_ESCAPE: case VK_BACK: back(); return true;
    }
    return false;
}

int Frontend::index_of(Id id) const {
    auto list = items();
    for (int i = 0; i < int(list.size()); i++)
        if (list[i].id == id) return i;
    return 0;
}

// ---- RESTART NEEDED ----

bool Frontend::aa_waits() const {
    return settings.aa != aa_at_launch && (nvidia_aa(settings.aa) || nvidia_aa(aa_at_launch));
}

std::wstring Frontend::restart_list() const {
    std::wstring list;
    if (settings.experience != (arcade ? 1 : 0))
        list = std::wstring(L"EXPERIENCE ") + (settings.experience ? L"ARCADE ORIGINAL" : L"PC EDITION");
    if (aa_waits()) {
        const Setting& d = *setting_of(OPT_AA);
        list += (list.empty() ? L"" : L" and ") + (L"ANTI-ALIASING " + std::wstring(d.shown[d.pos(settings)]));
    }
    return list;
}

bool Frontend::restart_key(int vk) {
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_TAB:
    case 'A': case 'D': case 'W': case 'S':
        restart_button_ ^= 1;
        dirty_ = true;
        return true;
    case VK_RETURN: case VK_SPACE: restart_choose(restart_button_); return true;
    case VK_ESCAPE: case VK_BACK: restart_choose(1); return true;   // LATER
    }
    return true;   // nothing else while it is open
}

void Frontend::restart_choose(int button) {
    dirty_ = true;
    if (restore_ask_) {   // RESTORE ORIGINAL: RESTORE, or CANCEL (back to the pause)
        restore_ask_ = restart_open_ = false;
        if (button == 1) return;
        settings.arcade_base();   // (not the screen, the speakers, the controls)
        push(Action::SettingsChanged);
        if (!restart_list().empty()) {   // (ANTI-ALIASING from DLAA or DLSS): RESTART NEEDED next
            restart_open_ = true;
            restart_button_ = 0;
            restart_hot_ = -1;
        }
        return;
    }
    if (button == 0) {   // RESTART NOW: app.cpp saves, closes and starts the game again
        restart_open_ = false;
        push(Action::Restart);
        return;
    }
    if (screen_ == Screen::Pause) {   // BACK TO PC EDITION or a restore, LATER: the pause again (saved already)
        restart_open_ = false;
        restart_note_.clear();
        return;
    }
    back();   // LATER: on, as BACK would (not asked again)
}

// ---- RESOLUTION's list ----

int Frontend::resolution_index() const {
    int n = int(resolutions.size());
    int cw = settings.res_w > 0 ? settings.res_w : settings.display_mode == 0 ? client_w : desktop_w;
    int ch = settings.res_w > 0 ? settings.res_h : settings.display_mode == 0 ? client_h : desktop_h;
    int best = 0;
    for (int k = 1; k < n; k++)
        if (std::abs(resolutions[size_t(k)].first - cw) + std::abs(resolutions[size_t(k)].second - ch) <
            std::abs(resolutions[size_t(best)].first - cw) + std::abs(resolutions[size_t(best)].second - ch))
            best = k;
    return best;
}

void Frontend::first_value(Id id) {
    switch (id) {
    case OPT_DISPLAY: settings.display_mode = 0; break;
    case OPT_MAX_FPS: settings.max_fps = 0; break;   // MONITOR first, UNLIMITED last
    default:
        if (const Setting* d = setting_of(id)) {
            if (d->kind == TOGGLE) d->set(settings, 1);   // ON, OFF
            else if (!d->steps.empty()) d->set(settings, d->steps[0]);
            else d->set(settings, d->kind == SLIDER ? d->lo : 0);
        }
        break;
    }
}

bool Frontend::open_drop(Id id) {
    auto value_of = [&](const FrontendSettings& s) {   // the row's value as its row shows it
        for (const Item& it : option_items(s))
            if (it.id == id) return it.value;
        return std::wstring();
    };
    drop_.clear();
    drop_in_use_ = -1;
    const Setting* d = setting_of(id);
    if (id == OPT_RESOLUTION) {
        if (settings.display_mode == 1 || resolutions.empty()) return false;
        int n = int(resolutions.size());
        for (int k = 0; k < n; k++) {   // largest first
            const auto& r = resolutions[size_t(n - 1 - k)];
            DropEntry e{std::to_wstring(r.first) + L" x " + std::to_wstring(r.second), settings};
            e.s.res_w = r.first;
            e.s.res_h = r.second;
            drop_.push_back(e);
        }
        drop_in_use_ = n - 1 - resolution_index();
        drop_action_ = Action::DisplayChanged;
    } else if (id == OPT_DISPLAY || id == OPT_MAX_FPS || (d && d->kind != BUTTON && d->kind != PLAIN)) {
        // stepped up from its first value until a value comes again
        const FrontendSettings keep = settings;
        const std::wstring now = value_of(settings);
        std::vector<Action> pushed;
        pushed.swap(actions_);
        first_value(id);
        for (int k = 0; k < 64; k++) {
            std::wstring v = value_of(settings);
            if (v.empty() || std::any_of(drop_.begin(), drop_.end(), [&](const DropEntry& e) { return e.label == v; })) break;
            if (v == now) drop_in_use_ = int(drop_.size());
            drop_.push_back({v, settings});
            activate(Item{id}, +1);
        }
        drop_action_ = actions_.empty() ? Action::SettingsChanged : actions_.back();
        actions_.swap(pushed);
        settings = keep;
    }
    if (drop_.size() < 2) {
        drop_.clear();
        return false;
    }
    drop_id_ = id;
    drop_sel_ = std::max(drop_in_use_, 0);
    drop_top_ = 0;
    drop_open_ = dirty_ = true;
    return true;
}

void Frontend::drop_pick(int i) {   // applied at once
    drop_open_ = false;
    dirty_ = true;
    if (i >= 0 && i < int(drop_.size()) && i != drop_in_use_) {
        settings = drop_[size_t(i)].s;
        push(drop_action_);
    }
    drop_.clear();
}

bool Frontend::drop_key(int vk) {
    int n = int(drop_.size());
    if (n == 0) {
        drop_open_ = false;
        return true;
    }
    switch (vk) {
    case VK_UP: case 'W': drop_sel_ = (drop_sel_ + n - 1) % n; break;
    case VK_DOWN: case 'S': drop_sel_ = (drop_sel_ + 1) % n; break;
    case VK_PRIOR: case 'Q': drop_sel_ = std::max(0, drop_sel_ - 8); break;
    case VK_NEXT: case 'E': drop_sel_ = std::min(n - 1, drop_sel_ + 8); break;
    case VK_RETURN: case VK_SPACE: drop_pick(drop_sel_); return true;
    case VK_ESCAPE: case VK_BACK: drop_open_ = false; drop_.clear(); break;
    default: return true;   // nothing else while it is open
    }
    dirty_ = true;
    return true;
}

void Frontend::mouse_move(int x, int y) {
    if (screen_ == Screen::Splash) return;
    mouse_y_ = y;
    if (drop_open_ && screen_ == Screen::Options) {   // the resolution under the pointer lit
        for (int k = 0; k < int(drop_rects_.size()); k++)
            if (PtInRect(&drop_rects_[k], POINT{x, y}) && drop_top_ + k != drop_sel_) {
                drop_sel_ = drop_top_ + k;
                dirty_ = true;
            }
        return;
    }
    if (restart_shown()) {   // only its two buttons
        int hot = -1;
        for (int b = 0; b < 2; b++)
            if (PtInRect(&restart_buttons_[b], POINT{x, y})) hot = b;
        if (hot != restart_hot_ || (hot >= 0 && hot != restart_button_)) dirty_ = true;
        restart_hot_ = hot;
        if (hot >= 0) restart_button_ = hot;
        return;
    }
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
    if (screen_ == Screen::Splash) {
        skip_splash();
        return;
    }
    end_strike();
    if (restart_shown()) {
        for (int b = 0; b < 2; b++)
            if (PtInRect(&restart_buttons_[b], p)) restart_choose(b);
        return;
    }
    if (drop_open_ && screen_ == Screen::Options) {   // a value, or away from the list: closed
        for (int k = 0; k < int(drop_rects_.size()); k++)
            if (PtInRect(&drop_rects_[k], p)) {
                drop_pick(drop_top_ + k);
                return;
            }
        drop_open_ = false;
        drop_.clear();
        dirty_ = true;
        return;
    }
    if (locked_notice_) {
        locked_notice_ = false;
        dirty_ = true;
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
    if (PtInRect(&more_up_, p) || PtInRect(&more_down_, p)) {   // the scroll arrows of a long section
        int n = int(items().size());
        int k = std::clamp(selected_ - 1, 0, n - 3) + (PtInRect(&more_up_, p) ? -1 : 1);
        select(1 + std::clamp(k, 0, n - 3));
        return;
    }
    mouse_move(x, y);
    auto list = items();
    if (hovered_ < 0 || hovered_ >= int(list.size()) || !list[hovered_].enabled) return;
    if (list[hovered_].id == TABS) {   // the section under the pointer
        for (int t = 0; t < int(tab_rects_.size()); t++)
            if (PtInRect(&tab_rects_[t], p)) set_tab(t);
        return;
    }
    if (list[hovered_].id == REC_ROW) return;   // a record: nothing to change
    // Option rows: the arrows step down and up; anywhere else the list of values
    // opens (a row without one, a button, does what it does)
    int dir = 0;
    if (hovered_ < int(values_.size()) && values_[hovered_].left >= 0) {
        const ValueArea& v = values_[size_t(hovered_)];
        if (x >= v.left && x < v.value) dir = -1;
        else if (x >= v.right) dir = +1;
    }
    if (dir == 0 && list[hovered_].id >= OPT_DISPLAY && open_drop(list[hovered_].id)) return;
    wrap_ = dir == 0;
    activate(list[hovered_], dir == 0 ? +1 : dir);
    wrap_ = false;
}

void Frontend::wheel(int x, int y, int delta) {
    if (screen_ == Screen::GunTest || screen_ == Screen::Splash || restart_shown()) return;
    if (drop_open_ && screen_ == Screen::Options) {
        drop_key(delta > 0 ? VK_UP : VK_DOWN);
        return;
    }
    mouse_move(x, y);
    auto list = items();
    bool on_value = hovered_ >= 0 && hovered_ < int(values_.size()) && values_[hovered_].left >= 0 &&
                    x >= values_[hovered_].left;
    bool scrolls = (screen_ == Screen::Options || screen_ == Screen::Records) && options_scroll_;
    if (hovered_ >= 0 && hovered_ < int(list.size()) && list[hovered_].id >= OPT_DISPLAY && list[hovered_].kind != BUTTON &&
        (on_value || !scrolls)) {
        int keep = hovered_;   // over a value: change it
        hovered_ = -1;   // no wrap-around from the wheel
        activate(list[keep], delta > 0 ? +1 : -1);
        hovered_ = keep;
    } else if (scrolls) {   // a long section, away from the values: scroll the list
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

std::wstring fe::grouped(int64_t v) {   // 12,345
    std::wstring d = std::to_wstring(v < 0 ? -v : v), r;
    for (size_t i = 0; i < d.size(); i++) {
        if (i && (d.size() - i) % 3 == 0) r += L',';
        r += d[i];
    }
    return v < 0 ? L"-" + r : r;
}

namespace {

std::wstring clock_time(int64_t frames) {   // 2:31.4
    double s = double(frames) / GAME_HZ;
    wchar_t b[32];
    swprintf(b, 32, L"%d:%04.1f", int(s / 60), s - 60 * int(s / 60));
    return b;
}

std::wstring hours(int64_t frames) {   // 3 H 05 MIN
    int64_t m = int64_t(double(frames) / GAME_HZ / 60);
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
            RecordRow r{a.name, a.text, L"", when(i) != 0, when(i) != 0 ? 0 : 1};
            if (r.lit) {
                time_t t = time_t(when(i));
                wchar_t b[32];
                wcsftime(b, 32, L"%d %b %Y", localtime(&t));
                r.value = b;
            } else if (a.goal > 1) {
                int64_t p = achievement_progress(s, i);
                r.value = std::string(a.key) == "veteran"
                              ? std::to_wstring(int(double(p) / GAME_HZ / 3600)) + L" / 5 H"
                              : grouped(p) + L" / " + grouped(a.goal);
            } else {
                r.value = L"LOCKED";
            }
            rows.push_back(r);
        }
        return rows;
    }
    int group = 2;   // TOTALS, BEST SCORES, BEST STAGES, BOSSES, SURVIVAL (RECORD_GROUPS)
    auto row = [&](const std::wstring& label, const std::wstring& value, const std::wstring& detail = L"") {
        rows.push_back({label, detail, value, true, group});
    };
    row(L"GAMES PLAYED", grouped(s.games), grouped(s.games_finished) + L" finished, " + grouped(s.continues) +
                                               L" continues");
    row(L"TIME PLAYED", hours(s.play_frames));
    row(L"HIT RATE", percent(s.hits, s.shots), grouped(s.hits) + L" hits out of " + grouped(s.shots) + L" shots");
    row(L"TECHNICAL SHOTS", grouped(s.technical));
    row(L"DINOSAURS DOWN", grouped(s.kills));
    row(L"LIVES LOST", grouped(s.lives_lost));
    group = 3;
    static const wchar_t* const COURSES[STAT_COURSES] = {L"BEGINNER", L"EXPERT", L"SHUFFLE"};
    for (int i = 0; i < STAT_COURSES; i++)
        row(L"BEST SCORE " + std::wstring(COURSES[i]), s.best_score[i] ? grouped(s.best_score[i]) : L"-",
            s.finished_course[i] ? L"Finished " + grouped(s.finished_course[i]) + L" times" : L"");
    group = 4;
    for (int i = 0; i < STAT_STAGES; i++) {
        const Stats::Stage& st = s.stage[i];
        std::wstring detail = st.clears ? L"Best score " + grouped(st.best_score) + L",  best hit rate " +
                                              percent(st.best_accuracy, 1000)
                                        : L"Not cleared yet";
        row(L"STAGE " + std::to_wstring(i + 1), st.best_frames ? clock_time(st.best_frames) : L"-",
            grouped(st.clears) + L" clears.  " + detail);
    }
    group = 5;
    for (int i = 0; i < STAT_BOSSES && i < BOSS_COUNT; i++) {
        const Stats::Boss& b = s.boss[i];
        row(BOSSES[i].name, b.best_frames ? clock_time(b.best_frames) : L"-",
            grouped(b.wins) + L" wins in " + grouped(b.attempts) + L" fights" +
                (b.flawless ? L",  " + grouped(b.flawless) + L" flawless" : L""));
    }
    group = 6;
    row(L"SURVIVAL", s.best_wave ? L"WAVE " + std::to_wstring(s.best_wave) : L"-", L"Furthest wave reached");
    return rows;
}
