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

#include "bosses.h"
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
#include "survival.h"
#include "weather.h"
#include "../audio/sound_board.h"
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

// The stage select screen marks the gun with a small textured dot in its top
// viewport; in a stage the game draws an untextured X instead
static bool select_dot_on_screen(const R3DScene& scene) {
    if (scene.viewports.empty()) return false;
    const R3DViewport& v = scene.viewports.back();   // highest priority
    for (uint32_t i = 0; i < v.trans_count; i++)
        if (scene.vertices[v.trans_first + i].tex == 0x625A0340) return true;
    return false;
}
// Debugging aid for --shot: every triangle with its screen bounding box
// (native pixels), plus the 2D layers' alpha along two columns
static void dump_scene(const R3DScene& scene, const uint32_t* back, const uint32_t* front, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (size_t vi = 0; vi < scene.viewports.size(); vi++) {
        const R3DViewport& v = scene.viewports[vi];
        fprintf(f, "viewport %zu prio %d rect %.0f %.0f %.0f %.0f opaque %u trans %u\n", vi, v.priority, v.x, v.y,
                v.w, v.h, v.opaque_count / 3, v.trans_count / 3);
        fprintf(f, "  fog density %.5f start %.3f colour %.3f %.3f %.3f ambient %.3f scroll %.3f far %.1f world %d\n",
                v.fog_density, v.fog_start, v.fog_color[0], v.fog_color[1], v.fog_color[2], v.fog_ambient,
                v.scroll_fog, v.scenery_far, int(v.world));
        for (int pass = 0; pass < 2; pass++) {
            uint32_t first = pass ? v.trans_first : v.opaque_first, count = pass ? v.trans_count : v.opaque_count;
            for (uint32_t t = 0; t + 2 < count; t += 3) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, zmin = 1e9f;
                const R3DVertex* tri = &scene.vertices[first + t];
                for (int k = 0; k < 3; k++) {
                    const float* p = tri[k].pos;
                    float cx = v.proj[0] * p[0] + v.proj[4] * p[1] + v.proj[8] * p[2] + v.proj[12];
                    float cy = v.proj[1] * p[0] + v.proj[5] * p[1] + v.proj[9] * p[2] + v.proj[13];
                    float cw = v.proj[3] * p[0] + v.proj[7] * p[1] + v.proj[11] * p[2] + v.proj[15];
                    if (cw <= 1e-6f) cw = 1e-6f;
                    float sx = v.x + (cx / cw + 1) / 2 * v.w, sy = v.y + (cy / cw + 1) / 2 * v.h;
                    x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
                    zmin = std::min(zmin, -p[2]);
                }
                fprintf(f, "  tri v%zu %s bbox %.1f %.1f %.1f %.1f z %.3f color %08x flags %x tex %08x\n", vi,
                        pass ? "T" : "O", x0, y0, x1, y1, zmin, tri[0].color, tri[0].flags, tri[0].tex);
            }
        }
    }
    for (int y = 0; y < SCREEN_H; y += 4)
        fprintf(f, "2d row %d: back[0] %08x back[248] %08x front[0] %08x front[248] %08x\n", y,
                back[y * BACK_W + BACK_MARGIN], back[y * BACK_W + BACK_MARGIN + 248], front[y * SCREEN_W],
                front[y * SCREEN_W + 248]);
    fclose(f);
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

// Keyboard and mouse (only while the window is active):
//   5 = coin, 1 = start, mouse = gun (left button fires, right reloads),
//   6 / 2 = player 2's coin and start, F2 = test, F3 = service.
// With free play, 1 / 2 insert the credit themselves (player_join)
static bool g_menu_open = false;     // a menu has the controls
static bool g_block_buttons = false; // after a menu closes: buttons ignored until released
static int g_p1_pad = -1;            // the gamepad aiming for player 1; -1: the mouse (whichever moved last)
static bool g_p2_pad = false;        // a gamepad is player 2's (poll_gamepads)

// Window pixels -> fraction of the 4:3 game screen (accounting for the black
// bars; beyond 0..1 in the side bands of a wider 3D view)
static void aim_at(int player, HWND hwnd, POINT pt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    ScreenLayout lay = compute_layout(float(rc.right), float(rc.bottom), g_opt.max_aspect);
    float x = (pt.x - lay.ox - lay.game_x0 * lay.scale) / (SCREEN_W * lay.scale);
    float y = (pt.y - lay.oy) / (SCREEN_H * lay.scale);
    set_player_aim(player, x, y);
}

// The gun covers the whole 3D view, also the side bands of a wide one: how
// far it reaches beyond the 4:3 screen (each new picture, as the window may
// change)
static void update_aim_reach(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    ScreenLayout lay = compute_layout(float(rc.right), float(rc.bottom), g_opt.max_aspect);
    g_aim_reach = lay.game_x0 / SCREEN_W;
}

// A shot outside the 3D view (the black bars of a window wider than it) or on
// its very edge (where a light gun puts a shot that misses the screen)
// reloads, as on the cabinet, instead of firing at the edge
static void shot_outside_reloads(int player) {
    PlayerInputs& pl = g_inputs.p[player];
    PlayerAim a = player_aim(player);   // the very edge too: where a gun can park an off-screen shot
    float reach = gun_reach();
    if (pl.trigger && (a.x <= -reach || a.y <= 0 || a.x >= 1 + reach || a.y >= 1)) {
        pl.trigger = false;
        pl.offscreen = true;
    }
}

static void poll_keyboard_mouse(HWND hwnd) {
    // Scripted input (--auto) takes over the controls
    if (GetForegroundWindow() != hwnd || !g_auto.events.empty() || survival_owns_inputs()) {
        if (g_auto.events.empty() && !survival_owns_inputs())   // in the background: no gun stays held
            for (PlayerInputs& pl : g_inputs.p) pl.trigger = pl.offscreen = false;
        raw_mice::update(hwnd, false, false);
        return;
    }
    // A second mouse becomes player 2's when it clicks during a game, also
    // after player 2 joined with start (a light gun's button) before firing,
    // unless a gamepad is player 2's
    const PlayerInputs &in1 = g_inputs.p[0], &in2 = g_inputs.p[1];
    bool join_allowed = g_opt.several_mice && !g_menu_open && (in1.playing || in2.playing) && !(in2.playing && g_p2_pad);
    raw_mice::update(hwnd, join_allowed, !g_menu_open);
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (g_menu_open) {
        g_inputs.test = g_inputs.service = false;
        for (PlayerInputs& pl : g_inputs.p) pl.coin = pl.start = pl.trigger = pl.offscreen = false;
        return;
    }
    PlayerInputs& p1 = g_inputs.p[0];
    p1.coin = down('5');
    g_inputs.p[1].coin = down('6');
    static bool start_was[2] = {};
    for (int p = 0; p < 2; p++) {
        bool start = down(p ? '2' : '1');
        if (!g_opt.free_play) g_inputs.p[p].start = start;
        else if (start && !start_was[p] && !g_inputs.p[p].playing) player_join(p);   // also continues
        start_was[p] = start;
    }
    g_inputs.test = down(VK_F2);
    g_inputs.service = down(VK_F3);
    POINT cursor;
    GetCursorPos(&cursor);
    static POINT last_cursor{-100000, -100000};
    if (cursor.x != last_cursor.x || cursor.y != last_cursor.y) g_p1_pad = -1;   // the mouse moved: it aims again
    last_cursor = cursor;
    if (raw_mice::two_mice()) {   // each mouse its own gun; player 2's click joins while player 1 plays
        for (int p = 0; p < 2; p++) {
            raw_mice::Gun g = raw_mice::gun(p, hwnd);
            PlayerInputs& pl = g_inputs.p[p];
            if (p == 1 && g.clicked && !pl.playing && p1.playing) player_join(1);
            pl.trigger = g.trigger;
            pl.offscreen = g.reload;
            if (p == 1 || g_p1_pad < 0) {
                aim_at(p, hwnd, g.pos);
                shot_outside_reloads(p);
            }
        }
    } else {
        p1.trigger = down(VK_LBUTTON);
        p1.offscreen = down(VK_RBUTTON);
        g_inputs.p[1].trigger = g_inputs.p[1].offscreen = false;   // player 2's pad adds its buttons after this
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (g_p1_pad < 0) {
            aim_at(0, hwnd, pt);
            shot_outside_reloads(0);
        }
    }
    if (g_block_buttons) {   // the click that closed the menu does not fire
        bool held = false;
        for (PlayerInputs& pl : g_inputs.p) held |= pl.trigger || pl.offscreen;
        if (held) for (PlayerInputs& pl : g_inputs.p) pl.trigger = pl.offscreen = false;
        else g_block_buttons = false;
    }
    static bool c_was_down = false;   // C toggles the extra crosshair
    bool c_down = down('C');
    if (c_down && !c_was_down) g_opt.show_crosshair = !g_opt.show_crosshair;
    c_was_down = c_down;
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

// Settings > Controls > Hide idle crosshair: a gun that has not moved (more
// than a hand's tremble, 1% of the screen) nor fired for that many seconds
// loses its crosshair until it does. A player who joined by mistake (a light
// gun's start button) leaves no crosshair standing still on the screen
static bool crosshair_idle(int p, const PlayerAim& aim) {
    static float at_x[2] = {-9, -9}, at_y[2] = {-9, -9};
    static ULONGLONG used_at[2] = {};
    ULONGLONG now = GetTickCount64();
    const PlayerInputs& pl = g_inputs.p[p];
    if (pl.trigger || pl.offscreen || std::fabs(aim.x - at_x[p]) > 0.01f || std::fabs(aim.y - at_y[p]) > 0.01f) {
        at_x[p] = aim.x, at_y[p] = aim.y;
        used_at[p] = now;
    }
    return g_opt.crosshair_hide > 0 && now - used_at[p] > ULONGLONG(g_opt.crosshair_hide) * 1000;
}

// Gamepads (gamepad.h), after the keyboard and mouse: their buttons are added
// to those of the player pad_player() gives them. The first pad shares player
// 1 with the mouse, and whichever moved last aims. START joins the game, takes
// player 1's sight from the mouse or, pressed by the pad that plays, pauses
// it; BACK pauses. Returns true when a pad asks for the pause menu.
static Gamepads g_pads;
static GamepadSettings g_pad_settings;

// The aim assist's targets: a box around each enemy the game placed on
// screen, from the point it stands on up the body, sized with its distance.
// In world units (LW_AIM_BOX=up,half_width,half_height to try others)
static std::vector<AimTarget> aim_targets() {
    static float up = 7, half_w = 6, half_h = 5;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("LW_AIM_BOX")) sscanf(e, "%f,%f,%f", &up, &half_w, &half_h);
    }
    std::vector<AimTarget> out;
    for (const EnemyMark& m : enemy_marks()) {
        float cy = m.y - up * m.scale, w = half_w * m.scale, h = half_h * m.scale;
        out.push_back({(m.x - w) / SCREEN_W, (cy - h) / SCREEN_H, (m.x + w) / SCREEN_W, (cy + h) / SCREEN_H});
    }
    return out;
}

