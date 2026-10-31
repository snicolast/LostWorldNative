// PC front end: main menu (Play, Options, Quit) shown at launch instead of
// the arcade attract loop, and the pause menu Esc opens during the game.
// Menus are a column of entries at the left (the logo or the screen's title
// above); the settings and the records have their categories down the left,
// the rows in the middle and a panel about the selected row at the right.
//
// Drawn on the CPU into a full-window BGRA picture (premultiplied alpha) with
// the OFL fonts in third_party/fonts and the game's yellow-to-orange letter
// colours; video/ui_overlay.h puts it on screen. Driven by the keyboard
// (arrows, Enter, Esc), by the mouse/gun (point and click, wheel) and by the
// gamepads (app.cpp turns what they send into keys).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../runtime/stats.h"
#include "ui_canvas.h"

struct FrontendSettings {
    bool fullscreen = false;
    int volume = 5;    // 0..10 (output gain)
    int speakers = 0;  // 0 stereo, 1 surround 4.0, 2 headphones (AudioStream::Speakers)
    int rear_boost = 0;  // dB on the rear pair: -6, -3, 0, 3, 6, 9, 12
    int weather_sound = 2;  // rain and thunder of our weather: 0 off, 1 low, 2 medium, 3 high
    bool cubic_samples = true;   // smooth (cubic) sample interpolation instead of the original linear one
    int aa = 0;        // 0 MSAA, 1 DLAA, 2 off (takes effect on the next launch)
    int scale = 100;   // 3D resolution in % of the window's (SCALES)
    bool half_rate = false;   // above 100 Hz the 3D is drawn every other refresh
    bool vsync = true;        // off: images go out without waiting for the monitor's refresh
    int aspect = 1;    // widest 3D view: 0 4:3 (original), 1 16:9, 2 21:9 (ultrawide, up to 2.4:1)
    bool haze = true;  // distance haze
    int bloom = 2;     // glow around bright things: 0 off, 1 low, 2 medium, 3 high
    bool ao = true;    // ambient occlusion
    bool sky = true;   // procedural sky instead of the original picture
    bool sun = true;   // a sun in the procedural sky
    bool shadows = true;   // sun shadows
    int ray_tracing = 2;   // traced through the world (GPUs with ray tracing): 0 off, 1 sun shadows, 2 also ambient occlusion
    bool shafts = true;    // light shafts from the sun
    bool soft = true;      // soft particles
    bool hdr = true;       // HDR lighting
    bool rim = true;       // rim light on dinosaurs and people
    bool relief = true;    // surface detail from the textures
    bool sharpen = true;   // contrast adaptive sharpening
    bool torch = true;     // modern flashlight at night (false: the original spotlight)
    bool lightning = true; // lightning in dark grey fog
    int rain = 2;          // rain where the game has its own: 0 off, 1 light, 2 heavy
    bool hd_text = true;
    bool crosshair = true;
    int crosshair_style = 1; // 0 the drawn rings, 1 the crosshair.png pictures when there are any (ui/crosshair_file.h)
    int crosshair_hide = 0; // seconds without use before a crosshair hides: 0 never, 3, 5 or 10
    bool free_play = false; // 1 / 2 (or a gun's start button) insert the credit themselves
    bool smooth = true;     // the 3D drawn between game frames at the monitor's refresh rate
    bool second_mouse = true;   // a second mouse is player 2's gun
    int subtitles = 0;      // voice-line subtitles: 0 off, 1 English, 2 Japanese
    int saturation = 80;    // colour saturation in % (100 = original; 80 by default)
    int gamma = 100;        // gamma x 100 (100 = original, higher = brighter)
    int pad_speed = 5;      // gamepad sight speed with the sticks, 1..10
    bool gyro = true;       // the gamepad's gyroscope moves the sight
    int gyro_speed = 5;     // 1..10
    int vibration = 2;      // gamepad vibration: 0 off, 1 low, 2 medium, 3 high
    int aim_assist = 2;     // gamepad aim assist: 0 off, 1 low, 2 medium, 3 high
    bool menu_music = true; // ui/menu_music.h, when there is a music file
    int run_weather = 0;    // RANDOM SPAWNS and survival: the WEATHER last picked before a run (frontend.cpp)
    std::string gun_p1, gun_p2; // the guns picked in GUN TEST: device paths ("" = the first and second to fire)

