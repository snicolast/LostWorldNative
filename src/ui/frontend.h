// PC front end: main menu (Play, Options, Quit) shown at launch instead of
// the arcade attract loop, and the pause menu Esc opens during the game.
// Menus are a column of entries at the right (the logo above). The settings
// and the records are a page as Sega Rally 2's: the sections in a column at the
// left, the open one's rows in groups, a panel at the right with what the
// chosen row is and does, the keys along the bottom.
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

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../runtime/stats.h"
#include "frontend_settings.h"
#include "news.h"
#include "splash_video.h"
#include "ui_canvas.h"

class Frontend : UiCanvas {
public:
    enum class Screen { None, Splash, Main, Play, Options, Pause, Boss, Records, Survival, Classic, GunTest, Weather };
    enum class Action { None, StartArcade, Resume, Quit, SettingsChanged,
                        DisplayChanged,     // MODE or RESOLUTION: applied at once (app.cpp)
                        StartBoss, PrepareBosses, LeaveBossMode, StartSurvival, QuitToMenu,
                        CrosshairChanged,   // a new crosshair picture (crosshair_pick)
                        Restart };          // RESTART NOW: saved, closed and started again (app.cpp)

    bool init(const std::string& font_dir);
    void shutdown();

    void open(Screen s);
    // At launch: the logo video with its sound (third_party/menu/ or res/menu/
    // splash_snicolast.mp4; if Windows cannot play it, splash_snicolast.png
    // faded in and out), then the main menu. Enter / Space / Esc / a click (a
    // pad's START or A) hurries it out. Straight to the menu without one
    void open_splash();
    Screen after_splash = Screen::Main;   // None: the game (--no-menu)
    bool splash() const { return screen_ == Screen::Splash; }
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

    FrontendSettings settings;   // the experience running (EXPERIENCE: settings.experience, the one for the next start)
    // The other experience's settings (PC EDITION's while ARCADE ORIGINAL runs, and the
    // other way round): kept as they are, saved with these (app.cpp)
    FrontendSettings other;
    bool arcade = false;    // ARCADE ORIGINAL is running: no main menu, a pause of its own
    int aa_at_launch = 0;   // anti-aliasing the renderer was started with
    bool rt_available = false;   // the GPU can trace rays (RAY TRACING option)
    std::wstring gamepads;  // the pads connected, for the CONTROLS tab ("" = none)
    bool gyro_pad = false;  // one of them has a gyroscope
    bool pad_prompts = false;   // a gamepad drives the menu: its buttons in the key help, not the keyboard's
    // RESOLUTION: the monitor's modes (VkPresenter::display_modes, smallest
    // first), the window's inside and the desktop's size (app.cpp keeps them up to date)
    std::vector<std::pair<int, int>> resolutions;
    int client_w = 0, client_h = 0, desktop_w = 0, desktop_h = 0;
    int refresh_hz = 0;   // the window's monitor (MAX FPS: MONITOR)

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

    // At launch: news.txt from GitHub in the background (when the settings
    // allow it). An entry newer than the news.txt here and than the last one
    // closed with LATER opens a window over the main menu listing every entry
    // this version lacks (see news.h)
    void check_for_news();

    // RECORDS screen (runtime/stats.h): the window keeps this copy up to date while it is open
    StatsView records;