static bool poll_gamepads(HWND hwnd) {
    bool focused = GetForegroundWindow() == hwnd;
    g_pads.set_targets(aim_targets());
    g_pads.poll(g_pad_settings, focused, g_menu_open);
    static bool blocked[Gamepads::MAX] = {}, fire_was[Gamepads::MAX] = {}, reload_was[Gamepads::MAX] = {};
    static uint32_t lives_was[2] = {};
    uint32_t connected = 0;
    for (int i = 0; i < Gamepads::MAX; i++)
        if (g_pads.pad(i).connected) connected |= 1u << i;
    if (g_p1_pad >= 0 && !(connected >> g_p1_pad & 1)) g_p1_pad = -1;   // unplugged: the mouse aims again
    g_p2_pad = false;
    for (int i = 0; i < Gamepads::MAX; i++)
        if (connected >> i & 1 && pad_player(i, connected) == 1) g_p2_pad = true;
    if (g_menu_open) std::fill(std::begin(blocked), std::end(blocked), true);   // the button that closes it does not fire
    if (!focused || g_menu_open || !g_auto.events.empty() || survival_owns_inputs()) return false;
    bool pause = false;
    for (int i = 0; i < Gamepads::MAX; i++) {
        const PadState& pad = g_pads.pad(i);
        if (!pad.connected) continue;
        int p = pad_player(i, connected);
        PlayerInputs& pl = g_inputs.p[p];
        if (pad.aimed && p == 0) g_p1_pad = i;
        if (p == 1 || g_p1_pad == i) set_player_aim(p, pad.aim_x, pad.aim_y);
        else {   // the mouse aims: the pad's sight waits where the mouse is
            PlayerAim a = player_aim(0);
            g_pads.set_aim(i, a.x, a.y);
        }
        bool fire = pad.fire, reload = pad.reload;
        if (blocked[i]) {
            if (fire || reload) fire = reload = false;
            else blocked[i] = false;
        }
        if (fire) pl.trigger = true;
        if (reload) pl.offscreen = true;
        if (fire && !fire_was[i]) {
            g_pads.rumble(i, 0.2f, 0.5f, 70);
            g_pads.rumble_triggers(i, 0.0f, 0.6f, 60);
        } else if (reload && !reload_was[i]) {
            g_pads.rumble(i, 0.35f, 0.0f, 50);
        }
        fire_was[i] = fire, reload_was[i] = reload;
        if (pad.back) pause = true;
        if (pad.start) {
            if (pl.playing && (p == 1 || g_p1_pad == i)) pause = true;
            else if (pl.playing) g_p1_pad = i;   // player 1 plays with the mouse (a pad plugged in mid-game)
            else {
                if (p == 0) g_p1_pad = i;   // this pad starts player 1's game
                player_join(p);
            }
        }
    }
    // A hit takes a life (game_ram.md): the pads of that player that were used
    // in the last minute rumble
    for (int p = 0; p < 2; p++) {
        uint32_t lives = rd32(p ? 0x1A377C : 0x1A3720);
        if (g_inputs.p[p].playing && lives < lives_was[p])
            for (int i = 0; i < Gamepads::MAX; i++)
                if (g_pads.pad(i).connected && pad_player(i, connected) == p && g_pads.now() - g_pads.pad(i).last_used < 60)
                    g_pads.rumble(i, 0.9f, 0.7f, 320);
        lives_was[p] = lives;
    }
    return pause;
}

// Debug snapshot (F9): the Real3D memory of the frame on screen, texture RAM
// and 2D layer memory in one file, to look at a scene again with --replay
static const uint32_t SNAPSHOT_MAGIC = 0x3153574C;   // "LWS1"

static void save_snapshot(const char* path, const R3DFrameState& state, const std::vector<uint8_t>& vram) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t count = uint32_t(state.pages.size());
    fwrite(&SNAPSHOT_MAGIC, 4, 1, f);
    fwrite(&state.frame, 8, 1, f);
    fwrite(&count, 4, 1, f);
    for (auto& [key, page] : state.pages) {
        fwrite(&key, 4, 1, f);
        fwrite(page->data(), 1, page->size(), f);
    }
    std::vector<uint16_t> texels;
    g_texture_ram.copy_all(texels);
    fwrite(texels.data(), 2, texels.size(), f);
    fwrite(vram.data(), 1, vram.size(), f);
    fclose(f);
}

static bool load_snapshot(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint32_t magic = 0, count = 0;
    auto state = std::make_shared<R3DFrameState>();
    bool ok = fread(&magic, 4, 1, f) == 1 && magic == SNAPSHOT_MAGIC && fread(&state->frame, 8, 1, f) == 1 &&
              fread(&count, 4, 1, f) == 1;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint32_t key;
        auto page = std::make_shared<R3DPage>();
        ok = fread(&key, 4, 1, f) == 1 && fread(page->data(), 1, page->size(), f) == page->size();
        state->pages[key] = page;
    }
    std::vector<uint16_t> texels(2048 * 2048);
    ok = ok && fread(texels.data(), 2, texels.size(), f) == texels.size();
    std::vector<uint8_t> vram(TILEGEN_SIZE);
    ok = ok && fread(vram.data(), 1, vram.size(), f) == vram.size();
    fclose(f);
    if (!ok) return false;
    g_texture_ram.load_all(texels);
    {
        std::lock_guard<std::mutex> lock(g_video_mutex);
        g_video_vram = vram;
        g_video_frame = state->frame;
        for (uint64_t& serial : g_video_page_serial) serial++;
    }
    std::lock_guard<std::mutex> lock(g_r3d_mutex);
    g_r3d_latest = std::move(state);
    return true;
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
// LW_SKYLOG=path: the sky's scroll and the world camera, frame by frame (tools/sky_log.py)
static FILE* g_sky_log = getenv("LW_SKYLOG") ? fopen(getenv("LW_SKYLOG"), "w") : nullptr;
static std::mutex g_sky_log_mutex;   // 2D frames are logged on the preparation thread

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
    static double prev_period = 1 / 57.524, game_period = 1 / 57.524;
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

// The folder of LostWorld.exe, with its trailing backslash
static bool file_exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string full_path(const std::string& path) {
    char full[MAX_PATH];
    DWORD n = GetFullPathNameA(path.c_str(), MAX_PATH, full, nullptr);
    return n && n < MAX_PATH ? std::string(full, n) : path;
}