    // save/settings.ini; missing keys keep their current value
    void load(const char* path);
    void save(const char* path) const;

    // QUALITY: the graphics options that cost the most, set together. quality()
    // is the level they match, CUSTOM if none
    enum Quality { LOW, MEDIUM, HIGH, ULTRA, CUSTOM };
    static constexpr int SCALES[5] = {50, 67, 75, 85, 100};
    void set_quality(int q);
    int quality() const;
};

class Frontend : UiCanvas {
public:
    enum class Screen { None, Main, Play, Options, Pause, Boss, Records, Survival, Classic, GunTest, Weather };
    enum class Action { None, StartArcade, Resume, Quit, ToggleFullscreen, SettingsChanged,
                        StartBoss, PrepareBosses, LeaveBossMode, StartSurvival, QuitToMenu,
                        CrosshairChanged };   // a new crosshair picture (crosshair_pick)

    bool init(const std::string& font_dir);
    void shutdown();

    void open(Screen s);
    Screen screen() const { return screen_; }
    bool active() const { return screen_ != Screen::None; }
    // A menu over the paused game (the pause menu, its settings, the boss list from it)
    bool over_game() const {
        return screen_ == Screen::Pause || ((screen_ == Screen::Options || screen_ == Screen::GunTest) && options_from_pause_) ||
               (screen_ == Screen::Boss && boss_over_game_);
    }

    // Window input; true when the menu used it
    bool key(int vk);
    void mouse_move(int x, int y);
    void click(int x, int y);
    void wheel(int x, int y, int delta);
    // Actions picked since the last call, oldest first
    std::vector<Action> take_actions();

    FrontendSettings settings;
    int aa_at_launch = 0;   // anti-aliasing the renderer was started with
    bool rt_available = false;   // the GPU can trace rays (RAY TRACING option)
    std::wstring gamepads;  // the pads connected, for the CONTROLS tab ("" = none)
    bool gyro_pad = false;  // one of them has a gyroscope
    bool pad_prompts = false;   // a gamepad drives the menu: its buttons in the key help, not the keyboard's

    // Boss mode (runtime/bosses.h): the runtime says which fights are ready
    // and reads the one picked (StartBoss; also "retry" from the pause menu)
    enum BossStatus { BOSS_MISSING, BOSS_PREPARING, BOSS_READY, BOSS_FAILED };
    std::vector<int> boss_status;
    int boss_choice = 0;
    int survival_choice = 0;   // StartSurvival: index in SURVIVAL_ZONES (runtime/survival.h)
    bool random_spawns = false;   // StartArcade: CLASSIC > RANDOM SPAWNS (runtime/random_spawns.h)
    // RANDOM SPAWNS and survival: the weather picked on the WEATHER screen
    // before the run (runtime/weather.h: WEATHER_RANDOM, one of them or WEATHER_NONE)
    int run_weather() const;
    bool boss_mode = false;   // a boss fight is on: the pause menu offers retry and the list
    // The list of bosses, over the game (from the pause menu or after a fight) or on the menu backdrop
    void open_boss_list(bool over_game);

    // GUN TEST (Controls): every mouse and light gun Windows lists, kept up to
    // date by the window while the screen is open (runtime/raw_mice.h)
    struct GunView {
        std::string path;      // what settings.ini keeps
        std::wstring name;
        bool absolute = false; // reports positions over the screen: a light gun (or a tablet)
        bool active = false;   // reported in the last second
        bool has_pos = false;
        int x = 0, y = 0;      // client pixels
        bool trigger = false, reload = false;
        int player = -1;       // whose gun it is now (0, 1; -1 none)
    };
    std::vector<GunView> guns;   // those that have reported something since the start
    int quiet_mice = 0;          // and how many others Windows lists that never did
    bool gun_test_open() const { return screen_ == Screen::GunTest; }
    bool picking_guns() const { return screen_ == Screen::GunTest && gun_step_ > 0; }
    void gun_fired(int index);   // a left button press from guns[index]

