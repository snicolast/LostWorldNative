// PC front end: main menu (Play, Options, Quit) shown at launch instead of
// the arcade attract loop, and the pause menu Esc opens during the game.
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

struct FrontendSettings {
    bool fullscreen = false;
    int volume = 5;    // 0..10 (output gain)
    int speakers = 0;  // 0 stereo, 1 surround 4.0, 2 headphones (AudioStream::Speakers)
    int rear_boost = 0;  // dB on the rear pair: 0, 3, 6, 9, 12
    bool cubic_samples = true;   // smooth (cubic) sample interpolation instead of the original linear one
    int aa = 0;        // 0 MSAA, 1 DLAA, 2 off (takes effect on the next launch)
    int aspect = 1;    // widest 3D view: 0 4:3 (original), 1 16:9, 2 21:9 (ultrawide)
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

    // save/settings.ini; missing keys keep their current value
    void load(const char* path);
    void save(const char* path) const;
};

class Frontend {
public:
    enum class Screen { None, Main, Play, Options, Pause, Boss, Records, Survival };
    enum class Action { None, StartArcade, Resume, Quit, ToggleFullscreen, SettingsChanged,
                        StartBoss, PrepareBosses, LeaveBossMode, StartSurvival, QuitToMenu };

    bool init(const std::string& font_dir);
    void shutdown();

    void open(Screen s);
    Screen screen() const { return screen_; }
    bool active() const { return screen_ != Screen::None; }

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

    // Boss mode (runtime/bosses.h): the runtime says which fights are ready
    // and reads the one picked (StartBoss; also "retry" from the pause menu)
    enum BossStatus { BOSS_MISSING, BOSS_PREPARING, BOSS_READY, BOSS_FAILED };
    std::vector<int> boss_status;
    int boss_choice = 0;
    int survival_choice = 0;   // StartSurvival: index in SURVIVAL_ZONES (runtime/survival.h)
    bool boss_mode = false;   // a boss fight is on: the pause menu offers retry and the list
    // The list of bosses, over the game (from the pause menu or after a fight) or on the menu backdrop
    void open_boss_list(bool over_game);

    // RECORDS screen (runtime/stats.h): the window keeps this copy up to date while it is open
    StatsView records;

    // Redraws into `out` when something changed (or the size did); true if it did
    bool draw(int w, int h, std::vector<uint32_t>& out);
    void invalidate() { dirty_ = true; }

private:
    enum Id {
        PLAY, OPTIONS, QUIT, ARCADE, BOSS, BACK, RESUME, SETTINGS, QUIT_TO_MENU, TABS,
        BOSS_ENTRY, PREPARE, RETRY, BOSS_LIST, LEAVE_BOSS, RECORDS, SURVIVAL, SURVIVAL_ENTRY,
        OPT_DISPLAY, OPT_VOLUME, OPT_SPEAKERS, OPT_REAR, OPT_SAMPLES, OPT_AA, OPT_HAZE, OPT_BLOOM, OPT_AO, OPT_SKY, OPT_SUN, OPT_SHADOWS, OPT_SHAFTS, OPT_SOFT, OPT_HDR, OPT_RIM, OPT_RELIEF, OPT_SHARPEN, OPT_LIGHTNING, OPT_RAIN, OPT_TEXT, OPT_CROSS, OPT_MICE, OPT_SUBS,
        OPT_SAT, OPT_GAMMA, OPT_MOTION, OPT_PAD_SPEED, OPT_GYRO, OPT_GYRO_SPEED, OPT_VIBRATION, OPT_ASSIST, OPT_TORCH, OPT_RT, OPT_CROSS_HIDE, OPT_FREE_PLAY, OPT_ASPECT,
    };
    struct Item {
        Id id;
        std::wstring label, value, hint;
        bool enabled = true;
        int index = -1;   // BOSS_ENTRY: which boss
    };
    struct Mask {   // text coverage (0..255) with `pad` pixels around the text box
        int w = 0, h = 0, pad = 0, text_w = 0, text_h = 0;
        int grad0 = 0, grad1 = 0;   // rows of the colour gradient (top of the capitals to the baseline)
        std::vector<uint8_t> a, e;  // letters, letters grown by the outline width
    };
    struct Style {
        uint32_t top, bottom;   // gradient (0xRRGGBB)
        uint32_t edge = 0;      // outline colour
        int edge_px = 0;
        float alpha = 1;
        int shadow = 0;         // drop shadow offset (pixels), black
    };
    enum Family { TITLE, ITEM, MONO, JURASSIC };
    // Titles and menu entries: the Jurassic Park font when it is installed
    Family display() const { return jurassic_ ? JURASSIC : TITLE; }