    // Redraws into `out` when something changed (or the size did); true if it did
    bool draw(int w, int h, std::vector<uint32_t>& out);
    // After draw: only rows y0..y1-1 changed (GUN TEST's marks moving)
    bool partial_rows(int& y0, int& y1) const {
        y0 = partial_y0_, y1 = partial_y1_;
        return partial_;
    }
    void invalidate() { dirty_ = true; }

private:
    bool locked_notice_ = false;   // SURVIVAL: every arena locked, the notice saying how they unlock
    void draw_locked_notice();
    enum Id {
        PLAY, OPTIONS, QUIT, ARCADE, BOSS, BACK, RESUME, SETTINGS, QUIT_TO_MENU, TABS,
        BOSS_ENTRY, PREPARE, RETRY, BOSS_LIST, LEAVE_BOSS, RECORDS, SURVIVAL, SURVIVAL_ENTRY, CLASSIC, RANDOM, WEATHER_ENTRY,
        REC_ROW,   // a row of RECORDS
        // the settings' rows: OPT_DISPLAY first (key() and wheel(): an option row is one from it on)
        OPT_DISPLAY, OPT_VOLUME, OPT_SPEAKERS, OPT_REAR, OPT_SAMPLES, OPT_AA, OPT_HAZE, OPT_BLOOM, OPT_AO, OPT_SKY, OPT_SUN, OPT_SHADOWS, OPT_SHAFTS, OPT_SOFT, OPT_HDR, OPT_RIM, OPT_SHARPEN, OPT_LIGHTNING, OPT_RAIN, OPT_TEXT, OPT_CROSS, OPT_MICE, OPT_SUBS,
        OPT_SAT, OPT_GAMMA, OPT_MOTION, OPT_PAD_SPEED, OPT_GYRO, OPT_GYRO_SPEED, OPT_VIBRATION, OPT_ASSIST, OPT_TORCH, OPT_RT, OPT_CROSS_HIDE, OPT_FREE_PLAY,
        OPT_SCALE, OPT_MAX_FPS, OPT_BGFPS, OPT_VSYNC,
        OPT_CROSS_STYLE, OPT_CROSS_P1, OPT_CROSS_P2, OPT_MUSIC, OPT_ASPECT, OPT_GUN_TEST, OPT_WEATHER_SOUND,
        OPT_MSAA, OPT_ANISO, OPT_FILTER_2D, OPT_UPDATES, OPT_RESOLUTION, OPT_TRIGGERS, OPT_EXPERIENCE,
        PAUSE_CREDIT, PAUSE_RESTORE, PC_EDITION,   // ARCADE ORIGINAL's pause
        NO_ROW,   // a setting only kept in the file
    };
    // How an option row shows its value: a switch, a choice with a dot per
    // value, a slider, or a button that opens something
    enum Kind { PLAIN, TOGGLE, CHOICE, SLIDER, BUTTON };
    struct Item {
        Id id;
        std::wstring label, value, hint;
        bool enabled = true;
        int index = -1;   // BOSS_ENTRY: which boss; REC_ROW: its group (RECORD_GROUPS)
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
    // In the order of GdiText::Face. ROW, ROW_MEDIUM and PROSE: the settings
    // page's rows, group headings and descriptions (Barlow, as Sega Rally 2's)
    enum Family { TITLE, ITEM, MONO, JURASSIC, BODY, ROW, ROW_MEDIUM, PROSE };
    // Titles and menu entries: the Jurassic Park font when it is installed
    Family display() const { return jurassic_ ? JURASSIC : TITLE; }
    // The same for a name on the settings page: Saira for one with digits (the Jurassic font has none)
    Family page_font(const std::wstring& s) const;

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
    FrontendSettings base_defaults() const;   // those defaults: PC EDITION's, or ARCADE ORIGINAL's look
    void back();
    void push(Action a) { actions_.push_back(a); }
    void select(int index);

    const Mask& mask(Family f, int px, const std::wstring& text, int edge_px) {
        return gdi_->mask(GdiText::Face(f), px, text, edge_px);
    }
    int text(Family f, int px, const std::wstring& s, int x, int y, int align, const Style& st);
    int measure(Family f, int px, const std::wstring& s);
    void ink_rows(Family f, int px, const std::wstring& s, int& top, int& bottom);   // from the y given to text()
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
    // OPTIONS, SETTINGS (over the paused game) and RECORDS, as Sega Rally 2's page
    void draw_options_page(bool over_game, const std::vector<Item>& list);
    // The page's ribbons behind the open section and the chosen row: amber from
    // a0 at the left to a1 at the right, leaning, a stripe before them
    void ribbon(int x0, int y0, int x1, int y1, int slant, float a0, float a1, int gap, int sw, float alpha);
    // They glide from one row (or section) to the next
    struct Glide {
        float from = 0, to = 0, from_h = 0, to_h = 0;
        double t0 = -1;
        long key = -1;
    };
    Glide glide_tab_, glide_row_;
    bool gliding_ = false;   // one of them is moving (this picture)
    void glide(Glide& g, long key, int y, int h, int& gy, int& gh);
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
    // UPDATE AVAILABLE: the window over the main menu with the entries of
    // news.txt this version lacks. UPDATE NOW starts update.bat (it waits for
    // the game to close, updates, rebuilds and starts it again) and closes the
    // game; LATER (Esc) keeps it closed until a newer entry
    struct NewsDownload {                  // shared with the thread that downloads it (left to finish on its own)
        std::mutex mutex;
        std::string text;                  // news.txt from GitHub
        std::atomic<int> state{0};         // 0 downloading, 1 there, 2 failed, 3 taken (take_news)
    };
    std::shared_ptr<NewsDownload> news_download_;
    std::vector<NewsEntry> news_;          // the entries shown
    int news_latest_ = 0;                  // the newest one on GitHub
    bool news_open_ = false;
    int news_button_ = 0;                  // 0 UPDATE NOW, 1 LATER
    int news_hot_ = -1;                    // the button under the pointer
    RECT news_buttons_[2]{};
    void take_news();                      // the download, once it is there
    void draw_news();
    bool news_key(int vk);
    void news_choose(int button);
    void start_update();
    // RESTART NEEDED: leaving the settings with a change that only applies
    // on a new start (ANTI-ALIASING to or from DLAA / DLSS). LATER goes on as BACK
    bool restart_open_ = false;
    int restart_button_ = 0;   // 0 RESTART NOW, 1 LATER
    int restart_hot_ = -1;     // the button under the pointer
    RECT restart_buttons_[2]{};
    bool aa_waits() const;               // ANTI-ALIASING changed, waiting for a restart
    std::wstring restart_list() const;   // what waits for a restart, "" nothing
    // BACK TO PC EDITION (ARCADE ORIGINAL's pause): the same window, with this instead
    // of the list; LATER goes back to the pause
    std::wstring restart_note_;
    // RESTORE ORIGINAL (ARCADE ORIGINAL's pause): the same window asks RESTORE / CANCEL
    // first; RESTORE puts arcade_base() back, then RESTART NEEDED if something needs it
    bool restore_ask_ = false;
    bool restart_shown() const { return restart_open_ && (screen_ == Screen::Options || screen_ == Screen::Pause); }
    bool restart_key(int vk);
    void restart_choose(int button);
    void draw_restart();
    // RESOLUTION's list: a card over the settings with the monitor's modes,
    // largest first; Enter (or a click) applies one at once
    bool drop_open_ = false;
    int drop_sel_ = 0, drop_top_ = 0;   // the one lit and the first shown, counted from the top (largest)
    std::vector<RECT> drop_rects_;
    int resolution_index() const;   // the mode in use (nearest), into resolutions
    void open_drop();
    void drop_pick(int i);          // i: into resolutions
    bool drop_key(int vk);
    void draw_drop();