// The zip the ROM came from (empty when it came from a data folder): the boss
// state makers are started on the same ROM
// QUALITY for the first start, from the graphics card's name: a rough guess
// at its speed (NVIDIA RTX and AMD RX by series and model, a laptop's a step
// lower; MEDIUM when the name says nothing). The options menu changes it.
static int suggest_quality(const VkPresenter& vk) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.gpu(), &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) return FrontendSettings::LOW;
    std::string name = props.deviceName;
    for (char& c : name) c = char(tolower((unsigned char)c));
    auto number_after = [&](const char* word) {
        size_t at = name.find(word);
        return at == std::string::npos ? 0 : atoi(name.c_str() + at + strlen(word));
    };
    bool laptop = name.find("laptop") != std::string::npos || name.find("mobile") != std::string::npos ||
                  name.find("max-q") != std::string::npos;
    // Ti, Super, XT: half a model up. Scores: 60 a desktop RTX 2060, +10 a model, +10 a series
    bool plus = name.find(" ti") != std::string::npos || name.find("super") != std::string::npos ||
                name.find(" xt") != std::string::npos;
    int score = 0, cap = FrontendSettings::ULTRA;
    if (int n = number_after("rtx "); n >= 2000) {   // 2060, 3070, 4080, 5090...
        score = n % 100 + (n / 1000 - 2) * 10;
    } else if (int r = number_after("rx "); r >= 5000 && r < 10000) {   // 6600, 7800 XT, 9070 XT...
        int series = r / 1000, model = (r / 100) % 10 ? (r / 100) % 10 : (r / 10) % 10;
        score = model * 10 + (series >= 9 ? 20 : (series - 6) * 10);
        cap = FrontendSettings::HIGH;   // slower ray tracing than NVIDIA's
    } else if (name.find("gtx") != std::string::npos || name.find("radeon") != std::string::npos) {
        return FrontendSettings::LOW;   // older cards without ray tracing (or an unknown Radeon)
    } else {
        return FrontendSettings::MEDIUM;
    }
    score += (plus ? 5 : 0) - (laptop ? 10 : 0);
    int q = score >= 100 ? FrontendSettings::ULTRA : score >= 85 ? FrontendSettings::HIGH
          : score >= 60 ? FrontendSettings::MEDIUM : FrontendSettings::LOW;
    return std::min(q, cap);
}

static std::string g_rom_zip;

// Loads the ROM from the first of: --rom or LW_ROM_ZIP (lostwsga.zip read
// directly), --data, data\ in the current folder or next to the exe (what
// build.bat extracts), lostwsga.zip next to the exe or in the current folder.
// If none is there, says where it looked and how to give the path
static bool load_game_rom(std::vector<uint8_t>& sound_program, std::vector<uint8_t>& sound_samples) {
    std::string zip = g_opt.rom_zip;
    if (zip.empty() && getenv("LW_ROM_ZIP")) zip = getenv("LW_ROM_ZIP");
    std::vector<std::string> looked;   // full paths, for the error
    auto missing = [&](const std::string& path) {
        std::string f = full_path(path);
        if (std::find(looked.begin(), looked.end(), f) == looked.end()) looked.push_back(f);
    };
    if (zip.empty()) {
        std::vector<std::string> dirs = {g_opt.data_dir};
        if (g_opt.data_dir == "data") dirs.push_back(exe_folder() + "data");   // --data not given
        for (const std::string& d : dirs) {
            if (file_exists(d + "/crom.bin")) {
                g_data_dir = d;
                if (!load_rom("crom.bin", g_crom, true)) return false;
                load_rom("banked_crom.bin", g_banked, false);
                load_rom("vrom.bin", g_vrom, false);
                load_rom("sound_program.bin", sound_program, false);
                load_rom("sound_samples.bin", sound_samples, false);
                printf("ROM: %s\n", d.c_str());
                return true;
            }
            missing(d + "\\crom.bin");
        }
        if (g_opt.data_dir == "data")
            for (const std::string& z : {exe_folder() + "lostwsga.zip", std::string("lostwsga.zip")}) {
                if (file_exists(z)) {
                    zip = z;
                    break;
                }
                missing(z);
            }
    }
    std::string error;
    if (!zip.empty()) {
        RomSet rom;
        if (load_rom_zip(zip, rom, error)) {
            g_crom = std::move(rom.crom);
            g_banked = std::move(rom.banked_crom);
            g_vrom = std::move(rom.vrom);
            sound_program = std::move(rom.sound_program);
            sound_samples = std::move(rom.sound_samples);
            g_rom_zip = zip;
            printf("ROM: %s\n", zip.c_str());
            return true;
        }
        error = "Cannot load the ROM: " + error + ".\n";
    } else {
        error = "Cannot find The Lost World ROM (lostwsga.zip). Looked in:\n";
        for (const std::string& l : looked) error += "  " + l + "\n";
    }
    error += "\nPut lostwsga.zip next to LostWorld.exe, or give its path with\n"
             "  LostWorld.exe --rom D:\\roms\\lostwsga.zip\n"
             "or in the LW_ROM_ZIP environment variable.";
    fprintf(stderr, "%s\n", error.c_str());
    if (!g_opt.headless) MessageBoxA(nullptr, error.c_str(), "Lost World Native", MB_OK | MB_ICONERROR);
    return false;
}

// The 2D layers of one vblank as the renderer takes them (App::compose_2d)
struct Layers2D {
    uint64_t frame = ~0ull;   // the vblank
    std::vector<uint8_t> vram = std::vector<uint8_t>(TILEGEN_SIZE);   // its layer memory
    std::vector<uint32_t> back = std::vector<uint32_t>(BACK_W * SCREEN_H),
                          front = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          text_cells = std::vector<uint32_t>(TextHD::ROW_TEXELS * TextHD::ROWS),
                          sky_coords = std::vector<uint32_t>(TileGen::SKY_COORDS_W * TileGen::SKY_COORDS_H);
    std::vector<RgbaImage> overlays, sky_pictures;   // new pictures for the GPU slots
    bool hud_band = false;
    float overlay_brightness = 1;
    std::vector<int32_t> back_rows = std::vector<int32_t>(SCREEN_H * 3);   // layer and scroll per row (TileGen)
    uint64_t shift_from = ~0ull, shift_to = ~0ull;   // this 2D frame and the one composed before it
};
// A game frame made ready for the screen ahead of time: its 3D scene and
// the 2D layers of its vblank (App::prepare_frames)
struct Prepared {
    std::shared_ptr<const R3DFrameState> state;
    R3DScene scene;
    Layers2D layers;
    uint64_t serial = 0;
    double ready_at = 0;   // when it was ready
    double period = 1.0 / 57.524;   // since the frame built before it (where its vertices come from)
};
// The 2D layers of a vblank as the game left them (App::snaps)
struct Snapshot2D {
    uint64_t frame = ~0ull;
    std::vector<uint8_t> vram = std::vector<uint8_t>(TILEGEN_SIZE);
    std::array<uint64_t, TILEGEN_PAGES> serial;   // g_video_page_serial of each page copied
    Snapshot2D() { serial.fill(~0ull); }
};
// New pictures for the GPU slots of a prepared frame (App::ready_pictures)
struct NewPictures {
    uint64_t serial = 0;
    std::vector<RgbaImage> replacements, overlays, sky;
};

// The window: the game runs on its own thread at real speed; this thread draws
// the latest image at the monitor's refresh rate (vsync). main() hands over
// to App::init, App::run and App::shutdown; what was its state is here
struct App {
    FrontendSettings ini;   // save/settings.ini (main)
    bool survival_cli = false;   // --survival STAGE:ZONE
    int cli_stage = 0, cli_zone = 0;

    VkPresenter presenter;
    // --replay on a folder (recorded with LW_FILM): its snapshots one after
    // another, each captured to out/film_out, so effects that depend on the
    // camera motion can be compared frame by frame
    std::vector<std::string> film;
    size_t film_pos = 0;
    R3DRenderer renderer;
    R3DSceneBuilder builder;
    TexturePack pack;
    // 2D replacements: the foreground (GPU slots 1-27) and the layers below the
    // 3D image (28-55; the backdrops of SkyPack use 56-63)
    Pack2D pack2d, pack2d_back;
    SkyPack sky;   // sky and other backdrops below the 3D image
    // Interpolation of the back layers (the sky, renderer's SkyMotion): the
    // layer and scroll of each row of the 2D frame composed, and the vblanks
    // of the last two 2D frames composed (the sky goes on with the camera
    // while the 3D goes between the 3D frames of those same vblanks)
    std::vector<int32_t> back_rows = std::vector<int32_t>(SCREEN_H * 3);
    uint64_t back_rows_frame = ~0ull, shift_from = ~0ull, shift_to = ~0ull, built_from = ~0ull;
    TextHD text;   // menus, captions and score drawn from vector fonts
    std::atomic<bool> capture_2d{false};   // F12 / --shot: the next 2D composition is saved
    bool f12_was_down = false, f9_was_down = false;
    R3DScene own_scene;   // built on this thread (no interpolation)
    const R3DScene* scene = &own_scene;
    Layers2D own_layers;   // composed on this thread (no 3D, or no interpolation)
    const Layers2D* layers_shown = &own_layers;
    std::shared_ptr<Prepared> built_prep, layers_prep;   // the prepared frames in use (scene, 2D layers)
    std::shared_ptr<const R3DFrameState> built;
    double frame_period = 1.0 / 57.524;   // between the frame built and the one before
    // Interpolation timing: the last frame the game published, the time
    // between its last two frames and how late after its vblank a frame gets
    // here (the game's CPU time plus this loop's wait for it; follows the
    // latest frames, easing down slowly)
    std::shared_ptr<const R3DFrameState> seen;
    double next_period = 1.0 / 57.524, lateness = 0.004, lateness_rose = 0, lateness_spike = -100;
    DisplayClock display_clock;

