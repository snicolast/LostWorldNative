// The window side: command line and settings, the Vulkan window, the
// presentation loop (2D composition, scene building, rendering, captures),
// the menus and the boss and survival mode screens. The emulator runs on its
// own thread in runtime.cpp; runtime.h is what this side uses of it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <dwmapi.h>     // DwmGetCompositionTimingInfo
#include <mmsystem.h>   // timeBeginPeriod
#include <windowsx.h>   // GET_X_LPARAM

#include "app_input.h"
#include "boss_mode.h"
#include "bosses.h"
#include "frame_prep.h"
#include "debug_aids.h"
#include "debug_snapshot.h"
#include "game_ram.h"
#include "gamepad.h"
#include "inputs.h"
#include "options.h"
#include "ppc_rt.h"
#include "raw_mice.h"
#include "rom_zip.h"
#include "settings_bridge.h"
#include "runtime.h"
#include "stats.h"
#include "random_spawns.h"
#include "rom_load.h"
#include "survival.h"
#include "weather.h"
#include "../audio/sound_board.h"
#include "../common/clock.h"
#include "../common/game_hz.h"
#include "../common/paths.h"
#include "../common/worker.h"
#include "../ui/achievement_toast.h"
#include "../ui/crosshair_file.h"
#include "../ui/frontend.h"
#include "../ui/menu_music.h"
#include "../ui/survival_ui.h"
#include "../video/image_io.h"
#include "../video/pack2d.h"
#include "../video/r3d_renderer.h"
#include "../video/r3d_scene.h"
#include "../video/r3d_state.h"
#include "../video/sky_pack.h"
#include "../video/text_hd.h"
#include "../video/texture_pack.h"
#include "../video/tilegen.h"
#include "../video/ui_overlay.h"
#include "../video/vk_presenter.h"

static Options g_opt;   // command line and menu settings (options.h)
static Controls g_controls;   // keyboard, mice and guns, gamepads (app_input.h)

// EXPERIENCE ARCADE ORIGINAL runs (save/settings.ini's experience = arcade): no main
// menu, the attract plays, coins and START; Esc opens its own pause (frontend.cpp)
static bool g_arcade = false;

// The stage select screen marks the gun with a small textured dot in its top
// viewport; in a stage the game draws an untextured X instead
static bool select_dot_on_screen(const R3DScene& scene) {
    if (scene.viewports.empty()) return false;
    const R3DViewport& v = scene.viewports.back();   // highest priority
    for (uint32_t i = 0; i < v.trans_count; i++)
        if (scene.vertices[v.trans_first + i].tex == 0x625A0340) return true;
    return false;
}

static const float WEATHER_SOUND_LEVEL[4] = {0.0f, 0.5f, 1.0f, 1.6f};   // WEATHER SOUND: OFF, LOW, MEDIUM, HIGH

// The sound options (at the start and from the menu)
static void apply_sound_options(const Options& o) {
    g_sound.set_volume(o.volume);
    Scsp::set_cubic(o.cubic_samples);
    g_sound.set_rear_boost(o.rear_boost);
    g_sound.weather().set_level(WEATHER_SOUND_LEVEL[std::clamp(o.weather_sound, 0, 3)]);
    g_audio.set_speakers(AudioStream::Speakers(o.speakers));
}

// crosshair.png / crosshair_p2.png beside LostWorld.exe (ui/crosshair_file.h)
// replace the drawn crosshairs unless Crosshair style is Default; again after
// the menu picks a new one
static void load_crosshairs(R3DRenderer& renderer) {
    for (int p = 0; p < 2; p++) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (g_opt.custom_crosshair && crosshair_load(p, w, h, rgba)) printf("[video] player %d crosshair: %dx%d picture\n", p + 1, w, h);
        renderer.set_crosshair_picture(p, w, h, rgba);
    }
}

// When the image being recorded will be on screen: with vsync the images go
// out once per refresh, evenly, while this thread wakes up for them at uneven
// moments (it waits for the GPU and the display, then builds the scene). The
// moment is the next vblank of the monitor, which Windows reports (DWM); if
// it does not, a clock that advances one refresh per image and follows the
// earliest wake-ups (never earlier than a vblank). Either way it moves on by
// whole refreshes, and starts again after a stall (a window drag, a slow frame).
// DWM's vblanks are those of one monitor (the main one): they are not used
// when the window's monitor runs at another refresh rate (two monitors at 60
// and 144 Hz). When the images do not go out one per refresh (VSYNC: OFF or
// vsync turned off in the driver, G-Sync/FreeSync below the monitor's rate,
// refreshes missed) they are not tied to vblanks either: each goes out about
// as soon as it is ready, so its moment is about when this thread prepares
// it (it waits for the previous presents first, so its wake-ups follow
// theirs), evened out over the last ones
struct DisplayClock {
    enum Mode { Vblanks, Refreshes, Presents };
    double at = 0, last = 0, period = 1.0 / 60;
    double interval = 0;      // between wake-ups, averaged over ~100
    double monitor_hz = 0;    // the refresh rate of the window's monitor (0: unknown)
    double checked = -100;    // when monitor_hz was read
    Mode mode = Vblanks;
    bool told = false;        // the mode was printed
    double tick(HWND hwnd) {
        double now = now_seconds();
        if (last > 0 && now - last < 0.1) {
            double dt = std::clamp(now - last, 1.0 / 1000, 0.05);
            interval = interval > 0 ? interval + 0.01 * (dt - interval) : dt;
        }
        if (now - checked > 1) {   // the window may have moved to another monitor
            checked = now;
            monitor_hz = 0;
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
                EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
                monitor_hz = dm.dmDisplayFrequency;
        }
        DWM_TIMING_INFO info{};
        info.cbSize = sizeof(info);
        bool dwm = SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) && info.qpcRefreshPeriod > 0;
        double f = double(g_qpf.QuadPart), dwm_period = dwm ? double(info.qpcRefreshPeriod) / f : 0;
        // (59 for 59.94 Hz: whole numbers)
        bool other_monitor = dwm && monitor_hz > 0 && std::abs(1 / dwm_period - monitor_hz) > 1.5;
        double refresh = dwm && !other_monitor ? dwm_period : monitor_hz > 0 ? 1 / monitor_hz : period;
        // One image per refresh: the wake-ups at the refresh rate, within 2%
        // (4% to stop: a refresh missed now and then is still one per refresh)
        double off = interval > 0 ? std::abs(interval / refresh - 1) : 0;
        bool per_refresh = mode == Presents ? off < 0.02 : off < 0.04;
        Mode was = mode;
        mode = !per_refresh ? Presents : dwm && !other_monitor ? Vblanks : Refreshes;
        if (mode != was || !told) {
            told = true;
            if (mode == Vblanks)
                printf("[video] display clock: vblanks reported by Windows (%.1f Hz)\n", 1 / dwm_period);
            else if (mode == Refreshes)
                printf("[video] display clock: one image per refresh, timed from the presents (the window's monitor "
                       "%.0f Hz, Windows reports %.1f Hz)\n", monitor_hz, dwm ? 1 / dwm_period : 0.0);
            else
                printf("[video] display clock: each image when it is ready (%.1f images per second on a %.0f Hz "
                       "monitor: vsync off, G-Sync/FreeSync or refreshes missed)\n",
                       1 / interval, 1 / refresh);
            fflush(stdout);
            period = refresh;
            at = 0;
        }
        if (mode == Presents) {
            if (dwm && !other_monitor) period = dwm_period;
            // The wake-ups are less even than the images going out (the
            // thread is woken late now and then): the moment moves on by
            // their average interval and follows them by a tenth of the
            // difference, starting again when one is off by a whole interval
            double next = at + interval;
            at = at > 0 && interval > 0 && std::abs(now - next) < interval ? next + 0.1 * (now - next) : now;
            last = now;
            return at;
        }
        if (mode == Vblanks) {
            double vblank = double(info.qpcVBlank) / f;
            period = dwm_period;
            double next = vblank + std::ceil((now - vblank) / period) * period;
            // One refresh per image unless one was really skipped
            at = at > 0 && next < at + 0.5 * period && next > at - 2 * period ? at + period : next;
            last = now;
            return at;
        }
        if (last > 0 && now - last < 0.1) period += 0.002 * (std::clamp(now - last, 1.0 / 1000, 0.05) - period);
        last = now;
        if (at > 0 && std::abs(now - (at + period)) < 2 * period) {
            at += period;
            at += now < at ? now - at : 0.01 * (now - at);
        } else {
            at = now;
        }
        return at;
    }
};

// Debugging aid: LW_PACING=1 measures how evenly the interpolated motion
// runs. Every 5 s it prints, over the presents, the game time shown per
// present divided by the time between their moments on screen (1.00 =
// perfectly even; "dispersion" is its standard deviation) and the presents
// where the picture stood still or went back, and the same for the previous
// method (frames dated when the game finished them and placed at the moment
// the image is recorded, no delay for late frames)
static const bool g_pacing_log = getenv("LW_PACING") != nullptr;

// Debugging aid: LW_FRAMELOG=path writes one line per present to that file
// (when the image was prepared, which game frames it shows, where the time
// went) and, with VK_KHR_present_wait, one per present when it reached the
// screen; the game thread adds one per game frame (runtime.cpp). Read with
// tools/frame_log.py
static FILE* g_frame_log = getenv("LW_FRAMELOG") ? fopen(getenv("LW_FRAMELOG"), "w") : nullptr;
static std::mutex g_frame_log_mutex;

void frame_log(const char* fmt, ...) {
    if (!g_frame_log) return;
    va_list args;
    va_start(args, fmt);
    std::lock_guard<std::mutex> lock(g_frame_log_mutex);
    vfprintf(g_frame_log, fmt, args);
    va_end(args);
}

static void pacing_sample(double now, double display, double shown, const R3DFrameState& built) {
    struct Method {
        double last_shown = 0, sum = 0, sum2 = 0;
        int n = 0, still = 0, back = 0;
        void add(double dt, double value) {
            if (last_shown != 0 && dt > 0) {
                double rate = (value - last_shown) / dt;
                if (std::abs(rate) < 4) {   // pauses and cuts are left out
                    sum += rate;
                    sum2 += rate * rate;
                    n++;
                    still += rate < 0.05;
                    back += rate < -0.05;
                }
            }
            last_shown = value;
        }
        void print(const char* name) {
            double mean = n ? sum / n : 0, dev = n ? std::sqrt(std::max(sum2 / n - mean * mean, 0.0)) : 0;
            printf("[pacing] %s: speed %.3f  dispersion %.3f  still %d  back %d  (%d presents)\n", name, mean, dev,
                   still, back, n);
            *this = Method{};
        }
    };
    static Method current, previous;
    static double last_display = 0, last_print = now, prev_flushed = 0, prev_time = 0;
    static double prev_period = 1 / GAME_HZ, game_period = 1 / GAME_HZ;
    static uint64_t prev_frame = 0;
    if (built.frame != prev_frame) {   // the previous method's frame period, from the finishing times
        if (prev_frame) {
            prev_period = std::clamp(built.flushed - prev_flushed, 1.0 / 240, 0.1);
            game_period = std::clamp(built.time - prev_time, 1.0 / 240, 0.1);
        }
        prev_frame = built.frame;
        prev_flushed = built.flushed;
        prev_time = built.time;
    }
    // Both shown as game time: the scenes themselves belong to their vblanks
    double t_old = std::clamp((now - built.flushed) / prev_period, 0.0, 1.0);
    double dt = display - last_display;
    current.add(dt, shown);
    previous.add(dt, built.time - game_period * (1 - t_old));
    last_display = display;
    if (now - last_print >= 5) {
        last_print = now;
        current.print("vblank  ");
        previous.print("previous");
        fflush(stdout);
    }
}