    Screen screen_ = Screen::None;
    bool options_from_pause_ = false;
    bool weather_for_survival_ = false;   // WEATHER: before a survival run (else RANDOM SPAWNS)
    bool boss_over_game_ = false;
    int selected_ = 0, hovered_ = -1;
    bool dirty_ = true;
    // Menu animation: the description opening under the chosen entry (0..1
    // for each entry) and the highlight band gliding to it; while either
    // moves the menu is drawn again every refresh
    std::vector<float> desc_open_;
    float band_y_ = -1, band_h_ = -1;
    double anim_last_ = -1;
    Screen anim_screen_ = Screen::None;
    bool animating_ = false;
    std::vector<Action> actions_;
    std::vector<RECT> hit_;   // item rectangles of the last draw
    struct ValueArea { int left, middle; };   // option rows: value column (left < 0: none)
    std::vector<ValueArea> values_;
    bool wrap_ = false;
    std::vector<Item> defaults_;   // option_items(FrontendSettings{}), made once (again if rt_available changes)
    bool defaults_rt_ = false;
    // The settings' sections (setting_table says where each option goes,
    // ROW_GROUPS in frontend.cpp the headings over its rows)
    enum { EXPERIENCE_TAB, DISPLAY_TAB, LIGHTING_TAB, AUDIO_TAB, CONTROLS_TAB };   // LIGHTING_TAB is shown as GRAPHICS
    static constexpr int TAB_COUNT = 5;
    static const wchar_t* const TAB_NAMES[TAB_COUNT];
    static const wchar_t* group_of(Id id);   // the heading over that option's row, if it starts a group
    void set_tab(int t);
    int tab_ = 0;   // open section of the settings, or of the records
    // The sections of the page on: the settings', or ACHIEVEMENTS / STATS
    static constexpr int RECORD_TAB_COUNT = 2;
    static const wchar_t* const RECORD_TAB_NAMES[RECORD_TAB_COUNT];
    int tab_count() const { return screen_ == Screen::Records ? RECORD_TAB_COUNT : TAB_COUNT; }
    // Records: the rows of the open section (REC_ROW entries of items())
    struct RecordRow {
        std::wstring label, detail, value;
        bool lit = true;   // an achievement: unlocked
        int group = 0;     // RECORD_GROUPS: UNLOCKED, LOCKED, TOTALS, BEST SCORES, BEST STAGES, BOSSES, SURVIVAL
    };
    std::vector<RecordRow> record_rows() const;
    int scroll_ = 0;                // first row shown
    bool options_scroll_ = false;   // the open section's rows do not all fit (the last draw)
    RECT more_up_{}, more_down_{};  // clickable "more" arrows of a scrolled section
    std::vector<RECT> tab_rects_;   // the sections in the column at the left (clicks)
    int mouse_x_ = -1, mouse_y_ = -1;

    GdiText* gdi_ = nullptr;
    bool jurassic_ = false;   // third_party/fonts/Jurassic_Park.ttf loaded