    // RECORDS screen (runtime/stats.h): the window keeps this copy up to date while it is open
    StatsView records;

    // Redraws into `out` when something changed (or the size did); true if it did
    bool draw(int w, int h, std::vector<uint32_t>& out);
    // After draw: only rows y0..y1-1 changed (GUN TEST's marks moving)
    bool partial_rows(int& y0, int& y1) const {
        y0 = partial_y0_, y1 = partial_y1_;
        return partial_;
    }
    bool locked_notice_ = false;   // SURVIVAL: every arena locked, the notice saying how they unlock
    void draw_locked_notice();
    void invalidate() { dirty_ = true; }

private:
    enum Id {
        PLAY, OPTIONS, QUIT, ARCADE, BOSS, BACK, RESUME, SETTINGS, QUIT_TO_MENU, TABS,
        BOSS_ENTRY, PREPARE, RETRY, BOSS_LIST, LEAVE_BOSS, RECORDS, SURVIVAL, SURVIVAL_ENTRY, CLASSIC, RANDOM, WEATHER_ENTRY,
        OPT_DISPLAY, OPT_VOLUME, OPT_SPEAKERS, OPT_REAR, OPT_SAMPLES, OPT_AA, OPT_HAZE, OPT_BLOOM, OPT_AO, OPT_SKY, OPT_SUN, OPT_SHADOWS, OPT_SHAFTS, OPT_SOFT, OPT_HDR, OPT_RIM, OPT_RELIEF, OPT_SHARPEN, OPT_LIGHTNING, OPT_RAIN, OPT_TEXT, OPT_CROSS, OPT_MICE, OPT_SUBS,
        OPT_SAT, OPT_GAMMA, OPT_MOTION, OPT_PAD_SPEED, OPT_GYRO, OPT_GYRO_SPEED, OPT_VIBRATION, OPT_ASSIST, OPT_TORCH, OPT_RT, OPT_CROSS_HIDE, OPT_FREE_PLAY,
        OPT_QUALITY, OPT_SCALE, OPT_HALF_RATE, OPT_VSYNC,
        OPT_CROSS_STYLE, OPT_CROSS_P1, OPT_CROSS_P2, OPT_MUSIC, OPT_ASPECT, OPT_GUN_TEST, OPT_WEATHER_SOUND,
        NO_ROW,   // a setting only kept in the file
    };
    // How an option row shows its value: a switch, a choice with a dot per
    // value, a slider, or a button that opens something
    enum Kind { PLAIN, TOGGLE, CHOICE, SLIDER, BUTTON };
    struct Item {
        Id id;
        std::wstring label, value, hint;
        bool enabled = true;
        int index = -1;   // BOSS_ENTRY: which boss
        Kind kind = PLAIN;
        int pos = 0, count = 0;   // CHOICE: which value of how many; TOGGLE: pos 1 = on
        bool levels = false;      // CHOICE: low to high (the dots up to it lit)
        float frac = 0;           // SLIDER: 0..1
    };
    using Mask = GdiText::Mask;
    struct Style {
        uint32_t top, bottom;   // gradient (0xRRGGBB)
        uint32_t edge = 0;      // outline colour
        int edge_px = 0;
        float alpha = 1;
        int shadow = 0;         // drop shadow offset (pixels), black
    };
    enum Family { TITLE, ITEM, MONO, JURASSIC, BODY };   // in the order of GdiText::Face
    // Titles and menu entries: the Jurassic Park font when it is installed
    Family display() const { return jurassic_ ? JURASSIC : TITLE; }

    std::vector<Item> items() const;
    // Every setting: where save/settings.ini keeps it and its row in the
    // options (frontend.cpp)
    struct Setting;
    friend struct FrontendSettings;
    static const std::vector<Setting>& setting_table();
    static const Setting* setting_of(Id id);
    std::vector<Item> option_items(const FrontendSettings& s) const;   // every option, as with these settings
    const std::wstring& default_value(int id);   // what option_items shows for it with the defaults
    void activate(const Item& it, int dir);
    void reset_tab();   // the open settings tab back to its defaults
    void back();
    void push(Action a) { actions_.push_back(a); }
    void select(int index);