// The first start (no anti-aliasing in save/settings.ini yet) picks it for
// the GPU and the screen instead of MSAA 4x for everyone, as Sega Rally 2
// does: on an NVIDIA card DLSS is asked for, DLAA, or DLSS QUALITY on a screen
// larger than 1440p (the game starts in a window, but full screen is the
// screen's size). A card or a build without DLSS falls back to MSAA as before
static bool nvidia_card() {
    DISPLAY_DEVICEA d{};
    d.cb = sizeof(d);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &d, 0); i++)
        if (strstr(d.DeviceString, "NVIDIA")) return true;
    return false;
}

// The height in pixels of the screen the window is on (0: not known)
// (debugging aid: LW_SCREEN_HEIGHT=2160 stands for a screen of that height)
static int screen_height(HWND hwnd) {
    if (const char* e = getenv("LW_SCREEN_HEIGHT")) return atoi(e);
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return 0;
    if (!EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;   // (real pixels, whatever the scaling)
    return int(std::min(mode.dmPelsWidth, mode.dmPelsHeight));   // (the shorter side: a screen turned upright too)
}

bool g_restart = false;   // RESTART NOW: the game starts again once this one has closed
static void restart_game();   // (after App::shutdown)

// The game runs on unseen: at full speed, with nothing on screen and the sound
// muted, until runtime.cpp's boot skip ends (the boot screens, a game state
// loaded or PLAY's route selection reached): then the sound fades in
static void run_unseen() {
    g_boot_skip = g_fast_idle = true;
    g_sound.set_dsp_enabled(false);
    g_sound.set_muted(true);
}

// The zip the ROM came from (empty when it came from a data folder): the boss
// state makers are started on the same ROM
static std::string g_rom_zip;

// The window: the game runs on its own thread at real speed; this thread draws
// the latest image at the monitor's refresh rate (vsync). main() hands over
// to App::init, App::run and App::shutdown; what was its state is here
struct App {
    FrontendSettings ini;   // save/settings.ini (main): the experience running
    FrontendSettings other_ini;   // the other experience's (Frontend::other)
    bool arcade_section = false;  // the file has ARCADE ORIGINAL's [arcade] section (written again from then on)
    bool survival_cli = false;   // --survival STAGE:ZONE
    bool intro = false;          // --no-menu (not a test run): the splash, then the game
    int cli_stage = 0, cli_zone = 0;

    VkPresenter presenter;
    // --replay on a folder (recorded with LW_FILM): its snapshots one after
    // another, each captured to out/film_out, so effects that depend on the
    // camera motion can be compared frame by frame
    std::vector<std::string> film;
    size_t film_pos = 0;
    R3DRenderer renderer;
    // The 2D chain and the preparation thread (frame_prep.h): the 3D scene
    // builder, the texture and 2D replacement packs, the backdrops, HD text
    FramePrep prep;
    // The vblanks of the last two 2D frames shown (the sky goes on with the
    // camera while the 3D goes between the 3D frames of those same vblanks)
    uint64_t shift_from = ~0ull, shift_to = ~0ull, built_from = ~0ull;
    bool f12_was_down = false, f9_was_down = false;
    R3DScene own_scene;   // built on this thread (no interpolation)
    const R3DScene* scene = &own_scene;
    Layers2D own_layers;   // composed on this thread (no 3D, or no interpolation)
    const Layers2D* layers_shown = &own_layers;
    std::shared_ptr<Prepared> built_prep, layers_prep;   // the prepared frames in use (scene, 2D layers)
    std::shared_ptr<const R3DFrameState> built;
    double frame_period = 1.0 / GAME_HZ;   // between the frame built and the one before
    // Interpolation timing: the last frame the game published, the time
    // between its last two frames and how late after its vblank a frame gets
    // here (the game's CPU time plus this loop's wait for it; follows the
    // latest frames, easing down slowly)
    std::shared_ptr<const R3DFrameState> seen;
    double next_period = 1.0 / GAME_HZ, lateness = 0.004, lateness_rose = 0, lateness_spike = -100;
    DisplayClock display_clock;

    std::vector<uint16_t> tex_rows;
    uint64_t shown = ~0ull, presents = 0;
    double scene_total = 0;   // seconds of 2D composition and scene building since the last title
    double last_title = 0;   // now_seconds() of the last title update

    // PC front end: main menu at launch, pause menu on Esc
    Frontend frontend;
    // The menus are being driven with a gamepad: no mouse pointer until the mouse moves
    bool pad_in_menu = false;
    UiOverlay ui;
    std::vector<uint32_t> ui_pixels;
    float haze_strength = 1, ao_strength = 1;   // of --haze and --ao, for the options menu


    BossMode boss;   // the fights from a saved state per boss (boss_mode.h)

    // Survival mode screens (loading, start, wave counter, results)
    SurvivalUi survival_ui;
    UiOverlay survival_layer;
    std::vector<uint32_t> survival_pixels;
    bool survival_shown = false;
    bool confirm_was_down = true;
    // A run starts from the arcade game at its attract (the home state: kept
    // the first time, put back the next ones), fast-forwarded and muted
    // behind the loading screen; Esc on the results goes back to the menu
    // PLAY > CLASSIC: the menu shows LOADING while the game runs on to its
    // route selection unseen (g_arcade_entry, runtime.cpp), then closes on it
    bool play_waits = false;
    bool survival_cli_pending = false;   // waits for the boot screens
    bool survival_wait_home = false;
    uint64_t survival_audio_at = 0;   // audio frames out when the loading began (it stays silent)
    SurvivalMode survival_last = SurvivalMode::Off;

    // Achievement notices, over the game and under the menus
    AchievementToast toast;
    UiOverlay toast_layer;
    std::vector<uint32_t> toast_pixels;
    bool toast_shown = false;
    MenuMusic music;   // over the menus' backdrop, when there is a music file
    double music_time = 0;

    // LW_FRAMELOG: a thread notes when each present reaches the screen
    std::atomic<uint64_t> presented_id{0};
    std::atomic<bool> threads_stop{false};
    std::thread shown_waiter;

    // At most this many images wait for the screen (VK_KHR_present_wait): the
    // picture goes out that many refreshes after it is drawn (~17 ms at
    // 120 Hz) instead of after a full swapchain (~32 ms). When refreshes are
    // missed twice within 2 s (the GPU busier than usual), one more for 10 s.
    // Debugging aid: LW_QUEUE=N (0 = the whole swapchain)
    int queue_depth = 2;
    int queue_extra = 0;
    double went_out = 0, last_miss = -100;
    bool repeat_image = false;   // MAX FPS below the refresh rate (vsync): this refresh shows the last image again
    bool in_background = false;  // BACKGROUND FPS: another window has the focus (wait_for_refresh)
    int repeat_count = 0;        // refreshes since the last image drawn
    // VSYNC: OFF still draws no more images than the monitor's refreshes:
    // without a limit the GPU draws thousands a second for nothing and the
    // game thread falls behind (uneven game frames). Measured with
    // LW_FRAMELOG on an RTX 5070 Ti at 120 Hz: ~1700 images a second
    double next_image = 0;
    HANDLE image_timer = nullptr;

    bool init(bool aa_auto);   // false: could not start. aa_auto: the first start's anti-aliasing (nvidia_card)
    void run();                                            // until the window closes
    [[noreturn]] void shutdown();
    // Both threads stop when the loop ends (and with the App, however it ends)
    void stop_threads();
    ~App() { stop_threads(); }

    void survival_to_menu();
    void open_menu(Frontend::Screen s);   // a menu over the game, which stands still
    void close_menu();
    void start_survival(int stage, int zone);
    void survival_go_on(const SurvivalView& sv);
    void apply_2d(const Layers2D& l);
    void give_pictures(uint64_t serial);
    // run(), one refresh after another
    void wait_for_refresh();
    void update_survival();
    void update_toast();
    void handle_menu_actions();
    void save_settings();
    void start_arcade();
    void cancel_arcade_entry();
    void update_arcade_entry();
    void quit_to_menu();
    void cancel_play();
    bool menu_key(int vk);
    void game_escape();
    // The video options the menu changes, to the renderer and the presenter:
    // at the start (was = nullptr; the 3D resolution, MSAA, anisotropic
    // filtering and vsync were given to init) and after a change in the menu
    void apply_video_options(const Options* was);
    void update_boss_mode();   // boss.update with this window's steps
    void update_menus();
    void update_intro();
    void read_inputs();
    bool present_frame(double loop_from);
    void update_title();
    void note_presents();    // the LW_FRAMELOG thread
};

// Survival results: Esc goes back to the main menu, and the arcade game as
// it was comes back when the menu closes
void App::survival_to_menu() {
    survival_end();
    game_state_load_home();
    open_menu(Frontend::Screen::Main);
}

void App::open_menu(Frontend::Screen s) {
    frontend.open(s);
    g_controls.menu_open = g_hold = true;
}

void App::close_menu() {
    frontend.open(Frontend::Screen::None);
    ui.hide();
    g_controls.menu_open = g_hold = false;
    g_controls.block_buttons = true;
}

// A survival run at that arena, from the home state (see survival_wait_home)
void App::start_survival(int stage, int zone) {
    if (game_state_has_home()) game_state_load_home();
    else game_state_keep_home();
    survival_wait_home = true;
    survival_audio_at = g_sound.frames_out();
    survival_begin(stage, zone);
    run_unseen();
    if (g_opt.menu) close_menu();
    else g_hold = false;
}

// A shot on the start screen begins the hunt; on the results, a new one
// from the very start (the arena's first spot and encounter), as from the menu
void App::survival_go_on(const SurvivalView& sv) {
    if (sv.mode == SurvivalMode::Results) {
        start_survival(sv.stage, sv.zone);
        return;
    }
    survival_confirm();
    g_hold = false;
}

// Hands composed 2D layers to the renderer (their new pictures go separately)
void App::apply_2d(const Layers2D& l) {
    renderer.set_sky_coords(l.sky_coords.data());
    renderer.set_sky_rows(l.frame, l.back_rows.data());
    renderer.set_hud_band(l.hud_band);
    renderer.set_overlay_brightness(l.overlay_brightness);
    renderer.set_text_cells(l.text_cells.data());
    renderer.set_layers(l.back.data(), l.front.data());
    shift_from = l.shift_from;
    shift_to = l.shift_to;
    shown = l.frame;
    layers_shown = &l;
}

// Hands the new pictures of the frames up to that one to the renderer
void App::give_pictures(uint64_t serial) {
    std::vector<NewPictures> due;
    {
        std::lock_guard<std::mutex> lock(prep.ready_mutex);
        while (!prep.ready_pictures.empty() && prep.ready_pictures.front().serial <= serial) {
            due.push_back(std::move(prep.ready_pictures.front()));
            prep.ready_pictures.pop_front();
        }
    }
    for (NewPictures& n : due) {
        if (!n.replacements.empty()) renderer.add_replacements(n.replacements);
        if (!n.overlays.empty()) renderer.add_overlays(n.overlays);
        if (!n.sky.empty()) renderer.add_sky_pictures(n.sky);
    }
}

void App::note_presents() {
    uint64_t next = 1;
    while (!threads_stop) {
        if (next > presented_id) {
            Sleep(1);
            continue;
        }
        if (presenter.wait_present(next, 100'000'000))
            frame_log("S %llu %.6f\n", (unsigned long long)next, now_seconds());
        next++;
    }
}

void App::stop_threads() {
    threads_stop = true;
    prep.stop();
    if (shown_waiter.joinable()) shown_waiter.join();
}

bool App::init(bool aa_auto) {
    presenter.set_vsync(g_opt.vsync);
    // (the window no wider than 21:9 at first, whatever WIDESCREEN allows: a 32:9 one would not fit most desktops)
    if (!presenter.init("Lost World Native by snicolast", int(SCREEN_H * 2 * std::clamp(g_opt.max_aspect, 4.0f / 3.0f, 2.4f)), SCREEN_H * 2,
                        g_opt.dlaa)) {
        fprintf(stderr, "Could not start Vulkan; try --headless\n");
        MessageBoxA(nullptr, "Could not start Vulkan. Update the graphics driver: the game needs Vulkan 1.2 or newer.",
                    "Lost World Native", MB_OK | MB_ICONERROR);
        return false;
    }
    printf("GPU: %s\n", presenter.device_name().c_str());
    if (aa_auto) g_opt.dlss = screen_height(presenter.hwnd()) > 1440 ? 1 : 0;   // DLSS QUALITY above 1440p
    g_controls.pads.trigger_rumble = g_opt.trigger_rumble;
    g_controls.pads.init();
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_start_time);
    g_realtime = !getenv("LW_FAST");   // debugging aid: a window at full speed
    DWORD replay_attr = g_opt.replay.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesA(g_opt.replay.c_str());
    if (replay_attr != INVALID_FILE_ATTRIBUTES && (replay_attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((g_opt.replay + "/*.bin").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do film.push_back(g_opt.replay + "/" + fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(film.begin(), film.end());
        if (film.empty()) {
            fprintf(stderr, "No snapshots in %s\n", g_opt.replay.c_str());
            return false;
        }
        g_opt.replay = film[0];
        CreateDirectoryA("out/film_out", nullptr);
    }
    if (!g_opt.replay.empty()) {   // --replay: a frozen debug snapshot, the game does not run
        if (!load_snapshot(g_opt.replay.c_str())) {
            fprintf(stderr, "Could not read the snapshot %s\n", g_opt.replay.c_str());
            return false;
        }
        printf("Snapshot %s (frame %llu)\n", g_opt.replay.c_str(), (unsigned long long)g_video_frame);
    } else {
        // Records and achievements: real games only, never the scripted or cheated test runs
        if ((g_opt.menu || survival_enabled()) && g_auto.events.empty() && !getenv("LW_SURV_GOD") &&
            !getenv("LW_SURV_LIFE"))
            stats_init();
        g_audio.start();
        intro = intro && !survival_cli && g_opt.replay.empty() && !g_opt.shot_frame && ini.splash &&
                !getenv("LW_MENU_KEYS") && !getenv("LW_MENU_SHOT") && !getenv("LW_MENU_FILM");
        g_hold = ((g_opt.menu && !g_arcade) || intro) && !survival_cli;   // the main menu (or the splash) comes first
        g_boot_skip = g_fast_idle = !g_opt.show_boot;
        g_sound.set_dsp_enabled(!g_boot_skip);
        g_sound.set_muted(g_boot_skip);
        CreateThread(nullptr, 512u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }

    if (!renderer.init(presenter, g_opt.supersample, g_opt.msaa, g_opt.aniso, g_opt.dlaa, g_opt.dlss, g_opt.dlss_model)) {
        fprintf(stderr, "Could not start the 3D renderer on %s (see the [vulkan] line above)\n",
                presenter.device_name().c_str());
        MessageBoxA(nullptr, "Could not start the 3D renderer. Update the graphics driver.", "Lost World Native",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    renderer.set_hud_edges(!g_opt.hud_centre);
    if (g_opt.haze > 0) printf("[video] distance haze on (strength %.2f)\n", g_opt.haze);
    // DLAA or DLSS asked for on a card without them (not an RTX): FXAA instead
    // (the renderer already started without MSAA; the menu shows it)
    if (g_opt.dlaa && !renderer.dlaa_active()) {
        printf("[video] DLAA/DLSS not available on %s: using FXAA\n", presenter.device_name().c_str());
        g_opt.dlaa = false, g_opt.dlss = 0, g_opt.fxaa = true, g_opt.msaa = 1;
    }
    if (aa_auto) printf("[video] first start: anti-aliasing chosen for this GPU and screen\n");
    if (renderer.dlaa_active())
        printf("[video] antialiasing NVIDIA %s, anisotropic filtering %dx%s\n",
               !renderer.dlss_upscaling() ? "DLAA"
               : g_opt.dlss == 2          ? "DLSS BALANCED"
               : g_opt.dlss == 3          ? "DLSS PERFORMANCE"
               : g_opt.dlss == 4          ? "DLSS ULTRA PERFORMANCE"
                                          : "DLSS QUALITY",
               g_opt.aniso, g_opt.aniso > 0 ? " with mipmaps" : " (no mipmaps)");
    else if (g_opt.fxaa)
        printf("[video] antialiasing FXAA, anisotropic filtering %dx%s\n", g_opt.aniso,
               g_opt.aniso > 0 ? " with mipmaps" : " (no mipmaps)");
    else
        printf("[video] antialiasing MSAA %dx, anisotropic filtering %dx%s\n", renderer.msaa_samples(), g_opt.aniso,
               g_opt.aniso > 0 ? " with mipmaps" : " (no mipmaps)");
    if (g_opt.light_gain != 1.0f) {
        renderer.set_light_gain(g_opt.light_gain);
        printf("[video] light x%.2f on the sun and the ambient light\n", g_opt.light_gain);
    }
    prep.builder.set_remove_letterbox(!g_opt.letterbox);
    prep.builder.set_hud_edges(!g_opt.hud_centre);
    prep.pack.init("textures", g_opt.dump_textures ? "out/textures" : "");
    prep.pack.set_max_slots(renderer.replacement_slots());   // as many as this GPU's descriptor limits allow
    prep.builder.set_texture_pack(&prep.pack, &g_texture_ram);
    prep.pack2d.init("textures/2d", 1, 27);
    prep.pack2d_back.init("textures/2d_back", 28, SkyPack::FIRST_SLOT - 1);
    prep.sky.init("textures/sky", g_opt.dump_textures ? "out/sky" : "");
    haze_strength = g_opt.haze > 0 ? g_opt.haze : 1;
    ao_strength = g_opt.ao > 0 ? g_opt.ao : 1;
    apply_video_options(nullptr);   // the menu's video options (and the sky's picture)
    if ((g_opt.menu || !g_opt.classic_text) && prep.text.init("textures/text")) renderer.set_text_atlas(prep.text);
    load_crosshairs(renderer);
    prep.builder.set_vrom(g_vrom.data(), g_vrom.size());
    last_title = now_seconds();
    raw_mice::init(presenter.hwnd());   // a second mouse can be player 2's gun
    if (g_opt.several_mice) raw_mice::set_saved(ini.gun_p1, ini.gun_p2);   // the guns picked in GUN TEST
    presenter.raw_input = raw_mice::on_input;
    if (g_opt.menu) {
        if (!frontend.init("third_party/fonts")) printf("[menu] fonts missing: using the Windows ones\n");
        if (!ui.init(presenter)) g_opt.menu = false;
    }
    if (intro) {   // the frontend only for the splash, which then hands over to the game
        frontend.init("third_party/fonts");
        frontend.settings = ini;
        settings_from_options(g_opt, frontend.settings);
        frontend.after_splash = Frontend::Screen::None;
        if (ui.init(presenter)) frontend.open_splash();
        intro = frontend.splash();
        if (intro)
            presenter.input_hook = [this](UINT msg, WPARAM wp, LPARAM lp) -> bool {
                if (!intro) return false;
                if (msg == WM_KEYDOWN) frontend.key(int(wp));
                else if (msg == WM_LBUTTONDOWN) frontend.click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return true;
            };
        else g_hold = false;
    }
    presenter.save_pipeline_cache();   // what start-up compiled, kept even if the run ends without shutdown()
    if (g_opt.menu) {
        FrontendSettings& s = frontend.settings;
        s = ini;   // the window, the menu music, the gamepad and the guns picked: as the file has them
        settings_from_options(g_opt, s);
        frontend.other = other_ini;
        frontend.arcade = g_arcade;
        frontend.aa_at_launch = s.aa;
        frontend.rt_available = renderer.ray_tracing_available();
        frontend.resolutions = presenter.display_modes();
        if (s.res_w > 0 && !frontend.resolutions.empty()) {   // a size that is not one of the monitor's modes: the nearest
            auto best = frontend.resolutions[0];
            for (const auto& m : frontend.resolutions)
                if (std::abs(m.first - s.res_w) + std::abs(m.second - s.res_h) <
                    std::abs(best.first - s.res_w) + std::abs(best.second - s.res_h))
                    best = m;
            if (best.first != s.res_w || best.second != s.res_h) {
                printf("[display] resolution %dx%d is not a mode of the monitor: %dx%d\n", s.res_w, s.res_h, best.first, best.second);
                s.res_w = best.first, s.res_h = best.second;
            }
        }
        printf("[display] %zu resolutions\n", frontend.resolutions.size());
        if (s.display_mode != 0 || (s.res_w > 0 && s.res_h > 0)) {
            presenter.set_mode(s.display_mode, s.res_w, s.res_h);
            s.display_mode = presenter.mode();   // (a FULLSCREEN the monitor refused: BORDERLESS)
        }
        if (g_arcade) {   // ARCADE ORIGINAL: the game at once, its attract (Esc: the pause)
            frontend.open(Frontend::Screen::None);
            printf("[experience] ARCADE ORIGINAL: no main menu, the attract plays (Esc pauses)\n");
        } else if (!survival_cli) {   // survival mode goes straight into the game
            // the logos first (not when a debugging aid drives the menu)
            if (getenv("LW_MENU_KEYS") || getenv("LW_MENU_SHOT") || getenv("LW_MENU_FILM") || getenv("LW_RESTARTED"))
                frontend.open(Frontend::Screen::Main);
            else frontend.open_splash();
            g_controls.menu_open = true;
        }
        presenter.input_hook = [this](UINT msg, WPARAM wp, LPARAM lp) -> bool {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (!frontend.active()) {   // in the game: Esc pauses
                if (msg == WM_KEYDOWN && wp == VK_ESCAPE && survival_view().mode == SurvivalMode::Results) {
                    survival_to_menu();
                    return true;
                }
                if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
                    game_escape();
                    return true;
                }
                return false;
            }
            switch (msg) {
            case WM_KEYDOWN: return menu_key(int(wp));
            case WM_MOUSEMOVE: {
                // Windows also sends it when nothing moved (a window or the
                // pointer changing): those neither bring the pointer back nor
                // take the highlight from the pad's choice
                static int last_x = INT_MIN, last_y = INT_MIN;
                bool moved = x != last_x || y != last_y;
                last_x = x, last_y = y;
                if (!moved) return true;
                pad_in_menu = false;
                frontend.mouse_move(x, y);
                return true;
            }
            case WM_LBUTTONDOWN:
                if (!play_waits) frontend.click(x, y);   // (PLAY on its way: only Esc, menu_key)
                return true;
            case WM_RBUTTONDOWN:   // right button: back (not in GUN TEST, where it is a gun reloading)
                if (!frontend.gun_test_open()) menu_key(VK_ESCAPE);
                return true;
            case WM_MOUSEWHEEL: {
                if (play_waits) return true;
                POINT p{x, y};   // screen coordinates for this one
                ScreenToClient(presenter.hwnd(), &p);
                frontend.wheel(p.x, p.y, GET_WHEEL_DELTA_WPARAM(wp));
                return true;
            }
            }
            return false;
        };
    }
    boss.rom_zip = g_rom_zip;
    boss.init(frontend);
    // Survival mode screens (loading, start, wave counter, results)
    survival_shown = (survival_cli || g_opt.menu) && survival_ui.init("third_party/fonts") &&
                     survival_layer.init(presenter);
    prep.survival_shown = survival_shown;
    survival_cli_pending = survival_cli;
    // Achievement notices, over the game and under the menus
    toast_shown = toast.init("third_party/fonts") && toast_layer.init(presenter);
    toast.set_volume(g_opt.volume);
    if (g_opt.menu && !g_opt.headless && music.open())   // heard through the game's own output
        g_audio.set_extra([](void* m, float* out, size_t frames, int channels, int rate) {
            static_cast<MenuMusic*>(m)->mix(out, frames, channels, rate);
        }, &music);
    music_time = now_seconds();
    prep.start(&g_opt);
    if (g_frame_log && presenter.can_wait_present()) shown_waiter = std::thread([this] { note_presents(); });
    queue_depth = getenv("LW_QUEUE") ? atoi(getenv("LW_QUEUE")) : 2;
    image_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return true;
}

void App::run() {
    for (;;) {
        wait_for_refresh();   // before the inputs are read
        if (!presenter.pump()) break;
        double loop_from = now_seconds();
        update_survival();
        update_toast();
        if (g_opt.menu) {
            update_arcade_entry();
            handle_menu_actions();
            update_boss_mode();
            update_menus();
        }
        if (intro) update_intro();
        read_inputs();
        if (!present_frame(loop_from)) break;   // --shot taken, or the end of a --replay film
    }
}

// The present queue and, with VSYNC off, the wait for the next refresh
void App::wait_for_refresh() {
    int depth = queue_depth ? queue_depth + queue_extra : 0;
    double out = presenter.limit_queue(depth, 50'000'000);
    if (out > 0 && went_out > 0 && out - went_out > 1.5 * display_clock.period) {
        if (out - last_miss < 2 && !queue_extra) {
            queue_extra = 1;
            printf("[video] refreshes missed: %d images queued for a while\n", depth + 1);
        }
        last_miss = out;
    }
    if (queue_extra && out - last_miss > 10) {
        queue_extra = 0;
        out = 0;   // the next wait skips a refresh: not a miss
    }
    went_out = out;
    // MAX FPS with MOTION SMOOTH: UNLIMITED, nothing waited for; a number, its interval
    double period = !g_opt.interp || g_opt.max_fps == 0 ? display_clock.period
                    : g_opt.max_fps > 0                 ? std::max(1.0 / g_opt.max_fps, 1.0 / 1000)
                                                        : 0;
    // BACKGROUND FPS: 30 a second at most while another window has the focus,
    // with vsync too (each image then waits for its refresh as well)
    bool background = g_opt.menu && frontend.settings.background_fps && GetForegroundWindow() != presenter.hwnd();
    if (background != in_background) {
        in_background = background;
        printf(background ? "[video] in the background: 30 images a second at most\n" : "[video] in front again\n");
    }
    if (background) period = std::max(period, 1.0 / 30);
    if ((!g_opt.vsync || background) && period > 0) {   // the next image an interval after the last (from now after a slow one)
        double now = now_seconds();
        if (now - next_image > period) next_image = now;
        if (image_timer && next_image - now > 0.001) {   // to ~0.5 ms before, then a spin
            LARGE_INTEGER due;
            due.QuadPart = -LONGLONG((next_image - now - 0.0005) * 1e7);
            if (SetWaitableTimer(image_timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(image_timer, 100);
        } else if (!image_timer) {
            while (next_image - now > 0.002) Sleep(1), now = now_seconds();
        }
        while (now_seconds() < next_image) YieldProcessor();
        next_image += period;
    }
}

// Survival: the start from the command line, the home state, its screens
void App::update_survival() {
    if (survival_cli_pending && !g_boot_skip) {   // at the SEGA logo: the home state is the game from here
        survival_cli_pending = false;
        start_survival(cli_stage, cli_zone);
    }
    if (survival_wait_home && game_state_result() != 0) {
        survival_wait_home = false;
        survival_ungate();
    }
    if (survival_shown) {
        SurvivalView sv = survival_view();
        if (survival_last == SurvivalMode::Loading && sv.mode != SurvivalMode::Loading)
            printf("[survival] audio during the loading: %llu samples\n",
                   (unsigned long long)(g_sound.frames_out() - survival_audio_at));
        if (survival_last == SurvivalMode::Loading && sv.mode == SurvivalMode::Off && g_opt.menu && !g_controls.menu_open)
            survival_to_menu();   // the game never got to the arena: not left running behind the loading screen
        survival_last = sv.mode;
        if (survival_cli_pending) {   // the boot screens: already the loading screen
            sv.mode = SurvivalMode::Loading;
            sv.stage = cli_stage, sv.zone = cli_zone;
        }
        // A shot (or Enter / Space) on the start and results screens
        HWND hw = presenter.hwnd();
        bool fg = GetForegroundWindow() == hw;
        bool down = fg && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000) ||
                           (GetAsyncKeyState(VK_SPACE) & 0x8000));
        for (int i = 0; i < Gamepads::MAX; i++) down |= fg && g_controls.pads.pad(i).fire;
        // (the results only after a moment: the player is still shooting when the hunt ends)
        bool waiting = sv.mode == SurvivalMode::Ready ||
                       (sv.mode == SurvivalMode::Results && now_seconds() - sv.results_at > SURVIVAL_RESULTS_DELAY);
        // Where the game screen is in the window: for the BONUS target
        RECT rc;
        GetClientRect(hw, &rc);
        ScreenLayout lay = compute_layout(float(rc.right), float(rc.bottom), g_opt.max_aspect);
        SurvivalLayout surv_lay{lay.scale, lay.ox, lay.oy, lay.game_x0};
        if (waiting && down && !confirm_was_down && !frontend.active()) {
            survival_go_on(sv);
            g_controls.block_buttons = true;   // the click does not fire in the game
        }
        confirm_was_down = down;
        // Debugging aid: LW_SURV_SHOTS=1 captures each screen of the mode
        // (out/survival_<mode>.bmp) and then shoots by itself
        static const bool surv_shots = getenv("LW_SURV_SHOTS") != nullptr;
        static SurvivalMode shot_mode = SurvivalMode::Off;
        static double shot_since = 0;
        if (surv_shots) {
            if (sv.mode != shot_mode) shot_mode = sv.mode, shot_since = now_seconds();
            double t = now_seconds() - shot_since;
            if (t > 1.5 && t < 1.6 && shot_mode != SurvivalMode::Off) {
                shot_since -= 1;   // once
                presenter.capture_next("out/survival_" + std::to_string(int(shot_mode)) + ".bmp");
            }
            if (t > 3 && waiting) {   // LW_SURV_SHOTS=2: the results go back to the menu
                if (sv.mode == SurvivalMode::Results && getenv("LW_SURV_SHOTS")[0] == '2' && g_opt.menu) {
                    survival_to_menu();
                    shot_mode = SurvivalMode::Off;
                } else {
                    survival_go_on(sv);
                }
            }
        }
        if (survival_ui.draw(rc.right, rc.bottom, sv, now_seconds(), survival_pixels, surv_lay))
            survival_layer.set_image(survival_pixels.data(), rc.right, rc.bottom);
    }
}

// Achievement notices
void App::update_toast() {
    if (!toast_shown) return;
    // (ARCADE ORIGINAL: counted, in RECORDS once back in PC EDITION, but nothing over the game)
    for (int i : stats_take_new_achievements())
        if (!g_arcade) toast.push(achievement_table()[size_t(i)].name, achievement_table()[size_t(i)].text);
    for (int i : survival_take_unlocked())
        if (!g_arcade)
            toast.push_rising(L"ARENA UNLOCKED  \u00B7  SURVIVAL!", SURVIVAL_ZONES[i].name,
                              L"Play it from PLAY > SURVIVAL");
    // The author's credit over the attract (seen with the menu off, --no-menu)
    toast.set_credit(L"PC port by snicolast", L"github.com/snicolast/LostWorldNative",
                     g_in_attract && !g_controls.menu_open && !g_boot_skip && !g_arcade_entry && !g_arcade);
    RECT rc;
    GetClientRect(presenter.hwnd(), &rc);
    if (toast.draw(rc.right, rc.bottom, now_seconds(), toast_pixels))
        toast_layer.set_image(toast_pixels.data(), rc.right, rc.bottom);
}

// What was picked in the menus
void App::apply_video_options(const Options* was) {
    const Options& o = g_opt;
    if (was) {
        renderer.set_supersample(o.supersample);
        renderer.set_msaa(o.msaa);
        renderer.set_aniso(o.aniso);
        presenter.set_vsync(o.vsync);
    }
    renderer.set_fxaa(o.fxaa);   // at once, like MSAA (not while DLAA runs: R3DRenderer::fxaa)
    if (was && o.fxaa != was->fxaa && o.msaa == was->msaa && !renderer.dlaa_active())   // (with MSAA: apply_pending)
        printf("[video] antialiasing %s\n", o.fxaa ? "FXAA" : "off");
    renderer.set_smooth_2d(!o.pixel_2d);
    if (!was || o.max_aspect != was->max_aspect) renderer.set_max_aspect(o.max_aspect);
    renderer.set_distance_haze(o.haze > 0 ? haze_strength : 0);   // at the strength of the start (--haze)
    renderer.set_bloom(o.bloom);
    renderer.set_picture(o.saturation, o.gamma);
    renderer.set_ambient_occlusion(o.ao > 0 ? ao_strength : 0);   // and --ao
    renderer.set_sun(o.sun);
    renderer.set_shadows(o.shadows ? 1.0f : 0.0f);
    renderer.set_ray_tracing(o.ray_tracing);
    renderer.set_light_shafts(o.shafts ? 1.0f : 0.0f);
    renderer.set_soft_particles(o.soft);
    renderer.set_lightning(o.lightning);
    renderer.set_rain(o.rain);
    renderer.set_hdr(o.hdr);
    renderer.set_rim_light(o.rim);
    renderer.set_sharpen(o.sharpen ? 0.5f : 0.0f);
    renderer.set_modern_torch(o.modern_torch);
    if (!was || o.sky_procedural != was->sky_procedural) {
        {
            std::lock_guard<std::mutex> lock(prep.prep_mutex);   // not while composing
            prep.sky.set_procedural(o.sky_procedural);
        }
        renderer.set_night_sky(o.sky_procedural);
    }
}

// No attract (runtime.cpp, arcade_entry_frame): PLAY > CLASSIC runs the game
// on to its route selection behind the menu (LOADING), which closes once it is there
void App::start_arcade() {
    g_arcade_entry = 1;
    run_unseen();
    play_waits = true;
    frontend.starting = true;
    frontend.invalidate();
}

// Another mode picked while the game was on its way to the route selection
void App::cancel_arcade_entry() {
    play_waits = false;
    frontend.starting = false;
    g_arcade_entry = 0;
}

// QUIT TO MAIN MENU (the pause menu of any mode): the game at its home state
// again (the SEGA logo, before any credit), put back when the menu closes
void App::quit_to_menu() {
    boss.pending = -1;
    cancel_arcade_entry();
    if (survival_view().mode != SurvivalMode::Off) {
        survival_to_menu();
        return;
    }
    boss.to_menu(frontend);
}

// PLAY given up during LOADING (Esc, the pad's B or BACK, the right button),
// while the game runs on to its route selection behind the menu. As QUIT TO
// MAIN MENU: the home state is from before the credit PLAY put in, so none is left
void App::cancel_play() {
    printf("[play] LOADING given up: back to the main menu\n");
    quit_to_menu();
    g_controls.menu_open = g_hold = true;
}

// A key for the menu (the keyboard, a pad's buttons as keys, LW_MENU_KEYS).
// While the game runs on to its route selection (play_waits) Esc and BACK
// give PLAY up and the rest wait: the menu closes on its own then, and would
// close on whatever page they had opened
bool App::menu_key(int vk) {
    if (!play_waits) return frontend.key(vk);
    if (vk == VK_ESCAPE || vk == VK_BACK) cancel_play();
    return true;
}

// Esc in the game (or a pad's BACK or START): the pause menu, also on the route selection
void App::game_escape() {
    open_menu(Frontend::Screen::Pause);
}

void App::update_arcade_entry() {
    // Only the arcade game ends in the attract: the boss fights have their own end
    // (ARCADE ORIGINAL: always the attract, as on the cabinet)
    g_skip_attract = g_auto.events.empty() && boss.fight < 0 && !boss.loading && boss.pending < 0 && !g_arcade;
    // A game over: the main menu instead of the attract, the game back at its
    // home state behind it (attract_guard_frame)
    if (g_game_over_menu.exchange(false)) {
        play_waits = false;
        frontend.starting = false;
        open_menu(Frontend::Screen::Main);
    }
    if (play_waits && !g_arcade_entry && !g_boot_skip) {
        play_waits = false;
        frontend.starting = false;
        close_menu();
    }
}

// save/settings.ini: PC EDITION's settings at the top, ARCADE ORIGINAL's in its
// [arcade] section (once it has one, or once it is chosen)
void App::save_settings() {
    const FrontendSettings& s = frontend.settings;
    if (g_arcade) FrontendSettings::save_both("save/settings.ini", frontend.other, &s, s);
    else
        FrontendSettings::save_both("save/settings.ini", s, arcade_section || s.experience == 1 ? &frontend.other : nullptr, s);
}

void App::handle_menu_actions() {
    for (Frontend::Action a : frontend.take_actions()) {
        FrontendSettings& s = frontend.settings;
        switch (a) {
        case Frontend::Action::StartArcade:   // the game at the SEGA logo is the home state QUIT TO MAIN MENU puts back
            if (!g_boot_skip) game_state_keep_home();
            random_spawns_set(frontend.random_spawns);   // CLASSIC > ARCADE or RANDOM SPAWNS
            if (frontend.random_spawns) weather_choose(frontend.run_weather());
            boss.pending = -1;
            start_arcade();
            break;
        case Frontend::Action::Resume:
            boss.pending = -1;
            close_menu();
            break;
        case Frontend::Action::StartSurvival: {
            const SurvivalZone& z = SURVIVAL_ZONES[frontend.survival_choice];
            boss.pending = -1;
            cancel_arcade_entry();
            random_spawns_set(false);
            weather_choose(z.indoor ? WEATHER_NONE : frontend.run_weather());   // indoors: the game's own
            if (boss.fight >= 0 || frontend.boss_mode) boss.end(frontend);   // from boss mode: back to the arcade first
            start_survival(z.stage, z.zone);
            break;
        }
        case Frontend::Action::StartBoss:
            cancel_arcade_entry();
            random_spawns_set(false);
            // (not again while a fight is being put back behind the menu)
            if (frontend.boss_status[frontend.boss_choice] == Frontend::BOSS_READY && !boss.loading && boss.shown_at == 0)
                boss.pending = frontend.boss_choice;
            break;
        case Frontend::Action::PrepareBosses:
            for (int i = 0; i < BOSS_COUNT; i++)
                if (frontend.boss_status[i] == Frontend::BOSS_FAILED) frontend.boss_status[i] = Frontend::BOSS_MISSING;
            break;
        case Frontend::Action::LeaveBossMode:   // the arcade game as it was comes back when the menu closes
            boss.pending = -1;
            boss.to_menu(frontend);
            break;
        case Frontend::Action::QuitToMenu: quit_to_menu(); break;   // from the pause menu of any mode
        case Frontend::Action::Quit: presenter.close(); break;
        case Frontend::Action::Restart:   // RESTART NOW: saved, closed as on QUIT, started again (App::shutdown)
            save_settings();
            g_restart = true;
            presenter.close();
            break;
        case Frontend::Action::DisplayChanged:   // MODE, RESOLUTION: at once
            presenter.set_mode(s.display_mode, s.res_w, s.res_h);
            s.display_mode = presenter.mode();   // (a FULLSCREEN the monitor refused: BORDERLESS)
            save_settings();
            frontend.invalidate();
            break;
        case Frontend::Action::CrosshairChanged: load_crosshairs(renderer); break;
        case Frontend::Action::SettingsChanged: {
            const Options was = g_opt;
            options_from_settings(s, g_opt);
            apply_sound_options(g_opt);
            toast.set_volume(g_opt.volume);
            apply_video_options(&was);
            if (g_opt.sky_procedural != was.sky_procedural || g_opt.classic_text != was.classic_text)
                shown = ~0ull;   // compose the 2D layer again even while the game stands still
            if (g_opt.custom_crosshair != was.custom_crosshair) load_crosshairs(renderer);
            if (g_opt.interp != was.interp) {
                renderer.set_interpolation(1.0f);   // the latest game frame as it is
                renderer.set_back_shift(0.0f, false);
            }
            if (!g_opt.several_mice) raw_mice::reset();
            else raw_mice::set_saved(s.gun_p1, s.gun_p2);
            g_subtitles = g_opt.subtitles;
            g_free_play = g_opt.free_play;
            g_controls.pad_settings = {s.pad_speed, s.gyro, s.gyro_speed, s.vibration, s.trigger_rumble, s.aim_assist};
            save_settings();
            break;
        }
        default: break;
        }
    }
}

// Boss mode (boss_mode.h), once per refresh
void App::update_boss_mode() {
    boss.update(frontend, {.run_unseen = [] { run_unseen(); },
                           .close_menu = [this] { close_menu(); },
                           .menu_over = [] { g_controls.menu_open = g_hold = true; }});
}

// The menus: the window mode, their debugging aids, the records, GUN TEST, the
// music, the pointer and the menu picture
void App::update_menus() {
    if (frontend.settings.display_mode != presenter.mode()) {   // F11 / Alt+Enter
        frontend.settings.display_mode = presenter.mode();
        save_settings();
        frontend.invalidate();
    }
    {   // the window's inside and the desktop, for RESOLUTION; the refresh rate, for MAX FPS
        RECT c{}, m = presenter.monitor_rect();
        GetClientRect(presenter.hwnd(), &c);
        int hz = int(std::lround(display_clock.monitor_hz > 0 ? display_clock.monitor_hz : 1.0 / std::max(display_clock.period, 1e-3)));
        if (c.right != frontend.client_w || c.bottom != frontend.client_h || m.right - m.left != frontend.desktop_w ||
            m.bottom - m.top != frontend.desktop_h || hz != frontend.refresh_hz) {
            frontend.refresh_hz = hz;
            frontend.client_w = c.right, frontend.client_h = c.bottom;
            frontend.desktop_w = m.right - m.left, frontend.desktop_h = m.bottom - m.top;
            frontend.invalidate();
        }
    }
    // Debugging aid: LW_MENU_KEYS (debug_aids.h)
    debug_aids::MenuStep step = debug_aids::menu_keys_step();
    if (step.kind == debug_aids::MenuStep::CLICK) {
        frontend.click(step.x, step.y);
    } else if (step.kind == debug_aids::MenuStep::KEY) {
        if (frontend.active()) menu_key(step.vk);
        else if (step.vk == VK_ESCAPE) game_escape();
    } else if (step.kind == debug_aids::MenuStep::DONE) {
        presenter.capture_next("out/menu.bmp");
        presenter.close();
    }
    static Frontend::Screen menu_was = Frontend::Screen::None;
    if ((frontend.screen() == Frontend::Screen::Records || frontend.screen() == Frontend::Screen::Main) &&
        frontend.screen() != menu_was) {   // the records page, and the main menu's achievements and best score
        frontend.records = stats_view();   // the game stands still while the menu is open
        frontend.invalidate();
    }
    // GUN TEST: what every mouse and gun does, and a press from one of
    // them while the players' guns are being picked
    static std::map<std::string, unsigned> gun_presses;
    if (frontend.gun_test_open()) {
        double now = now_seconds();
        std::vector<raw_mice::Device> devs = raw_mice::devices(presenter.hwnd());
        std::vector<Frontend::GunView> view;
        int hidden = 0;
        for (size_t i = 0; i < devs.size(); i++) {
            const raw_mice::Device& d = devs[i];
            auto known = gun_presses.find(d.path);
            bool fired = menu_was == Frontend::Screen::GunTest && known != gun_presses.end() && d.presses > known->second;
            gun_presses[d.path] = d.presses;
            if (fired && frontend.picking_guns()) {
                frontend.guns = {{d.path, d.name}};
                frontend.gun_fired(0);
            }
            if (!d.last_input) {   // nothing from it yet: the pointer side of a keyboard, say, unless it moves
                hidden++;
                continue;
            }
            Frontend::GunView g{d.path, d.name, d.absolute, d.last_input > 0 && now - d.last_input < 1.0, d.has_pos,
                                int(d.pos.x), int(d.pos.y), d.trigger, d.reload, d.player};
            view.push_back(g);
        }
        auto same = [](const Frontend::GunView& a, const Frontend::GunView& b) {
            return a.path == b.path && a.absolute == b.absolute && a.active == b.active && a.has_pos == b.has_pos &&
                   a.x == b.x && a.y == b.y && a.trigger == b.trigger && a.reload == b.reload && a.player == b.player;
        };
        bool changed = view.size() != frontend.guns.size() || hidden != frontend.quiet_mice;
        for (size_t i = 0; !changed && i < view.size(); i++) changed = !same(view[i], frontend.guns[i]);
        if (changed) {   // every refresh while a mark moves: only the marks are drawn again
            frontend.guns = view;
            frontend.quiet_mice = hidden;
            frontend.invalidate();
        }
    } else {
        gun_presses.clear();
    }
    menu_was = frontend.screen();
    {
        double t = now_seconds();
        music.update(frontend.active() && !frontend.over_game() && !frontend.splash() && frontend.settings.menu_music,
                     frontend.settings.volume, t - music_time);
        music_time = t;
    }
    if (!frontend.active()) pad_in_menu = false;   // a menu opened later starts with the mouse
    presenter.set_cursor_visible(frontend.active() && !pad_in_menu && !frontend.gun_test_open() && !frontend.splash());
    static const bool menu_pad = getenv("LW_MENU_PAD") != nullptr;   // debugging aid: the gamepad's key help
    if (frontend.pad_prompts != (pad_in_menu || menu_pad)) {   // the key help shows the gamepad's buttons or the keys
        frontend.pad_prompts = pad_in_menu || menu_pad;
        frontend.invalidate();
    }
    // Debugging aid: LW_MENU_FILM, a picture every refresh (debug_aids.h)
    if (debug_aids::menu_film_on() && frontend.active()) frontend.invalidate();
    if (frontend.active()) {
        RECT rc;
        GetClientRect(presenter.hwnd(), &rc);
        double t0 = now_seconds();
        if (frontend.draw(rc.right, rc.bottom, ui_pixels)) {
            int y0, y1;
            if (frontend.partial_rows(y0, y1)) ui.update_rows(ui_pixels.data(), rc.right, rc.bottom, y0, y1);
            else ui.set_image(ui_pixels.data(), rc.right, rc.bottom);
            double ms = (now_seconds() - t0) * 1000;
            if (ms > 20) printf("[menu] slow redraw: %.0f ms\n", ms);
            // Debugging aids: LW_MENU_SHOT, LW_MENU_FILM (debug_aids.h)
            if (debug_aids::menu_picture(ui_pixels.data(), rc.right, rc.bottom, [&](int vk) { frontend.key(vk); }))
                presenter.close();
        }
    }
}

// --no-menu: the splash over the held game, then the game
void App::update_intro() {
    presenter.set_cursor_visible(false);
    RECT rc;
    GetClientRect(presenter.hwnd(), &rc);
    if (frontend.draw(rc.right, rc.bottom, ui_pixels)) ui.set_image(ui_pixels.data(), rc.right, rc.bottom);
    if (frontend.splash()) return;
    frontend.open(Frontend::Screen::None);
    ui.hide();
    intro = g_hold = false;
    g_controls.block_buttons = true;   // the press that hurried it on does not shoot
}

// The guns, the keyboard, the gamepads, F12 / F9 and the crosshairs
void App::read_inputs() {
    g_controls.update_aim_reach(presenter.hwnd(), g_opt);
    g_controls.poll_keyboard_mouse(presenter.hwnd(), g_opt);
    bool pad_pause = g_controls.poll_gamepads(presenter.hwnd());
    if (g_controls.pads.changed()) {   // the CONTROLS tab names the pads
        std::wstring names;
        bool gyro = false;
        for (int i = 0; i < Gamepads::MAX; i++) {
            const PadState& pad = g_controls.pads.pad(i);
            if (!pad.connected) continue;
            names += (names.empty() ? L"" : L", ") + std::wstring(pad.name.begin(), pad.name.end());
            gyro |= pad.has_gyro;
        }
        frontend.gamepads = names;
        frontend.gyro_pad = gyro;
        frontend.invalidate();
    }
    for (PadKey k : g_controls.pads.take_keys()) {   // the menus, as keys (START resumes from the pause menu)
        static const int VK[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE, 'Q', 'E', 'R'};
        bool resume = frontend.screen() == Frontend::Screen::Pause;
        if (frontend.active()) {
            menu_key(k == PadKey::Start ? (resume ? VK_ESCAPE : VK_RETURN) : VK[int(k)]);
            pad_in_menu = true;
        }
    }
    if (pad_pause && g_opt.menu && !frontend.active()) {
        game_escape();
        pad_in_menu = true;
    }
    join_update();
    bool f12 = GetForegroundWindow() == presenter.hwnd() && (GetAsyncKeyState(VK_F12) & 0x8000);
    if (f12 && !f12_was_down) prep.capture_2d = true;
    f12_was_down = f12;
    bool f9 = GetForegroundWindow() == presenter.hwnd() && (GetAsyncKeyState(VK_F9) & 0x8000);
    if (f9 && !f9_was_down && built) {   // debug snapshot of the scene on screen
        CreateDirectoryA("out/snap", nullptr);
        std::string base = "out/snap/snap_" + std::to_string(built->frame);
        save_snapshot((base + ".bin").c_str(), *built, layers_shown->vram);
        dump_scene(*scene, layers_shown->back.data(), layers_shown->front.data(), (base + ".txt").c_str());
        presenter.capture_next(base + ".bmp");
        printf("Snapshot saved to %s.bin\n", base.c_str());
    }
    f9_was_down = f9;
    if (g_opt.shot_frame && shown != ~0ull && shown + 1 >= g_opt.shot_frame) prep.capture_2d = true;
    for (int p = 0; p < 2; p++) {   // player 2's only while that gun is in use
        PlayerAim aim = player_aim(p);
        bool used = (p == 0 || g_inputs.p[1].playing) && !g_controls.crosshair_idle(p, aim, g_opt);
        renderer.set_crosshair(p, g_opt.show_crosshair && used && aim.inside && !g_opt.fixed_aim, aim.x, aim.y);
    }
}

// Picks the 3D frame and 2D layers to show, draws them and presents the image;
// false when the loop ends (--shot taken, end of a --replay film)
bool App::present_frame(double loop_from) {
    // The 3D frame to draw. While interpolating, the picture shows the game
    // as it was at `shown_time`: a frame period plus the frames' lateness
    // before this image's moment on screen, so the next frame is always
    // here before it is needed and the motion stays even; a new frame is
    // taken once the picture reaches the one built. Otherwise at once.
    // While interpolating the frames come from the preparation thread
    // (their lateness includes its work), otherwise straight from the game
    bool interpolating = g_opt.interp && g_opt.replay.empty() && !g_opt.shot_frame;
    if (interpolating != prep.prep_on) {   // a frame it made earlier is not one to go back to
        prep.prep_on = interpolating;
        std::lock_guard<std::mutex> lock(prep.ready_mutex);
        prep.ready_frames.clear();
    }
    double display = display_clock.tick(presenter.hwnd());
    // MAX FPS below the refresh rate with vsync: in whole refreshes, the ones
    // in between show the image before again (only the crosshairs move), less
    // work for the GPU (60 at 120 Hz: every other refresh; without vsync the
    // images are not tied to refreshes, wait_for_refresh spaces them)
    int refreshes = 1;   // per image drawn
    if (g_opt.vsync && g_opt.interp && g_opt.max_fps > 0 && g_opt.replay.empty() && !g_opt.shot_frame && film.empty() &&
        !in_background)   // (BACKGROUND FPS paces the images itself)
        refreshes = std::max(1, int(std::lround(1.0 / (g_opt.max_fps * display_clock.period))));
    repeat_count = refreshes > 1 ? (repeat_count + 1) % refreshes : 0;
    repeat_image = repeat_count != 0;
    if (repeat_image) {
        presenter.frame([&](VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {
            renderer.record_repeat(cmd, view, extent);
            if (survival_shown) survival_layer.record(cmd, view, extent);
            ui.record(cmd, view, extent);
            if (toast_shown) toast_layer.record(cmd, view, extent);
        });
        presents++;
        return true;
    }
    std::shared_ptr<const R3DFrameState> latest;
    std::shared_ptr<Prepared> newest;   // the newest frame made ready
    std::deque<std::shared_ptr<Prepared>> waiting;   // those after the one built (made after the switch)
    if (interpolating) {
        std::lock_guard<std::mutex> lock(prep.ready_mutex);
        while (!prep.ready_frames.empty() && built && prep.ready_frames.front()->state->frame <= built->frame)
            prep.ready_frames.pop_front();
        waiting = prep.ready_frames;
    } else {
        std::lock_guard<std::mutex> lock(g_r3d_mutex);
        latest = g_r3d_latest;
    }
    if (!waiting.empty()) newest = waiting.back(), latest = newest->state;
    if (latest && latest != seen) {
        if (seen) next_period = std::clamp(latest->time - seen->time, 1.0 / 240, 0.1);
        // How long after its vblank the frame was ready to take, plus how
        // far ahead of now this image's moment is: from the preparation
        // thread, when it finished; from the game, when this loop saw it
        double late = newest ? newest->ready_at - latest->time + (display - display_clock.last) : display - latest->time;
        // It rises at once with 1.5 ms to spare, so that later frames by a
        // little do not each step the motion back; a frame much later
        // than the rest (a one-off slow 2D composition) only raises it if
        // another follows 0.25-3 s later: alone it costs one hitch, while
        // raising it would add that delay for a long while. After 5 s
        // without rising it eases down, by 1% of its excess per frame
        // (at least 1.7 ms per second): the motion runs up to ~2% faster
        late = std::clamp(late, 0.0, 0.05);
        if (late > lateness) {
            // (the frames right after a slow one are late too: the same hitch)
            bool spike = false;
            if (late > lateness + 0.004) {
                double since = display - lateness_spike;
                if (since > 3) lateness_spike = display;
                spike = since > 3 || since < 0.25;
            }
            if (!spike) {
                lateness = late + 0.0015;
                lateness_rose = display;
            }
        } else if (display - lateness_rose > 5) {
            lateness = std::max(lateness - std::max(0.00003, 0.01 * (lateness - late)), late);
        }
        seen = latest;
    }
    double shown_time = display - lateness - next_period - 0.0005;
    bool take = latest && latest != built && (!interpolating || !built || shown_time >= built->time);
    // The frame to take: the first one waiting that reaches the picture's
    // moment (the newest if none does)
    std::shared_ptr<Prepared> ready;
    if (take && newest) {
        for (auto& w : waiting)
            if (w->state->time >= shown_time) {
                ready = w;
                break;
            }
        if (!ready) ready = newest;
        latest = ready->state;
    }
    // The 2D layers of each vblank are kept in a few copies: while the 3D
    // frames are interpolated, those of the 3D frame drawn are shown (the
    // picture runs behind the game; the preparation thread composes them
    // with it), unless the game stopped drawing 3D; otherwise the newest
    uint64_t newest_2d;
    {
        std::lock_guard<std::mutex> lock(prep.snaps_mutex);
        prep.poll_snapshots();
        newest_2d = prep.snaps[prep.newest_snap].frame;
    }
    // (on a refresh with no new frame, the one built: the 2D layers must
    // not run ahead of it in the meantime)
    const R3DFrameState* game_newest = newest ? newest->state.get() : latest ? latest.get() : built.get();
    bool follow = interpolating && game_newest && newest_2d != ~0ull && newest_2d <= game_newest->vblank + 4;
    double scene_from = now_seconds(), scene_seconds = 0;   // 2D composition and 3D scene build here (CPU)
    const uint64_t shown_before = shown;
    // (shown = ~0: composed again here, as after a change of settings in the pause menu)
    uint64_t compose_up_to = !follow ? (newest_2d != shown ? newest_2d : ~0ull)
                                     : shown == ~0ull && built ? built->vblank : ~0ull;
    if (compose_up_to != ~0ull) prep.take_snapshot(compose_up_to, own_layers);
    if (compose_up_to != ~0ull && own_layers.frame != ~0ull) {
        std::lock_guard<std::mutex> lock(prep.prep_mutex);
        prep.compose_2d(own_layers);
        if (!own_layers.sky_pictures.empty()) renderer.add_sky_pictures(own_layers.sky_pictures);
        if (!own_layers.overlays.empty()) renderer.add_overlays(own_layers.overlays);
        own_layers.sky_pictures.clear();
        own_layers.overlays.clear();
        apply_2d(own_layers);
        layers_prep.reset();
        scene_seconds += now_seconds() - scene_from;
    }
    if (take) {
        if (ready) frame_period = ready->period;   // its vertices move from the frame built before it
        else if (built) frame_period = std::clamp(latest->time - built->time, 1.0 / 240, 0.1);
        built_from = built ? built->vblank : ~0ull;
        scene_from = now_seconds();
        if (ready) {   // made ready by the preparation thread, with the 2D layers of its vblank
            give_pictures(ready->serial);
            built_prep = ready;
            scene = &ready->scene;
            // (a frame number going back a long way is the game restarted from a saved state)
            const Layers2D& l = ready->layers;
            if (follow && l.frame != ~0ull && (shown == ~0ull || l.frame > shown || shown - l.frame > 8)) {
                apply_2d(l);
                layers_prep = ready;
            }
        } else {
            std::lock_guard<std::mutex> lock(prep.prep_mutex);
            prep.builder.build(*latest, own_scene);
            auto images = prep.pack.take_new_images();
            if (!images.empty()) renderer.add_replacements(images);
            built_prep.reset();
            scene = &own_scene;
        }
        scene_seconds += now_seconds() - scene_from;
        g_select_aim = select_dot_on_screen(*scene);
        built = latest;
        // The weather (weather.h): --weather in any game, or the one picked
        // (or rolled) for RANDOM SPAWNS and survival
        int weather = g_opt.weather;
        if (weather == WEATHER_NONE && (random_spawns_playing() || survival_enabled()))
            weather = weather_rolled();
        renderer.set_weather(R3DWeather::preset(weather));
        renderer.set_scene(scene);
        // Its sound (audio/weather_sound.h): the rain we add, and thunder after each strike
        {
            static int strikes_heard = 0;
            g_sound.weather().set_rain(renderer.own_rain());
            if (renderer.strikes() != strikes_heard) {
                strikes_heard = renderer.strikes();
                g_sound.weather().strike(renderer.last_strike());
            }
        }
        if (g_sky_log) {
            std::lock_guard<std::mutex> lock(g_sky_log_mutex);
            fprintf(g_sky_log, "3d %llu", (unsigned long long)latest->vblank);
            for (const R3DViewport& v : scene->viewports)
                if (v.world) {
                    fprintf(g_sky_log, " axes");
                    for (float f : v.world_axes) fprintf(g_sky_log, " %.5f", f);
                    fprintf(g_sky_log, " cam %.3f %.3f %.3f proj %.5f %.5f %.5f %.5f vp %.1f %.1f %.1f %.1f",
                            v.camera[0], v.camera[1], v.camera[2], v.proj[0], v.proj[5], v.proj[8], v.proj[9],
                            v.x, v.y, v.w, v.h);
                    break;
                }
            fprintf(g_sky_log, "\n");
        }
        // Debugging aids: LW_SPOTLOG, LW_FILM (debug_aids.h)
        debug_aids::spot_log(*scene, built->frame, renderer.back_black(), renderer.night_level());
        if (g_opt.replay.empty()) debug_aids::film_record(*built, layers_shown->vram);
    }
    int y0, y1, tex_dirty_rows = 0;
    if (g_texture_ram.take_dirty(tex_rows, y0, y1)) {
        renderer.upload_texture_rows(tex_rows, y0, y1);
        tex_dirty_rows = y1 - y0;
    }

    bool shot = g_opt.shot_frame && shown != ~0ull && shown >= g_opt.shot_frame;   // --shot N: screenshot and exit
    // A replayed scene is captured after a second on screen, once temporal
    // anti-aliasing (DLAA) has settled
    static uint64_t replay_presents = 0;
    if (shot && !g_opt.replay.empty() && replay_presents++ < 60) shot = false;
    // Debugging aid: LW_SHOTS (debug_aids.h)
    std::string shot_path = debug_aids::shot_path(shown);
    if (!shot_path.empty()) presenter.capture_next(shot_path);
    if (shot) {
        presenter.capture_next("out/capture.bmp");
        g_texture_ram.dump("out/texture_ram.bin");
        dump_scene(*scene, layers_shown->back.data(), layers_shown->front.data(), "out/scene.txt");
        if (built && g_opt.replay.empty()) {
            CreateDirectoryA("out/snap", nullptr);
            save_snapshot("out/snap/shot.bin", *built, layers_shown->vram);
        }
    }
    // The game makes ~57 frames per second; in between, the 3D scene is
    // drawn part way from the previous frame to the one built, at shown_time
    // on the game's own evenly spaced timeline
    static const char* fixed_interp = getenv("LW_INTERP");   // debugging aid: fixed position, e.g. 0.5
    if ((interpolating || fixed_interp) && built) {
        double now = now_seconds();
        double t = std::clamp((shown_time - built->time) / frame_period + 1.0, 0.0, 1.0);
        if (fixed_interp) t = std::clamp(atof(fixed_interp), 0.0, 1.0);
        renderer.set_interpolation(float(t));
        // The back layers move with the 3D when their 2D frames are those
        // of the two 3D frames it goes between. The 2D layers of a vblank
        // are what the game wrote in the frame before it: their sky
        // scroll is that of the previous 3D frame's camera, so the sky
        // goes on from it with the camera's turn in this frame, as far
        // as the 3D has gone towards the frame built (set_back_shift,
        // sky_motion.h)
        bool paired = shift_to == built->vblank && shift_from == built_from;
        renderer.set_back_shift(paired ? float(t * double(shift_to - shift_from)) : 0.0f, paired);
        if (g_sky_log) {
            std::lock_guard<std::mutex> lock(g_sky_log_mutex);
            SkyMotion::Row mx = renderer.sky_motion().row(40, 0), my = renderer.sky_motion().row(40, 1);
            fprintf(g_sky_log, "p %llu %llu %llu %llu %.3f %d %d %.3f %.3f %d %.3f %.3f\n",
                    (unsigned long long)built->vblank, (unsigned long long)built_from, (unsigned long long)shift_from,
                    (unsigned long long)shift_to, t, int(paired), int(mx.known), mx.offset, mx.velocity, int(my.known),
                    my.offset, my.velocity);
        }
        if (g_pacing_log) pacing_sample(now, display, built->time - frame_period * (1 - t), *built);
    }
    static int film_presents = 0;
    if (!film.empty() && ++film_presents == 3) {
        char path[64];
        snprintf(path, sizeof(path), "out/film_out/%04zu.bmp", film_pos);
        presenter.capture_next(path);
    }
    {   // Dynamic resolution (DLAA / DLSS): the graphics card's time for this
        // image should stay within the time until the next one (with vsync its
        // refreshes, without it MAX FPS; UNLIMITED: none to keep to, always the
        // full size; nor for --shot, --replay and films: the same picture each time)
        double budget = !g_opt.interp || g_opt.max_fps == 0 ? display_clock.period
                        : g_opt.max_fps > 0                 ? 1.0 / g_opt.max_fps
                                                            : 0;
        if (g_opt.vsync) budget = display_clock.period * refreshes;
        if (in_background) budget = std::max(budget, 1.0 / 30);
        if (!g_opt.replay.empty() || g_opt.shot_frame || !film.empty()) budget = 0;
        renderer.set_gpu_budget(float(budget * 1000.0));
    }
    presenter.frame([&](VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {
        renderer.record(cmd, view, extent);
        if (survival_shown) survival_layer.record(cmd, view, extent);
        ui.record(cmd, view, extent);
        if (toast_shown) toast_layer.record(cmd, view, extent);   // over the menus too: their backdrop is opaque
    });
    if (g_frame_log) {
        const VkPresenter::LastFrame& f = presenter.last_frame();
        double t = interpolating && built ? std::clamp((shown_time - built->time) / frame_period + 1.0, 0.0, 1.0) : 1;
        frame_log("P %llu %.6f %.6f %.6f %.4f %llu %llu %.6f %d %d %.3f %d %.3f %.3f %.3f %.3f %.3f %.6f\n",
                  (unsigned long long)f.present_id, loop_from, display, shown_time, t,
                  (unsigned long long)(built ? built->frame : 0), (unsigned long long)(latest ? latest->frame : 0),
                  latest ? latest->time : 0.0, int(take), int(shown != shown_before), scene_seconds * 1000,
                  tex_dirty_rows, f.fence * 1000, f.acquire * 1000, f.record * 1000, f.present * 1000,
                  (now_seconds() - loop_from) * 1000, lateness);
        if (f.present_id) presented_id = f.present_id;
    }
    if (!film.empty() && film_presents >= 4) {   // next snapshot of the sequence
        film_presents = 0;
        if (++film_pos >= film.size() || !load_snapshot(film[film_pos].c_str())) return false;
    }
    if (shot) {
        printf("Capture saved to out/capture.bmp (frame %llu, %zu polygons, %zu of %zu vertices moving)\n",
               (unsigned long long)shown, scene->polygons, scene->moving, scene->vertices.size());
        return false;
    }
    presents++;
    scene_total += scene_seconds;
    update_title();
    return true;
}

// Once a second: the window title (and every 5 s the log) with where the time goes
void App::update_title() {
    double now = now_seconds();
    if (now - last_title >= 1.0) {
        // Where the time goes, in ms per frame: the game thread (CPU, without
        // its waits), the preparation thread (per game frame), this thread's
        // scene work and command recording, and the time blocked on the GPU
        // and the display (vsync)
        VkPresenter::Timing t = presenter.take_timing();
        uint64_t game_us = g_game_busy_us.exchange(0), game_n = g_game_frames.exchange(0);
        double per = t.frames ? 1000.0 / t.frames : 0;   // seconds summed over the presents -> ms per present
        double game_ms = game_n ? double(game_us) / 1000.0 / double(game_n) : 0;
        double scene_ms = presents ? scene_total * 1000.0 / double(presents) : 0;
        uint64_t prep_total = prep.prep_us.exchange(0), prep_n = prep.prep_frames.exchange(0);
        double prep_ms = prep_n ? double(prep_total) / 1000.0 / double(prep_n) : 0;
        unsigned long long hz = (unsigned long long)(double(presents) / (now - last_title) + 0.5);
        char title[256];
        snprintf(title, sizeof(title),
                 "Lost World Native by snicolast - %llu Hz display - frame %llu - %zu polygons | ms/frame: game %.1f, "
                 "prep %.1f, scene %.1f, render %.1f, GPU %.1f",
                 hz, (unsigned long long)shown,
                 scene->polygons, game_ms, prep_ms, scene_ms, t.record * per, (t.gpu_wait + t.present) * per);
        SetWindowTextA(presenter.hwnd(), title);
        static double last_log = now;
        if (now - last_log >= 5.0) {   // the same, in the log, every 5 s
            last_log = now;
            printf("[timing] frame %llu  %zu polygons  ms/frame: game %.2f  prep %.2f  scene %.2f  render %.2f  "
                   "GPU wait %.2f  present %.2f  (%llu Hz display)\n",
                   (unsigned long long)shown, scene->polygons, game_ms, prep_ms, scene_ms, t.record * per,
                   t.gpu_wait * per, t.present * per, hz);
            fflush(stdout);
        }
        presents = 0;
        scene_total = 0;
        last_title = now;
    }
}

// Orderly exit: park the game thread, stop the audio, then the video
void App::shutdown() {
    stop_threads();
    g_quit = true;
    if (g_opt.replay.empty())
        for (int i = 0; i < 100 && !g_parked; i++) Sleep(10);   // a frame takes ~17 ms; 1 s is plenty
    music.close();
    g_audio.stop();   // before `music` goes: the output thread reads it
    stats_save();
    fflush(stdout);
    ui.shutdown();
    if (survival_shown) survival_layer.shutdown();
    if (toast_shown) toast_layer.shutdown();
    toast.shutdown();
    frontend.shutdown();
    survival_ui.shutdown();
    renderer.shutdown();
    presenter.shutdown();
    g_controls.pads.shutdown();
    if (g_restart) restart_game();
    console_prompt();
    ExitProcess(0);
}

// RESTART NOW: the same exe, command line, folder and environment, straight to
// the main menu (LW_RESTARTED: no logo), without the debugging aids of one run
static void restart_game() {
    SetEnvironmentVariableA("LW_RESTARTED", "1");
    for (const char* aid : {"LW_MENU_KEYS", "LW_MENU_SHOT", "LW_MENU_FILM", "LW_MENU_FILM_KEYS"})
        SetEnvironmentVariableA(aid, nullptr);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{sizeof(si)};
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    bool files = out && out != INVALID_HANDLE_VALUE && GetFileType(out) == FILE_TYPE_DISK;   // (a test's redirected log goes on)
    if (files) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out, si.hStdError = err, si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SetHandleInformation(err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    PROCESS_INFORMATION pi{};
    printf("[menu] restarting\n");
    fflush(stdout);
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, files, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        printf("[menu] could not start the game again (%lu)\n", GetLastError());
    }
}

static std::atomic<bool> g_terminal_prompt{false};   // started from a terminal, nothing redirected: its prompt to bring back

// The game is a windowed program (no console window of its own). Started from
// a terminal, its log goes to that terminal; redirected to a file or a pipe
// (the tests), it goes there as it always did
static void attach_parent_console() {
    auto valid = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    bool out_ok = valid(STD_OUTPUT_HANDLE), err_ok = valid(STD_ERROR_HANDLE);
    if (out_ok && err_ok) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;   // double click: no console at all
    if (!out_ok) freopen("CONOUT$", "w", stdout);
    if (!err_ok) freopen("CONOUT$", "w", stderr);
    g_terminal_prompt = !out_ok && !err_ok;   // (something redirected: a script's run, left as it is)
    atexit(console_prompt);                   // the ways out of main() that return
}

// A terminal does not wait for a windowed program: its prompt came back as the
// game started and the log was written after it, so when the game ended the
// terminal looked busy until Enter was pressed. The game presses it: an Enter
// in the terminal's input, and the terminal draws its prompt again
void console_prompt() {
    if (!g_terminal_prompt.exchange(false)) return;
    fflush(stdout);
    HANDLE in = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                            nullptr);
    if (in == INVALID_HANDLE_VALUE) return;
    INPUT_RECORD key[2]{};   // down and up
    for (int i = 0; i < 2; i++) {
        key[i].EventType = KEY_EVENT;
        KEY_EVENT_RECORD& k = key[i].Event.KeyEvent;
        k.bKeyDown = i == 0;
        k.wRepeatCount = 1;
        k.wVirtualKeyCode = VK_RETURN;
        k.wVirtualScanCode = WORD(MapVirtualKeyA(VK_RETURN, MAPVK_VK_TO_VSC));
        k.uChar.AsciiChar = '\r';
    }
    DWORD written = 0;
    WriteConsoleInputA(in, key, 2, &written);
    CloseHandle(in);
}

int main(int argc, char** argv) {
    attach_parent_console();
    setvbuf(stdout, nullptr, _IONBF, 0);   // keep logs even if the process dies
    // The menus (and the settings they save) are left out of test runs, so
    // those always start from the same defaults
    g_opt.menu = !options_disable_menu(argc, argv);
    const bool menu_wanted = g_opt.menu;   // not a test run (--no-menu may still turn the menu off)
    FrontendSettings ini, other_ini;   // the experience to run, the other one (EXPERIENCE)
    bool arcade_section = false;
    // The first start (no anti-aliasing saved yet) picks it for the graphics
    // card and the screen (nvidia_card), not over --aa or --msaa
    bool aa_auto = false;
    if (g_opt.menu) {   // save/settings.ini, written by the options menu; command-line options win
        ini.load("save/settings.ini");
        // ARCADE ORIGINAL's own settings: its section, over the arcade's defaults
        FrontendSettings arcade_ini = ini.arcade_defaults();
        arcade_section = arcade_ini.load("save/settings.ini", "arcade");
        if (ini.experience == 1) {
            other_ini = ini;
            ini = arcade_ini;
            g_arcade = true;
            printf("[experience] ARCADE ORIGINAL\n");
        } else other_ini = arcade_ini;
        bool aa_given = false;
        for (int i = 1; i < argc; i++)
            if (strcmp(argv[i], "--aa") == 0 || strcmp(argv[i], "--msaa") == 0) aa_given = true;
        aa_auto = !ini.aa_saved && !aa_given && nvidia_card();
        if (aa_auto) ini.aa = 1;   // DLAA; DLSS QUALITY once the screen is known (App::init)
        else if (!ini.aa_saved && !aa_given) ini.aa = 7;   // no DLSS on other cards: FXAA, cheap (MSAA costs old cards a lot)
        if (g_arcade && !other_ini.aa_saved)   // PC EDITION's first start comes in ARCADE ORIGINAL: its pick all the same
            other_ini.aa = nvidia_card() ? 1 : 7;
        options_from_settings(ini, g_opt);
        g_controls.pad_settings = {ini.pad_speed, ini.gyro, ini.gyro_speed, ini.vibration, ini.trigger_rumble, ini.aim_assist};
    }
    // The survival mode's options live in survival.cpp: the table hands them over
    auto extra = [](Options&, const std::string& a, const char* next) -> int {
        if (random_spawns_option(a)) return 1;
        if (!survival_option(a, next)) return 0;
        return a == "--survival" ? 2 : 1;
    };
    int exit_code = 0;
    if (!parse_options(g_opt, argc, argv, extra, exit_code)) return exit_code;
    if (const char* w = weather_name(g_opt.weather)) printf("[weather] %s in every game (--weather)\n", w);
    weather_force(g_opt.weather);
    // What the options set outside the struct: emulation globals and the gun
    g_data_dir = g_opt.data_dir;
    g_frame_limit = g_opt.frame_limit;
    g_boss_maker = g_opt.boss_maker;
    g_subtitles = g_opt.subtitles;
    g_free_play = g_opt.free_play;
    for (const std::string& script : g_opt.auto_scripts) g_auto.parse(script);
    g_fixed_aim = g_opt.fixed_aim;
    if (g_opt.fixed_aim) {   // fixed raw gun ADC values (calibration tests)
        g_inputs.p[0].gun_x = g_opt.aim_x;
        g_inputs.p[0].gun_y = g_opt.aim_y;
    }
    if (!g_opt.diff.empty()) {   // FN@FRAME
        const std::string& d = g_opt.diff;
        g_diff_fn = strtoul(d.c_str(), nullptr, 16);
        size_t at = d.find('@');
        if (at != std::string::npos) g_diff_frame = strtoull(d.c_str() + at + 1, nullptr, 10);
        g_trace_hits[g_diff_fn] = 0;
        rt_hook_function(g_diff_fn);
    }
    for (const std::string& list : g_opt.traces) {   // comma-separated: --trace 10F650,10D564
        size_t p = 0;
        while (p < list.size()) {
            uint32_t fn = strtoul(list.c_str() + p, nullptr, 16);
            g_trace_hits[fn] = 0;
            rt_hook_function(fn);
            p = list.find(',', p);
            if (p == std::string::npos) break;
            p++;
        }
    }
    if (g_boss_maker >= 0) {   // its log next to the state; the settings of the EEPROM stay as they are
        g_terminal_prompt = false;   // started by the game, which shares its terminal: the game brings the prompt back
        CreateDirectoryA("save", nullptr);
        CreateDirectoryA("save/boss", nullptr);
        freopen(("save/boss/boss" + std::to_string(g_boss_maker + 1) + ".log").c_str(), "w", stdout);
        // Without save/eeprom.bin the game's first boot writes its settings
        // and then starts without the course select, so the boss's stage
        // could not be picked: the game makes that file at its first launch
        if (GetFileAttributesA("save/eeprom.bin") == INVALID_FILE_ATTRIBUTES) {
            printf("[boss] save/eeprom.bin is missing: open the game once first\n");
            return 1;
        }
        g_fast_idle = true;
    } else if (g_opt.headless && g_frame_limit == INT32_MAX) {
        g_frame_limit = 600;
    }
    g_collect_stats = g_frame_limit != INT32_MAX;   // report() prints them when the run ends
    printf("Lost World Native (%s)\n", g_opt.headless ? "no window" : "Vulkan window, 3D + 2D");
    {
        std::vector<uint8_t> program, samples;
        if (!load_game_rom(g_opt, program, samples, g_rom_zip)) return 1;
        if (!getenv("LW_NOSOUND"))
            g_sound.init(std::move(program), std::move(samples), g_opt.headless ? nullptr : &g_audio);
        if (g_boss_maker >= 0) g_sound.set_dsp_enabled(false);   // the effects only matter when heard
        if (!g_opt.wav_path.empty()) g_sound.dump_wav(g_opt.wav_path.c_str());
        g_sound.weather().load("res/sound");
        apply_sound_options(g_opt);
    }
    g_ram = static_cast<uint8_t*>(calloc(RAM_SIZE + 8, 1));
    CreateDirectoryA("save", nullptr);
    CreateDirectoryA("out", nullptr);   // dumps, screenshots, out/missing.txt
    g_eeprom.load("save/eeprom.bin", g_boss_maker >= 0);

    // Survival mode: --survival STAGE:ZONE, or picked in the menu
    survival_hooks();
    random_spawns_hooks();
    int cli_stage = 0, cli_zone = 0;
    bool survival_cli = survival_cli_request(cli_stage, cli_zone);

    // The game code uses the original CPU's stack as memory and nests
    // many calls: run it on a thread with 512 MB of native stack.
    if (g_opt.headless) {
        if (survival_cli) {   // no menu to come back to: straight from the boot
            survival_begin(cli_stage, cli_zone);
            survival_ungate();
        }
        HANDLE t = CreateThread(nullptr, 512u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        WaitForSingleObject(t, INFINITE);
        return 0;
    }

    // Window (App): the game on its own thread, this one draws
    App app;
    app.ini = ini;
    app.other_ini = other_ini;
    app.arcade_section = arcade_section;
    app.survival_cli = survival_cli;
    app.intro = menu_wanted && !g_opt.menu;
    app.cli_stage = cli_stage, app.cli_zone = cli_zone;
    if (!app.init(aa_auto)) return 1;
    app.run();
    app.shutdown();
}