    float u_ = 1;   // pixels per unit of a 1920x1080 layout
    std::vector<uint32_t> bg_;   // main menu backdrop for the current size
    std::vector<uint32_t> bg_stone_;   // behind the settings page: the backdrop blurred and darker (a poster: its stone)
    // Optional poster (third_party/menu/cover.png, not in the repository):
    // portrait picture with the logo at the top and plain dark stone below.
    // It becomes the backdrop and the title; its stone continues at the sides.
    std::vector<uint32_t> poster_;
    int poster_w_ = 0, poster_h_ = 0;
    bool load_poster(const std::wstring& path);
    struct Picture { std::vector<uint32_t> px; int w = 0, h = 0; };
    void picture(const Picture& p, int x0, int y0, int x1, int y1);   // scaled to fill the box, cropped
    void sprite(const Picture& p, int x0, int y0, int w, int h);      // scaled to w x h, its own transparency kept
    // The gamepad's buttons in the key help (res/pad/<button>.png, CC0); w = 0:
    // none, the drawn disc or pill instead
    std::map<std::wstring, Picture> pad_icons_;
    const Picture& pad_icon(const std::wstring& button);
    // The achievements' badges on the records page (res/achievements): lit, locked
    Picture badges_[2];
    bool badges_loaded_[2] = {};
    const Picture& achievement_badge(bool lit);
    // Picture behind the menus (third_party/menu/background.png or .jpg, else res/menu/background.png):
    // the menus go to the right, the poster's logo above them; blurred behind
    // the options
    Picture backdrop_;
    Picture credit_;   // third_party/menu/snicolast.png, else res/menu/snicolast.png: the remaster's credit on the main menu
    Picture sega_;   // optional third_party/menu/sega.png: the credit at the bottom left of the menus
    // The splash screens: their pictures, which one is on and since when,
    // and the one on scaled to the window (white around it)
    std::vector<Picture> splash_;
    int splash_on_ = 0;
    double splash_from_ = 0;
    double skip_from_ = -1;     // hurried out at this time (-1: not)
    float skip_level_ = 1;      // from this brightness
    bool skip_next_ = false;    // pressed again while hurrying: the next one goes too
    std::vector<uint32_t> splash_px_;
    int splash_px_on_ = -1, splash_px_w_ = 0, splash_px_h_ = 0;
    void skip_splash();
    bool draw_splash(std::vector<uint32_t>& out);   // false: the last one ended
    // The splash video, when there is one Windows can open: it replaces the
    // pictures. Its picture now (fitted to the window), when it ended (-1:
    // not yet) and how bright it was last drawn
    SplashVideo video_;
    bool video_on_ = false;
    std::vector<uint32_t> video_px_;
    int video_pw_ = 0, video_ph_ = 0;
    double video_end_ = -1;
    float video_level_ = 0;
    bool draw_splash_video(std::vector<uint32_t>& out);
    Picture logo_;   // third_party/menu/logo.png, else res/menu/logo.png: the title over the backdrop, its own transparency kept
    void shade_backdrop();            // darker at the right and the bottom, the logo: on px_
    // Lightning, the side menus' entrance (frontend.cpp): a strike lights the
    // backdrop and the entries come on with it
    enum Strike { STRIKE_INTRO, STRIKE_SWAP, STRIKE_IDLE };
    std::vector<uint32_t> bg_lit_;   // bg_ as a strike lights it, for the current size
    double strike_from_ = -1;        // the strike going on (menu_clock), -1 none
    Strike strike_kind_ = STRIKE_INTRO;
    float strike_amp_ = 1;           // an idle one: how bright
    double next_idle_strike_ = -1;   // when the next idle one comes (menu_clock), -1 not yet chosen
    float reveal_ = 1, glare_ = 0;   // this picture: the entries' opacity, how white their letters
    long menu_frames_ = 0;           // pictures drawn: the menu's clock with LW_MENU_FPS
    double menu_clock() const;       // seconds (LW_MENU_FPS: pictures drawn / that rate, for films)
    static bool side_menu(Screen s);  // the entries in a column on the backdrop (Main, Play and those under it)
    void start_strike(Strike kind);
    void end_strike();               // a key or a click: at once as it ends
    bool strike_light(float& shade, float& flash);   // this picture's light, reveal_ and glare_; false: none
    void relight(std::vector<uint32_t>& out, float shade, float flash) const;
    int index_of(Id id) const;        // where an entry is in the list on screen (0 if not there)
    bool has_logo() const { return logo_.w > 0 && has_backdrop(); }
    bool has_backdrop() const { return backdrop_.w > 0; }
    void backdrop_background();
    bool has_poster() const { return poster_w_ > 0; }
    void poster_background(std::vector<uint32_t>& dst, std::vector<uint32_t>& stone);
    void poster_layout(float& scale, float& x0, float& y0) const;   // the logo at the top left
};