    std::array<Snapshot2D, 4> snaps;   // the 2D layers of the last vblanks (snaps_mutex)
    size_t newest_snap = 0;
    std::mutex snaps_mutex;
    // Scratch layers of the 2D composition (compose_2d)
    std::vector<uint32_t> front_raw = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          front_raw_top = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          back_raw = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          back_raw_top = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          back_under = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          front_under = std::vector<uint32_t>(SCREEN_W * SCREEN_H);
    std::vector<uint16_t> tex_rows;
    uint64_t shown = ~0ull, presents = 0;
    double scene_total = 0;   // seconds of 2D composition and scene building since the last title
    ULONGLONG last_title = 0;

    // PC front end: main menu at launch, pause menu on Esc
    Frontend frontend;
    // The menus are being driven with a gamepad: no mouse pointer until the mouse moves
    bool pad_in_menu = false;
    UiOverlay ui;
    std::vector<uint32_t> ui_pixels;
    float haze_strength = 1, ao_strength = 1;   // of --haze and --ao, for the options menu

    // Boss mode: each fight starts from a game state in save/boss, made the
    // first time by LostWorld.exe --make-boss-state N in the background (in
    // a job object, so they stop with the game)
    std::vector<HANDLE> boss_makers = std::vector<HANDLE>(BOSS_COUNT, nullptr);
    HANDLE boss_job = nullptr;
    int boss_pending = -1;     // picked: loads once the boot screens are skipped
    bool boss_loading = false; // asked the game thread for it
    int boss_fight = -1;       // the fight on
    double boss_over_at = 0;   // when its stage ended or the game was over

    // Survival mode screens (loading, start, wave counter, results)
    SurvivalUi survival_ui;
    UiOverlay survival_layer;
    std::vector<uint32_t> survival_pixels;
    bool survival_shown = false;
    bool confirm_was_down = true;
    // A run starts from the arcade game at its attract (the home state: kept
    // the first time, put back the next ones), fast-forwarded and muted
    // behind the loading screen; Esc on the results goes back to the menu
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

    // Preparation thread: while interpolating, each new game frame is made
    // ready as soon as the game finishes it, its 3D scene and its 2D layers
    // side by side, so this thread only draws and keeps up with every
    // refresh (the two took ~14 ms here, more than a refresh at 120 Hz, and
    // the images went out unevenly). The interpolation delay follows when
    // the frames are ready (lateness), so it includes this work
    std::mutex prep_mutex;    // one composition and one scene build at a time
    std::mutex ready_mutex;   // ready_frames and ready_pictures
    // The last frames made ready, oldest first: the picture runs behind the
    // game by the frames' lateness, so after a slow frame several can be
    // waiting, and each is taken in turn (its vertices move from the frame
    // before it, so skipping one would stretch that motion over two)
    std::deque<std::shared_ptr<Prepared>> ready_frames;
    // New pictures for the GPU slots, in order: handed over when their frame
    // (or a later one) is taken, also those of frames never taken
    std::deque<NewPictures> ready_pictures;
    std::atomic<bool> prep_on{false}, prep_stop{false};
    std::atomic<uint64_t> prep_us{0}, prep_frames{0};   // its work, read once a second
    std::thread prep_thread;
    Worker layers_worker;   // composes the 2D layers while prep_thread builds the 3D scene
    // LW_FRAMELOG: a thread notes when each present reaches the screen
    std::atomic<uint64_t> presented_id{0};
    std::thread shown_waiter;

    // At most this many images wait for the screen (VK_KHR_present_wait): the
    // picture goes out that many refreshes after it is drawn (~17 ms at
    // 120 Hz) instead of after a full swapchain (~32 ms). When refreshes are
    // missed twice within 2 s (the GPU busier than usual), one more for 10 s.
    // Debugging aid: LW_QUEUE=N (0 = the whole swapchain)
    int queue_depth = 2;
    int queue_extra = 0;
    double went_out = 0, last_miss = -100;
    bool repeat_image = false;   // HALF FRAME RATE: this refresh shows the last image again
    // VSYNC: OFF still draws no more images than the monitor's refreshes:
    // without a limit the GPU draws thousands a second for nothing and the
    // game thread falls behind (uneven game frames). Measured with
    // LW_FRAMELOG on an RTX 5070 Ti at 120 Hz: ~1700 images a second
    double next_image = 0;
    HANDLE image_timer = nullptr;

    bool init(int argc, char** argv, bool first_start);   // false: could not start
    void run();                                            // until the window closes
    [[noreturn]] void shutdown();
    // Both threads stop when the loop ends (and with the App, however it ends)
    void stop_threads();
    ~App() { stop_threads(); }

    void poll_snapshots();
    void take_snapshot(uint64_t up_to, Layers2D& out);
    void survival_to_menu();
    void close_menu();
    void start_maker(int i);
    void start_survival(int stage, int zone);
    void survival_go_on(const SurvivalView& sv);
    void compose_2d(Layers2D& out);
    void apply_2d(const Layers2D& l);
    void give_pictures(uint64_t serial);
    // run(), one refresh after another
    void wait_for_refresh();
    void update_survival();
    void update_toast();
    void handle_menu_actions();
    // The video options the menu changes, to the renderer and the presenter:
    // at the start (was = nullptr; the 3D resolution, MSAA, anisotropic
    // filtering and vsync were given to init) and after a change in the menu
    void apply_video_options(const Options* was);
    void update_boss_mode();
    void update_menus();
    void read_inputs();
    bool present_frame(double loop_from);
    void update_title();
    void prepare_frames();   // the preparation thread
    void note_presents();    // the LW_FRAMELOG thread
};

// Copies the game's newest 2D layers into the ring if they are new; the
// caller holds snaps_mutex
void App::poll_snapshots() {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    if (g_video_frame != snaps[newest_snap].frame && !g_boot_skip) {
        newest_snap = (newest_snap + 1) % snaps.size();
        Snapshot2D& s = snaps[newest_snap];
        s.frame = g_video_frame;
        for (uint32_t p = 0; p < TILEGEN_PAGES; p++)   // (the pages copied again since this one was filled)
            if (s.serial[p] != g_video_page_serial[p]) {
                memcpy(&s.vram[p << 16], &g_video_vram[p << 16], 0x10000);
                s.serial[p] = g_video_page_serial[p];
            }
    }
}

// Copies the newest 2D layers at or before that vblank into `out` (its
// frame stays ~0 if there are none); takes snaps_mutex
void App::take_snapshot(uint64_t up_to, Layers2D& out) {
    std::lock_guard<std::mutex> lock(snaps_mutex);
    poll_snapshots();
    const Snapshot2D* pick = nullptr;
    for (const Snapshot2D& s : snaps)
        if (s.frame != ~0ull && s.frame <= up_to && (!pick || s.frame > pick->frame)) pick = &s;
    out.frame = pick ? pick->frame : ~0ull;
    if (pick) memcpy(out.vram.data(), pick->vram.data(), TILEGEN_SIZE);
}

// Survival results: Esc goes back to the main menu, and the arcade game as
// it was comes back when the menu closes
void App::survival_to_menu() {
    survival_end();
    game_state_load_home();
    frontend.open(Frontend::Screen::Main);
    g_menu_open = g_hold = true;
}

void App::close_menu() {
    frontend.open(Frontend::Screen::None);
    ui.hide();
    g_menu_open = g_hold = false;
    g_block_buttons = true;
}

// Starts LostWorld.exe --make-boss-state for boss i (see boss_job)
void App::start_maker(int i) {
    const std::wstring exe = exe_path_w();
    const std::string& rom = g_rom_zip.empty() ? g_data_dir : g_rom_zip;
    std::wstring cmd = L"\"" + exe + L"\" --make-boss-state " + std::to_wstring(i + 1) +
                       (g_rom_zip.empty() ? L" --data \"" : L" --rom \"") + std::wstring(rom.begin(), rom.end()) +
                       L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
        frontend.boss_status[i] = Frontend::BOSS_FAILED;
        return;
    }
    if (boss_job) AssignProcessToJobObject(boss_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    boss_makers[i] = pi.hProcess;
    frontend.boss_status[i] = Frontend::BOSS_PREPARING;
    printf("[boss] preparing %ls\n", BOSSES[i].name);
}