    const Mask& mask(Family f, int px, const std::wstring& text, int edge_px) {
        return gdi_->mask(GdiText::Face(f), px, text, edge_px);
    }
    int text(Family f, int px, const std::wstring& s, int x, int y, int align, const Style& st);
    int measure(Family f, int px, const std::wstring& s);
    int extent(Family f, int px, const std::wstring& s);   // width without making a mask (body text)
    std::vector<std::wstring> wrap(Family f, int px, const std::wstring& s, int max_w);
    void band(int cx, int y0, int y1, int half_w, uint32_t rgb, float alpha);
    void triangle(int x, int y, int size, int dir, uint32_t rgb, float alpha);
    void hgrad(int x0, int y0, int x1, int y1, uint32_t rgb, float a0, float a1);   // alpha a0 at the left to a1
    void vgrad(int x0, int y0, int x1, int y1, uint32_t rgb, float a0, float a1);   // alpha a0 at the top to a1
    void frame(int x0, int y0, int x1, int y1, int t, uint32_t rgb, float alpha);   // outline, t pixels
    void pill(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha);           // round ends (a disc when square)
    void background();
    // The screens: the column of entries, and the settings / records page
    void draw_menu(bool over_game, const std::vector<Item>& list);
    void draw_options(bool over_game, const std::vector<Item>& list);
    void draw_value(const Item& it, bool sel, int vx0, int vx1, int y, int row_h);
    void draw_gun_test(bool over_game);
    int gun_step_ = 0;   // GUN TEST: 0 watching, 1 waiting for player 1's gun to fire, 2 for player 2's
    int gun_player(const GunView& g, bool& picked) const;   // whose gun it is, and whether GUN TEST picked it
    // The marks move every refresh: the page without them is kept, and only
    // where they were and are is drawn again
    std::string gun_key() const;   // all the page shows but the marks' places
    void draw_gun_marks();
    std::vector<uint32_t> gun_page_;
    std::string gun_page_key_;
    std::vector<RECT> gun_marks_;   // where the marks are drawn now
    bool partial_ = false;
    int partial_y0_ = 0, partial_y1_ = 0;
    // Key help along the bottom: button (or key) and what it does; returns its rectangle
    RECT prompt(int& x, int y, const wchar_t* pad_button, const wchar_t* key, const wchar_t* what);
    int prompt_width(const wchar_t* pad_button, const wchar_t* key, const wchar_t* what);   // how far it moves x
    RECT back_rect_{}, reset_rect_{};   // the BACK and RESET prompts, clickable
    RECT coffee_rect_{};   // the donation link on the main menu, clickable
    bool coffee_hot_ = false;   // the pointer is over it
    bool coffee_focus_ = false;   // chosen with the keys or a pad (Up from the first entry)
    void open_coffee();