    std::vector<Item> items() const;
    void activate(const Item& it, int dir);
    void back();
    void push(Action a) { actions_.push_back(a); }
    void select(int index);

    HFONT font(Family f, int px);
    const Mask& mask(Family f, int px, const std::wstring& text, int edge_px);
    int text(Family f, int px, const std::wstring& s, int x, int y, int align, const Style& st);
    int measure(Family f, int px, const std::wstring& s);
    void fill(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha);
    void band(int cx, int y0, int y1, int half_w, uint32_t rgb, float alpha);
    void triangle(int x, int y, int size, int dir, uint32_t rgb, float alpha);
    void blend(int x, int y, uint32_t rgb, float a);
    void background();

    Screen screen_ = Screen::None;
    bool options_from_pause_ = false;
    bool boss_over_game_ = false;
    int selected_ = 0, hovered_ = -1;
    bool dirty_ = true;
    std::vector<Action> actions_;
    std::vector<RECT> hit_;   // item rectangles of the last draw
    struct ValueArea { int left, middle; };   // option rows: value column (left < 0: none)
    std::vector<ValueArea> values_;
    bool wrap_ = false;
    // Settings tabs (tab_of in frontend.cpp says where each option goes)
    enum { DISPLAY_TAB, LIGHTING_TAB, ATMOSPHERE_TAB, AUDIO_TAB, CONTROLS_TAB, GAME_TAB };
    static constexpr int TAB_COUNT = 6;
    static const wchar_t* const TAB_NAMES[TAB_COUNT];
    static int tab_of(Id id);
    struct OptionTab { Id id; int tab; };
    static const OptionTab OPTION_TABS[];   // frontend.cpp: where each option goes
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
    static constexpr int VISIBLE_RECORDS = 7;
    int record_scroll_ = 0;
    void scroll_records(int by);
    void draw_records(int x0, int x1, int y0, int row_h);
    // A settings tab shows this many options at most; the rest scroll
    static constexpr int VISIBLE_OPTIONS = 6;
    int scroll_ = 0;           // first option shown
    RECT more_up_{}, more_down_{};   // clickable "more" arrows of a scrolled tab
    std::vector<RECT> tab_rects_;   // a step past the end starts again (clicks on the label)
    int mouse_x_ = -1, mouse_y_ = -1;

    std::vector<HANDLE> font_handles_;
    std::vector<std::vector<char>> font_data_;
    std::map<std::pair<int, int>, HFONT> fonts_;
    std::map<std::wstring, Mask> masks_;
    HDC dc_ = nullptr;
    bool jurassic_ = false;   // third_party/fonts/Jurassic_Park.ttf loaded

    int w_ = 0, h_ = 0;
    float u_ = 1;   // pixels per unit of a 1920x1080 layout
    uint32_t* px_ = nullptr;
    std::vector<uint32_t> bg_;   // main menu backdrop for the current size
    std::vector<uint32_t> bg_stone_;   // with a poster: its stone only, without the logo (options)
    // Optional poster (third_party/menu/cover.png, not in the repository):
    // portrait picture with the logo at the top and plain dark stone below.
    // It becomes the backdrop and the title; its stone continues at the sides.
    std::vector<uint32_t> poster_;
    int poster_w_ = 0, poster_h_ = 0;
    bool load_poster(const std::wstring& path);
    bool has_poster() const { return poster_w_ > 0; }
    void poster_background(bool stone_only, std::vector<uint32_t>& dst);
    void poster_layout(float& scale, float& y0) const;
};