// A survival run at that arena, from the home state (see survival_wait_home)
void App::start_survival(int stage, int zone) {
    if (game_state_has_home()) game_state_load_home();
    else game_state_keep_home();
    survival_wait_home = true;
    survival_audio_at = g_sound.frames_out();
    survival_begin(stage, zone);
    g_boot_skip = g_fast_idle = true;
    g_sound.set_dsp_enabled(false);
    g_sound.set_muted(true);
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

// Composes the 2D layers of a vblank (out.frame, its layer memory in
// out.vram) for the renderer. One composition at a time (prep_mutex): the
// 2D packs and the back layers' scroll keep state between them
void App::compose_2d(Layers2D& out) {
    const TextHD* hd_text = g_opt.classic_text ? nullptr : &text;
    if (!hd_text) std::fill(out.text_cells.begin(), out.text_cells.end(), 0u);
    bool capture = capture_2d.exchange(false);
    if (capture) sky.capture();   // F12 also saves the backdrops, for textures/sky
    bool sky_on = sky.active();
    // Raw layers for recognising the 2D replacements (and for F12)
    bool raw_front = !pack2d.empty() || capture, raw_back = !pack2d_back.empty() || capture;
    TileGen::compose(out.vram.data(),
                     {.back = out.back.data(),
                      .front = out.front.data(),
                      .front_raw = raw_front ? front_raw.data() : nullptr,
                      .front_raw_top = raw_front ? front_raw_top.data() : nullptr,
                      .front_under = raw_front ? front_under.data() : nullptr,
                      .back_raw = raw_back ? back_raw.data() : nullptr,
                      .back_raw_top = raw_back ? back_raw_top.data() : nullptr,
                      .back_under = raw_back ? back_under.data() : nullptr,
                      .text_cells = out.text_cells.data(),
                      .sky_coords = sky_on ? out.sky_coords.data() : nullptr,
                      .back_rows = back_rows.data()},
                     hd_text, sky_on ? &sky : nullptr);
    // The layer and scroll of each row of the back layers, for drawing
    // them in between game frames (renderer.set_sky_rows)
    {
        out.back_rows = back_rows;
        // Debugging aid: LW_SKYLOG=path writes the back layers' scroll of each 2D
        // frame and the world camera of each 3D frame (tools/sky_log.py)
        if (g_sky_log) {
            std::lock_guard<std::mutex> lock(g_sky_log_mutex);
            fprintf(g_sky_log, "2d %llu", (unsigned long long)out.frame);
            for (int y = 0; y < SCREEN_H; y += 8)
                fprintf(g_sky_log, " %d,%d,%d", back_rows[3 * y], back_rows[3 * y + 1], back_rows[3 * y + 2]);
            fprintf(g_sky_log, "\n");
        }
        out.shift_from = back_rows_frame;
        out.shift_to = back_rows_frame = out.frame;
    }
    out.sky_pictures = sky.take_new_images();
    if (capture) {   // F12 or --shot: capture for tools/make_2d_replacement.py
        CreateDirectoryA("out/2d", nullptr);
        std::string path = "out/2d/capture_" + std::to_string(out.frame);
        Pack2D::save_capture(front_raw.data(), path);
        Pack2D::save_capture(front_raw_top.data(), path + "_layer_a");   // only the texts' layers
        Pack2D::save_capture(back_raw.data(), path + "_back");          // the layers below the 3D image
        Pack2D::save_capture(back_raw_top.data(), path + "_back_layer_a");
        printf("2D layer saved to %s.png\n", path.c_str());
    }
    // The top band only moves to the screen edges when it looks like a
    // HUD (empty in the middle), not when it is part of a picture.
    // Measured before the 2D replacements clear what they cover (the
    // boss name plate in the middle stays a reason to keep it)
    int middle = 0;
    for (int y = 0; y < 36; y++)
        for (int x = 160; x < 336; x++)
            middle += (out.front[y * SCREEN_W + x] >> 24) != 0 ||
                      out.text_cells[((y / 8) * TextHD::CELLS_X + x / 8) * TextHD::CELL_TEXELS] != 0;
    out.hud_band = middle * 20 < 36 * 176;
    int offset[3];
    TileGen::front_offset(out.vram.data(), offset);
    // Survival's start screen: the game stands still on its first frame
    // in the arena, where the foreground can still hold the screen the
    // home state was kept at (from the menu, the boot's FBI warning)
    SurvivalMode surv_mode = survival_shown ? survival_view().mode : SurvivalMode::Off;
    if (surv_mode == SurvivalMode::Loading || surv_mode == SurvivalMode::Ready) {
        std::fill(out.front.begin(), out.front.end(), 0u);
        std::fill(out.text_cells.begin(), out.text_cells.end(), 0u);
    } else if (!pack2d.empty())
        pack2d.apply(out.front.data(), SCREEN_W, front_raw.data(), front_raw_top.data(), out.text_cells.data(),
                     false, front_under.data(), out.back.data() + BACK_MARGIN, BACK_W);
    if (!pack2d_back.empty())
        pack2d_back.apply(out.back.data() + BACK_MARGIN, BACK_W, back_raw.data(), back_raw_top.data(),
                          out.text_cells.data(), true, back_under.data());
    out.overlay_brightness = float(offset[0] + offset[1] + offset[2]) / (3 * 255.0f);
    out.overlays = pack2d.take_new_images();
    for (auto& img : pack2d_back.take_new_images()) out.overlays.push_back(std::move(img));
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
        std::lock_guard<std::mutex> lock(ready_mutex);
        while (!ready_pictures.empty() && ready_pictures.front().serial <= serial) {
            due.push_back(std::move(ready_pictures.front()));
            ready_pictures.pop_front();
        }
    }
    for (NewPictures& n : due) {
        if (!n.replacements.empty()) renderer.add_replacements(n.replacements);
        if (!n.overlays.empty()) renderer.add_overlays(n.overlays);
        if (!n.sky.empty()) renderer.add_sky_pictures(n.sky);
    }
}

// The preparation thread (see ready_frames)
void App::prepare_frames() {
    std::vector<std::shared_ptr<Prepared>> pool;   // reused once nobody else holds them
    std::shared_ptr<const R3DFrameState> done;
    uint64_t serial = 0;
    while (!prep_stop) {
        std::shared_ptr<const R3DFrameState> state;
        {
            std::unique_lock<std::mutex> lock(g_r3d_mutex);
            g_r3d_cv.wait_for(lock, std::chrono::milliseconds(20),
                              [&] { return prep_stop || (prep_on && g_r3d_latest != done); });
            state = g_r3d_latest;
        }
        if (!prep_on) done.reset();
        if (prep_stop || !prep_on || !state || state == done) continue;
        double before = done ? done->time : 0;
        done = state;
        std::shared_ptr<Prepared> p;
        for (auto& q : pool)
            if (q.use_count() == 1) {
                p = q;
                break;
            }
        if (!p) pool.push_back(p = std::make_shared<Prepared>());
        p->state = state;
        p->serial = ++serial;
        p->period = before > 0 ? std::clamp(state->time - before, 1.0 / 240, 0.1) : 1.0 / 57.524;
        NewPictures pictures;
        pictures.serial = p->serial;
        {
            std::lock_guard<std::mutex> lock(prep_mutex);
            take_snapshot(state->vblank, p->layers);   // the 2D layers of the frame's vblank
            bool layers = p->layers.frame != ~0ull;
            if (layers) layers_worker.run([&] { compose_2d(p->layers); });
            double from = now_seconds();
            builder.build(*state, p->scene);
            double built_at = now_seconds();
            pictures.replacements = pack.take_new_images();
            if (layers) layers_worker.wait();
            double took = now_seconds() - from;
            prep_us += uint64_t(took * 1e6);
            prep_frames++;
            frame_log("R %llu %.6f %.3f %.3f\n", (unsigned long long)state->frame, now_seconds(),
                      (built_at - from) * 1000, took * 1000);
            pictures.overlays.swap(p->layers.overlays);
            pictures.sky.swap(p->layers.sky_pictures);
            p->layers.overlays.clear();
            p->layers.sky_pictures.clear();
        }
        std::lock_guard<std::mutex> lock(ready_mutex);
        ready_pictures.push_back(std::move(pictures));
        p->ready_at = now_seconds();
        if (prep_on) {
            ready_frames.push_back(p);
            if (ready_frames.size() > 8) ready_frames.pop_front();
        }
    }
}