    Screen screen_ = Screen::None;
    bool options_from_pause_ = false;
    bool weather_for_survival_ = false;   // WEATHER: before a survival run (else RANDOM SPAWNS)
    bool boss_over_game_ = false;
    int selected_ = 0, hovered_ = -1;
    bool dirty_ = true;
    std::vector<Action> actions_;
    std::vector<RECT> hit_;   // item rectangles of the last draw
    struct ValueArea { int left, middle; };   // option rows: value column (left < 0: none)
    std::vector<ValueArea> values_;
    bool wrap_ = false;
    std::vector<Item> defaults_;   // option_items(FrontendSettings{}), made once (again if rt_available changes)
    bool defaults_rt_ = false;
    // Settings tabs (setting_table says where each option goes)
    enum { DISPLAY_TAB, LIGHTING_TAB, ATMOSPHERE_TAB, AUDIO_TAB, CONTROLS_TAB, GAME_TAB };
    static constexpr int TAB_COUNT = 6;
    static const wchar_t* const TAB_NAMES[TAB_COUNT];
    void set_tab(int t);
    int tab_ = 0;   // open tab of the settings, or of the records
    // The tab row of the screen on: settings tabs, or ACHIEVEMENTS / STATS
    static constexpr int RECORD_TAB_COUNT = 2;
    static const wchar_t* const RECORD_TAB_NAMES[RECORD_TAB_COUNT];
    int tab_count() const { return screen_ == Screen::Records ? RECORD_TAB_COUNT : TAB_COUNT; }
    const wchar_t* tab_name(int t) const { return screen_ == Screen::Records ? RECORD_TAB_NAMES[t] : TAB_NAMES[t]; }
    // Records: the rows of the open tab and the first one shown
    struct RecordRow {
        std::wstring label, detail, value;
        bool lit = true;   // an achievement: unlocked
    };
    std::vector<RecordRow> record_rows() const;
    int visible_records_ = 7;   // as many as fit the window
    int record_scroll_ = 0;
    void scroll_records(int by);
    void draw_records(int x0, int x1, int y0, int row_h);
    // A settings tab shows as many options as fit the window; the rest scroll
    int visible_opts_ = 12;
    int scroll_ = 0;           // first option shown
    RECT more_up_{}, more_down_{};   // clickable "more" arrows of a scrolled tab
    std::vector<RECT> tab_rects_;   // a step past the end starts again (clicks on the label)
    int mouse_x_ = -1, mouse_y_ = -1;

    GdiText* gdi_ = nullptr;
    bool jurassic_ = false;   // third_party/fonts/Jurassic_Park.ttf loaded

    float u_ = 1;   // pixels per unit of a 1920x1080 layout
    std::vector<uint32_t> bg_;   // main menu backdrop for the current size
    std::vector<uint32_t> bg_stone_;   // with a poster: its stone only, without the logo (options)
    // Optional poster (third_party/menu/cover.png, not in the repository):
    // portrait picture with the logo at the top and plain dark stone below.
    // It becomes the backdrop and the title; its stone continues at the sides.
    std::vector<uint32_t> poster_;
    int poster_w_ = 0, poster_h_ = 0;
    bool load_poster(const std::wstring& path);
    // Optional pictures of what an option does (res/preview/<name>.jpg),
    // loaded the first time their option is selected; w = 0: none
    struct Picture { std::vector<uint32_t> px; int w = 0, h = 0; };
    std::map<int, Picture> previews_;
    const Picture& preview(Id id);
    void picture(const Picture& p, int x0, int y0, int x1, int y1);   // scaled to fill the box, cropped
    void sprite(const Picture& p, int x0, int y0, int w, int h);      // scaled to w x h, its own transparency kept
    // The gamepad's buttons in the key help (res/pad/<button>.png, CC0); w = 0:
    // none, the drawn disc or pill instead
    std::map<std::wstring, Picture> pad_icons_;
    const Picture& pad_icon(const std::wstring& button);
    // The players' crosshairs as the game draws them (their pictures, or the
    // rings), over a stretch of the game: the panel of the crosshair rows
    Picture crosshair_[2];
    bool crosshair_loaded_ = false;
    void crosshair_preview(int x0, int y0, int x1, int y1);
    // Picture behind the menus (third_party/menu/background.png or .jpg, else res/menu/background.png):
    // the menus go to the right, the poster's logo above them; blurred behind
    // the options
    Picture backdrop_;
    Picture sega_;   // optional third_party/menu/sega.png: the credit at the bottom left of the menus
    Picture logo_;   // third_party/menu/logo.png, else res/menu/logo.png: the title over the backdrop, its own transparency kept
    bool has_logo() const { return logo_.w > 0 && has_backdrop(); }
    bool has_backdrop() const { return backdrop_.w > 0; }
    void backdrop_background();
    bool has_poster() const { return poster_w_ > 0; }
    void poster_background(std::vector<uint32_t>& dst, std::vector<uint32_t>& stone);
    void poster_layout(float& scale, float& x0, float& y0) const;   // the logo at the top left
};