void App::note_presents() {
    uint64_t next = 1;
    while (!prep_stop) {
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
    prep_stop = true;
    g_r3d_cv.notify_all();
    if (prep_thread.joinable()) prep_thread.join();
    if (shown_waiter.joinable()) shown_waiter.join();
}

bool App::init(int argc, char** argv, bool first_start) {
    presenter.set_vsync(g_opt.vsync);
    if (!presenter.init("Lost World Native", int(SCREEN_H * 2 * std::max(g_opt.max_aspect, 4.0f / 3.0f)), SCREEN_H * 2,
                        g_opt.dlaa)) {
        fprintf(stderr, "Could not start Vulkan; try --headless\n");
        return false;
    }
    printf("GPU: %s\n", presenter.device_name().c_str());
    g_pads.init();
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
        g_hold = g_opt.menu && !survival_cli;   // the main menu comes first
        g_boot_skip = g_fast_idle = !g_opt.show_boot;
        g_sound.set_dsp_enabled(!g_boot_skip);
        g_sound.set_muted(g_boot_skip);
        CreateThread(nullptr, 512u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }

    // (not in test runs, nor over graphics options given on the command line)
    for (int i = 1; i < argc && first_start; i++)
        for (const char* o : {"--scale", "--half-rate", "--rt", "--no-rt", "--ao", "--no-ao", "--no-shadows", "--no-shafts",
                              "--no-soft", "--sky", "--no-relief", "--bloom", "--no-bloom", "--msaa", "--aa", "--aniso",
                              "--pixel2d"})
            if (strcmp(argv[i], o) == 0) first_start = false;
    if (first_start && g_opt.menu) {
        int q = suggest_quality(presenter);
        static const char* const NAMES[4] = {"LOW", "MEDIUM", "HIGH", "ULTRA"};
        printf("[video] first start: quality %s for this graphics card (options menu, DISPLAY)\n", NAMES[q]);
        ini.set_quality(q);
        quality_options(ini, g_opt);   // what QUALITY sets, now from the level picked
    }
    if (!renderer.init(presenter, g_opt.supersample, g_opt.msaa, g_opt.aniso, g_opt.dlaa)) {
        fprintf(stderr, "Could not start the 3D renderer on %s (see the [vulkan] line above)\n",
                presenter.device_name().c_str());
        return false;
    }
    renderer.set_hud_edges(!g_opt.hud_centre);
    if (g_opt.haze > 0) printf("[video] distance haze on (strength %.2f)\n", g_opt.haze);
    if (g_opt.dlaa && !renderer.dlaa_active()) printf("[video] DLAA not available: using MSAA\n");
    if (renderer.dlaa_active())
        printf("[video] antialiasing NVIDIA DLAA, anisotropic filtering %dx%s\n", g_opt.aniso,
               g_opt.aniso > 0 ? " with mipmaps" : " (no mipmaps)");
    else
        printf("[video] antialiasing MSAA %dx, anisotropic filtering %dx%s\n", renderer.msaa_samples(), g_opt.aniso,
               g_opt.aniso > 0 ? " with mipmaps" : " (no mipmaps)");
    if (g_opt.light_gain != 1.0f) {
        renderer.set_light_gain(g_opt.light_gain);
        printf("[video] light x%.2f on the sun and the ambient light\n", g_opt.light_gain);
    }
    builder.set_remove_letterbox(!g_opt.letterbox);
    builder.set_hud_edges(!g_opt.hud_centre);
    pack.init("textures", g_opt.dump_textures ? "out/textures" : "");
    pack.set_max_slots(renderer.replacement_slots());   // as many as this GPU's descriptor limits allow
    builder.set_texture_pack(&pack, &g_texture_ram);
    pack2d.init("textures/2d", 1, 27);
    pack2d_back.init("textures/2d_back", 28, SkyPack::FIRST_SLOT - 1);
    sky.init("textures/sky", g_opt.dump_textures ? "out/sky" : "");
    haze_strength = g_opt.haze > 0 ? g_opt.haze : 1;
    ao_strength = g_opt.ao > 0 ? g_opt.ao : 1;
    apply_video_options(nullptr);   // the menu's video options (and the sky's picture)
    if ((g_opt.menu || !g_opt.classic_text) && text.init("textures/text")) renderer.set_text_atlas(text);
    load_crosshairs(renderer);
    builder.set_vrom(g_vrom.data(), g_vrom.size());
    last_title = GetTickCount64();
    raw_mice::init(presenter.hwnd());   // a second mouse can be player 2's gun
    if (g_opt.several_mice) raw_mice::set_saved(ini.gun_p1, ini.gun_p2);   // the guns picked in GUN TEST
    presenter.raw_input = raw_mice::on_input;
    if (g_opt.menu) {
        if (!frontend.init("third_party/fonts")) printf("[menu] fonts missing: using the Windows ones\n");
        if (!ui.init(presenter)) g_opt.menu = false;
    }
    presenter.save_pipeline_cache();   // what start-up compiled, kept even if the run ends without shutdown()
    if (g_opt.menu) {
        FrontendSettings& s = frontend.settings;
        s = ini;   // the window, the menu music, the gamepad and the guns picked: as the file has them
        settings_from_options(g_opt, s);
        frontend.aa_at_launch = s.aa;
        frontend.rt_available = renderer.ray_tracing_available();
        if (s.fullscreen && !presenter.fullscreen()) presenter.toggle_fullscreen();
        if (!survival_cli) {   // survival mode goes straight into the game
            frontend.open(Frontend::Screen::Main);
            g_menu_open = true;
        }
        presenter.input_hook = [this](UINT msg, WPARAM wp, LPARAM lp) -> bool {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (!frontend.active()) {   // in the game: Esc pauses
                if (msg == WM_KEYDOWN && wp == VK_ESCAPE && survival_view().mode == SurvivalMode::Results) {
                    survival_to_menu();
                    return true;
                }
                if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
                    frontend.open(Frontend::Screen::Pause);
                    g_menu_open = g_hold = true;
                    return true;
                }
                return false;
            }
            switch (msg) {
            case WM_KEYDOWN: return frontend.key(int(wp));
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
            case WM_LBUTTONDOWN: frontend.click(x, y); return true;
            case WM_RBUTTONDOWN:   // right button: back (not in GUN TEST, where it is a gun reloading)
                if (!frontend.gun_test_open()) frontend.key(VK_ESCAPE);
                return true;
            case WM_MOUSEWHEEL: {
                POINT p{x, y};   // screen coordinates for this one
                ScreenToClient(presenter.hwnd(), &p);
                frontend.wheel(p.x, p.y, GET_WHEEL_DELTA_WPARAM(wp));
                return true;
            }
            }
            return false;
        };
    }
    boss_job = CreateJobObjectW(nullptr, nullptr);
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (boss_job) SetInformationJobObject(boss_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    frontend.boss_status.assign(BOSS_COUNT, Frontend::BOSS_MISSING);
    for (int i = 0; i < BOSS_COUNT; i++)
        if (game_state_valid(boss_state_path(i))) frontend.boss_status[i] = Frontend::BOSS_READY;
    // Debugging aid: LW_BOSS=N starts the fight with boss N (1-4) right away
    if (const char* b = getenv("LW_BOSS")) {
        int i = atoi(b) - 1;
        if (i >= 0 && i < BOSS_COUNT && frontend.boss_status[i] == Frontend::BOSS_READY) boss_pending = i;
    }
    // Survival mode screens (loading, start, wave counter, results)
    survival_shown = (survival_cli || g_opt.menu) && survival_ui.init("third_party/fonts") &&
                     survival_layer.init(presenter);
    survival_cli_pending = survival_cli;
    // Achievement notices, over the game and under the menus
    toast_shown = toast.init("third_party/fonts") && toast_layer.init(presenter);
    toast.set_volume(g_opt.volume);
    if (g_opt.menu && !g_opt.headless) music.open();
    music_time = now_seconds();
    prep_thread = std::thread([this] { prepare_frames(); });
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
            handle_menu_actions();
            update_boss_mode();
            update_menus();
        }
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
    if (!g_opt.vsync) {   // the next image a refresh after the last (from now after a slow one)
        double period = display_clock.period, now = now_seconds();
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
        if (survival_last == SurvivalMode::Loading && sv.mode == SurvivalMode::Off && g_opt.menu && !g_menu_open)
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
        for (int i = 0; i < Gamepads::MAX; i++) down |= fg && g_pads.pad(i).fire;
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
            g_block_buttons = true;   // the click does not fire in the game
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
    for (int i : stats_take_new_achievements())
        toast.push(achievement_table()[size_t(i)].name, achievement_table()[size_t(i)].text);
    for (int i : survival_take_unlocked())
        toast.push_rising(L"ARENA UNLOCKED  \u00B7  SURVIVAL!", SURVIVAL_ZONES[i].name,
                          L"Play it from PLAY > SURVIVAL");
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
    renderer.set_relief(o.relief);
    renderer.set_sharpen(o.sharpen ? 0.5f : 0.0f);
    renderer.set_modern_torch(o.modern_torch);
    if (!was || o.sky_procedural != was->sky_procedural) {
        {
            std::lock_guard<std::mutex> lock(prep_mutex);   // not while composing
            sky.set_procedural(o.sky_procedural);
        }
        renderer.set_night_sky(o.sky_procedural);
    }
}

void App::handle_menu_actions() {
    for (Frontend::Action a : frontend.take_actions()) {
        FrontendSettings& s = frontend.settings;
        switch (a) {
        case Frontend::Action::StartArcade:   // the game at the SEGA logo is the home state QUIT TO MAIN MENU puts back
            if (!g_boot_skip) game_state_keep_home();
            random_spawns_set(frontend.random_spawns);   // CLASSIC > ARCADE or RANDOM SPAWNS
            if (frontend.random_spawns) weather_choose(frontend.run_weather());
            [[fallthrough]];
        case Frontend::Action::Resume:
            boss_pending = -1;
            close_menu();
            break;
        case Frontend::Action::StartSurvival: {
            const SurvivalZone& z = SURVIVAL_ZONES[frontend.survival_choice];
            boss_pending = -1;
            random_spawns_set(false);
            weather_choose(frontend.run_weather());
            if (boss_fight >= 0 || frontend.boss_mode) {   // from boss mode: back to the arcade first
                boss_fight = -1;
                frontend.boss_mode = false;
            }
            start_survival(z.stage, z.zone);
            break;
        }
        case Frontend::Action::StartBoss:
            random_spawns_set(false);
            if (frontend.boss_status[frontend.boss_choice] == Frontend::BOSS_READY) boss_pending = frontend.boss_choice;
            break;
        case Frontend::Action::PrepareBosses:
            for (int i = 0; i < BOSS_COUNT; i++)
                if (frontend.boss_status[i] == Frontend::BOSS_FAILED) frontend.boss_status[i] = Frontend::BOSS_MISSING;
            break;
        case Frontend::Action::LeaveBossMode:   // the arcade game as it was comes back when the menu closes
            boss_pending = -1;
            game_state_load_home();
            boss_fight = -1;
            frontend.boss_mode = false;
            frontend.open(Frontend::Screen::Main);
            break;
        case Frontend::Action::QuitToMenu:   // from the pause menu of any mode
            boss_pending = -1;
            if (survival_view().mode != SurvivalMode::Off) {
                survival_to_menu();
                break;
            }
            boss_fight = -1;
            frontend.boss_mode = false;
            game_state_load_home();
            frontend.open(Frontend::Screen::Main);
            break;
        case Frontend::Action::Quit: presenter.close(); break;
        case Frontend::Action::ToggleFullscreen: presenter.toggle_fullscreen(); break;
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
            g_pad_settings = {s.pad_speed, s.gyro, s.gyro_speed, s.vibration, s.aim_assist};
            s.save("save/settings.ini");
            break;
        }
        default: break;
        }
    }
}

// Boss mode: the state makers, the fight picked, the end of the fight
void App::update_boss_mode() {
    for (int i = 0; i < BOSS_COUNT; i++) {
        if (boss_makers[i] && WaitForSingleObject(boss_makers[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(boss_makers[i]);
            boss_makers[i] = nullptr;
            bool ok = game_state_valid(boss_state_path(i));
            frontend.boss_status[i] = ok ? Frontend::BOSS_READY : Frontend::BOSS_FAILED;
            printf("[boss] %ls: %s\n", BOSSES[i].name, ok ? "ready" : "could not be prepared");
            frontend.invalidate();
        }
    }
    // Missing states are made from the launch on (once the boot is
    // over and the EEPROM file exists, see --make-boss-state), one at
    // a time so the game keeps its speed; all at once while the list
    // of bosses is open
    bool can_make = !g_boot_skip && GetFileAttributesA("save/eeprom.bin") != INVALID_FILE_ATTRIBUTES;
    for (int i = 0; can_make && i < BOSS_COUNT; i++) {
        bool busy = std::any_of(boss_makers.begin(), boss_makers.end(), [](HANDLE h) { return h != nullptr; });
        if (frontend.boss_status[i] == Frontend::BOSS_MISSING &&
            (!busy || frontend.screen() == Frontend::Screen::Boss)) {
            start_maker(i);
            frontend.invalidate();
        }
    }
    if (boss_pending >= 0 && !g_boot_skip) {
        if (game_state_load(boss_state_path(boss_pending), true)) {
            boss_loading = true;
            boss_fight = boss_pending;
            boss_over_at = 0;
            frontend.boss_mode = true;
            close_menu();
        } else {
            frontend.boss_status[boss_pending] = Frontend::BOSS_MISSING;   // made again
            frontend.invalidate();
        }
        boss_pending = -1;
    }
    if (boss_loading && game_state_result() != 0) {
        boss_loading = false;
        if (game_state_result() > 0) stats_boss_begin(boss_fight);
        if (game_state_result() < 0) {
            printf("[boss] could not start the fight\n");
            boss_fight = -1;
            frontend.boss_mode = false;
            frontend.open_boss_list(true);
            g_menu_open = g_hold = true;
        }
    }
    // The fight is over once its stage ends (the boss is down) or the
    // game is (no credits left): a moment later, the main menu, with
    // the arcade game as it was waiting behind it
    if (boss_fight >= 0 && !boss_loading && !frontend.active()) {
        // Player 1 state (docs/game_ram.md): 3 once the continue has run out
        bool over = rd8(0x1A623C) != BOSSES[boss_fight].stage_index || rd8(0x1A3784 + 0xD) == 3;
        if (!over) boss_over_at = 0;
        else if (boss_over_at == 0) {
            boss_over_at = now_seconds();
            stats_boss_end(rd8(0x1A623C) != BOSSES[boss_fight].stage_index);   // the stage ended: the boss is down
        }
        else if (now_seconds() - boss_over_at > 3) {
            boss_fight = -1;
            frontend.boss_mode = false;
            game_state_load_home();
            frontend.open(Frontend::Screen::Main);
            g_menu_open = g_hold = true;
        }
    }
}

// The menus: the window mode, their debugging aids, the records, GUN TEST, the
// music, the pointer and the menu picture
void App::update_menus() {
    if (frontend.settings.fullscreen != presenter.fullscreen()) {   // also F11 / Alt+Enter
        frontend.settings.fullscreen = presenter.fullscreen();
        frontend.settings.save("save/settings.ini");
        frontend.invalidate();
    }
    // Debugging aid: LW_MENU_KEYS="40,13,27" presses those keys (one
    // every 10 presents; Esc in the game pauses; "cX/Y" clicks at window
    // pixel X,Y), then saves out/menu.bmp and quits
    static const char* menu_keys = getenv("LW_MENU_KEYS");
    static int menu_presents = 0;
    if (menu_keys && ++menu_presents % 10 == 0) {
        if (*menu_keys == 'c') {
            int cx = 0, cy = 0, len = 0;
            sscanf(menu_keys, "c%d/%d%n", &cx, &cy, &len);
            menu_keys += len;
            if (*menu_keys == ',') menu_keys++;
            frontend.click(cx, cy);
        } else if (*menu_keys) {
            char* end = nullptr;
            int vk = int(strtol(menu_keys, &end, 10));
            menu_keys = *end == ',' ? end + 1 : end;
            if (frontend.active()) frontend.key(vk);
            else if (vk == VK_ESCAPE) {
                frontend.open(Frontend::Screen::Pause);
                g_menu_open = g_hold = true;
            }
        } else if (menu_presents % 30 == 0) {
            presenter.capture_next("out/menu.bmp");
            menu_keys = nullptr;
            presenter.close();
        }
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
        ULONGLONG now = GetTickCount64();
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
            Frontend::GunView g{d.path, d.name, d.absolute, d.last_input && now - d.last_input < 1000, d.has_pos,
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
        music.update(frontend.active() && !frontend.over_game() && frontend.settings.menu_music,
                     frontend.settings.volume, t - music_time);
        music_time = t;
    }
    if (!frontend.active()) pad_in_menu = false;   // a menu opened later starts with the mouse
    presenter.set_cursor_visible(frontend.active() && !pad_in_menu && !frontend.gun_test_open());
    static const bool menu_pad = getenv("LW_MENU_PAD") != nullptr;   // debugging aid: the gamepad's key help
    if (frontend.pad_prompts != (pad_in_menu || menu_pad)) {   // the key help shows the gamepad's buttons or the keys
        frontend.pad_prompts = pad_in_menu || menu_pad;
        frontend.invalidate();
    }
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
            static const char* menu_shot = getenv("LW_MENU_SHOT");   // debugging aid: the menu picture
            if (menu_shot) bmp_save(menu_shot, rc.right, rc.bottom, ui_pixels.data());
        }
    }
}

// The guns, the keyboard, the gamepads, F12 / F9 and the crosshairs
void App::read_inputs() {
    update_aim_reach(presenter.hwnd());
    poll_keyboard_mouse(presenter.hwnd());
    bool pad_pause = poll_gamepads(presenter.hwnd());
    if (g_pads.changed()) {   // the CONTROLS tab names the pads
        std::wstring names;
        bool gyro = false;
        for (int i = 0; i < Gamepads::MAX; i++) {
            const PadState& pad = g_pads.pad(i);
            if (!pad.connected) continue;
            names += (names.empty() ? L"" : L", ") + std::wstring(pad.name.begin(), pad.name.end());
            gyro |= pad.has_gyro;
        }
        frontend.gamepads = names;
        frontend.gyro_pad = gyro;
        frontend.invalidate();
    }
    for (PadKey k : g_pads.take_keys()) {   // the menus, as keys (START resumes from the pause menu)
        static const int VK[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE, 'Q', 'E', 'R'};
        bool resume = frontend.screen() == Frontend::Screen::Pause;
        if (frontend.active()) {
            frontend.key(k == PadKey::Start ? (resume ? VK_ESCAPE : VK_RETURN) : VK[int(k)]);
            pad_in_menu = true;
        }
    }
    if (pad_pause && g_opt.menu && !frontend.active()) {
        frontend.open(Frontend::Screen::Pause);
        pad_in_menu = true;
        g_menu_open = g_hold = true;
    }
    join_update();
    bool f12 = GetForegroundWindow() == presenter.hwnd() && (GetAsyncKeyState(VK_F12) & 0x8000);
    if (f12 && !f12_was_down) capture_2d = true;
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
    if (g_opt.shot_frame && shown != ~0ull && shown + 1 >= g_opt.shot_frame) capture_2d = true;
    for (int p = 0; p < 2; p++) {   // player 2's only while that gun is in use
        PlayerAim aim = player_aim(p);
        bool used = (p == 0 || g_inputs.p[1].playing) && !crosshair_idle(p, aim);
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
    if (interpolating != prep_on) {   // a frame it made earlier is not one to go back to
        prep_on = interpolating;
        std::lock_guard<std::mutex> lock(ready_mutex);
        ready_frames.clear();
    }
    double display = display_clock.tick(presenter.hwnd());
    // HALF FRAME RATE: above 100 Hz every other refresh shows the image
    // before again (only the crosshairs move), half the work of the GPU
    // (with vsync: without it the images are not tied to refreshes)
    bool half = g_opt.half_rate && g_opt.vsync && display_clock.period < 1.0 / 100 && g_opt.replay.empty() && !g_opt.shot_frame &&
                film.empty();
    repeat_image = half && !repeat_image;
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
        std::lock_guard<std::mutex> lock(ready_mutex);
        while (!ready_frames.empty() && built && ready_frames.front()->state->frame <= built->frame)
            ready_frames.pop_front();
        waiting = ready_frames;
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
        std::lock_guard<std::mutex> lock(snaps_mutex);
        poll_snapshots();
        newest_2d = snaps[newest_snap].frame;
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
    if (compose_up_to != ~0ull) take_snapshot(compose_up_to, own_layers);
    if (compose_up_to != ~0ull && own_layers.frame != ~0ull) {
        std::lock_guard<std::mutex> lock(prep_mutex);
        compose_2d(own_layers);
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
            std::lock_guard<std::mutex> lock(prep_mutex);
            builder.build(*latest, own_scene);
            auto images = pack.take_new_images();
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
        // Debugging aid: LW_SPOTLOG=path writes the world viewport's spotlight every game frame
        static FILE* spot_log = getenv("LW_SPOTLOG") ? fopen(getenv("LW_SPOTLOG"), "w") : nullptr;
        if (spot_log) {
            int worlds = 0;
            for (const R3DViewport& v : scene->viewports)
                if (v.world) {
                    worlds++;
                    fprintf(spot_log,
                            "%llu vp%d col %.0f%.0f%.0f c %.4f %.4f r %.4f %.4f start %.3f ext %.2f sun %.3f amb %.3f "
                            "fog %.5f/%.3f scroll %.3f far %.1f black %d night %.2f "
                            "light %.3f %.3f %.3f world %.3f %.3f %.3f\n",
                            (unsigned long long)built->frame, worlds, v.spot_color[0], v.spot_color[1], v.spot_color[2],
                            v.spot_center[0], v.spot_center[1], v.spot_radius[0], v.spot_radius[1], v.spot_start,
                            v.spot_extent, v.sun_intensity, v.ambient, v.fog_density, v.fog_ambient, v.scroll_fog,
                            v.scenery_far, int(renderer.back_black()), renderer.night_level(), v.sun[0], v.sun[1], v.sun[2],
                            v.world_axes[0] * v.sun[0] + v.world_axes[1] * v.sun[1] + v.world_axes[2] * v.sun[2],
                            v.world_axes[3] * v.sun[0] + v.world_axes[4] * v.sun[1] + v.world_axes[5] * v.sun[2],
                            v.world_axes[6] * v.sun[0] + v.world_axes[7] * v.sun[1] + v.world_axes[8] * v.sun[2]);
                }
            if (!worlds) fprintf(spot_log, "%llu no world\n", (unsigned long long)built->frame);
            fflush(spot_log);
        }
        // Debugging aid: LW_FILM=first,count records that many game frames
        // as snapshots in out/film, to replay as a sequence
        static const char* film_rec = getenv("LW_FILM");
        static int film_saved = 0;
        if (film_rec && g_opt.replay.empty()) {
            unsigned long long first = 0;
            int count = 0, every = 1;   // optional third value: keep one frame in that many
            sscanf(film_rec, "%llu,%d,%d", &first, &count, &every);
            static unsigned long long film_last = 0;
            if (built->frame >= first && film_saved < count && (!film_saved || built->frame >= film_last + every)) {
                film_last = built->frame;
                CreateDirectoryA("out/film", nullptr);
                char path[64];
                snprintf(path, sizeof(path), "out/film/f_%06llu.bin", (unsigned long long)built->frame);
                save_snapshot(path, *built, layers_shown->vram);
                film_saved++;
            }
        }
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
    // Debugging aid: LW_SHOTS=N[,FROM[,TO]] saves one game frame in N as out/shots/<frame>.bmp
    static int shots_every = 0;
    static unsigned long long shots_from = 0, shots_to = ~0ull;
    static bool shots_parsed = false;
    if (!shots_parsed) {
        shots_parsed = true;
        if (const char* e = getenv("LW_SHOTS")) sscanf(e, "%d,%llu,%llu", &shots_every, &shots_from, &shots_to);
    }
    static uint64_t last_shot = 0;
    if (shots_every > 0 && shown != ~0ull && shown >= shots_from && shown <= shots_to &&
        shown >= last_shot + shots_every) {
        last_shot = shown;
        CreateDirectoryA("out/shots", nullptr);
        char path[64];
        snprintf(path, sizeof(path), "out/shots/%06llu.bmp", (unsigned long long)shown);
        presenter.capture_next(path);
    }
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
    ULONGLONG now = GetTickCount64();
    if (now - last_title >= 1000) {
        // Where the time goes, in ms per frame: the game thread (CPU, without
        // its waits), the preparation thread (per game frame), this thread's
        // scene work and command recording, and the time blocked on the GPU
        // and the display (vsync)
        VkPresenter::Timing t = presenter.take_timing();
        uint64_t game_us = g_game_busy_us.exchange(0), game_n = g_game_frames.exchange(0);
        double per = t.frames ? 1000.0 / t.frames : 0;   // seconds summed over the presents -> ms per present
        double game_ms = game_n ? double(game_us) / 1000.0 / double(game_n) : 0;
        double scene_ms = presents ? scene_total * 1000.0 / double(presents) : 0;
        uint64_t prep_total = prep_us.exchange(0), prep_n = prep_frames.exchange(0);
        double prep_ms = prep_n ? double(prep_total) / 1000.0 / double(prep_n) : 0;
        char title[256];
        snprintf(title, sizeof(title),
                 "Lost World Native - %llu Hz display - frame %llu - %zu polygons | ms/frame: game %.1f, "
                 "prep %.1f, scene %.1f, render %.1f, GPU %.1f",
                 (unsigned long long)(presents * 1000 / (now - last_title)), (unsigned long long)shown,
                 scene->polygons, game_ms, prep_ms, scene_ms, t.record * per, (t.gpu_wait + t.present) * per);
        SetWindowTextA(presenter.hwnd(), title);
        static ULONGLONG last_log = now;
        if (now - last_log >= 5000) {   // the same, in the log, every 5 s
            last_log = now;
            printf("[timing] frame %llu  %zu polygons  ms/frame: game %.2f  prep %.2f  scene %.2f  render %.2f  "
                   "GPU wait %.2f  present %.2f  (%llu Hz display)\n",
                   (unsigned long long)shown, scene->polygons, game_ms, prep_ms, scene_ms, t.record * per,
                   t.gpu_wait * per, t.present * per, (unsigned long long)(presents * 1000 / (now - last_title)));
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
    g_audio.stop();
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
    g_pads.shutdown();
    ExitProcess(0);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // keep logs even if the process dies
    // The menus (and the settings they save) are left out of test runs, so
    // those always start from the same defaults
    g_opt.menu = !options_disable_menu(argc, argv);
    FrontendSettings ini;
    // The first start (no settings saved yet) picks the QUALITY for the graphics card
    bool first_start = g_opt.menu && !file_exists("save/settings.ini");
    if (g_opt.menu) {   // save/settings.ini, written by the options menu; command-line options win
        ini.load("save/settings.ini");
        options_from_settings(ini, g_opt);
        g_pad_settings = {ini.pad_speed, ini.gyro, ini.gyro_speed, ini.vibration, ini.aim_assist};
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
    printf("Lost World Native - part 4 (%s)\n", g_opt.headless ? "no window" : "Vulkan window, 3D + 2D");
    {
        std::vector<uint8_t> program, samples;
        if (!load_game_rom(program, samples)) return 1;
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
    app.survival_cli = survival_cli;
    app.cli_stage = cli_stage, app.cli_zone = cli_zone;
    if (!app.init(argc, argv, first_start)) return 1;
    app.run();
    app.shutdown();
}
