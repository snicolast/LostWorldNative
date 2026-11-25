// The rules of the game's flow (game_flow.h)
#define WIN32_LEAN_AND_MEAN   // runtime.h brings windows.h: no min/max macros
#define NOMINMAX
#include "game_flow.h"

#include <cstdio>

#include "bosses.h"
#include "game_ram.h"
#include "inputs.h"
#include "ppc_rt.h"
#include "random_spawns.h"
#include "runtime.h"
#include "stats.h"
#include "survival.h"

// The place caption shown as each stage starts ("-Crossing point of rescue
// parties.") does not go through the subtitle function: it is one more image
// of the text table at 0x134B28 (16 bytes per text: Japanese image and width
// in tiles, English image and width). The caption is centred from its width
// and not drawn when it would start off the screen, so with subtitles off
// these widths are made too wide to fit; the originals come back when they are
// turned on again. Checked every frame: the table is reloaded at power-on.
static void hide_stage_captions(bool hide) {
    static const uint32_t IDS[] = {5, 18, 20, 46};
    static uint32_t orig[4][2];
    for (int i = 0; i < 4; i++)
        for (int lang = 0; lang < 2; lang++) {
            uint32_t a = ram::TEXT_TABLE + IDS[i] * 16 + 4 + lang * 8;
            // not loaded yet (or the power-on RAM test running): leave it alone
            if (rd32(a - 4) >> 24 != 0xFF) return;
            uint32_t w = rd32(a);
            if (w != 0x7E) orig[i][lang] = w < 0x40 ? w : 0;
            uint32_t want = hide ? 0x7E : orig[i][lang];
            if (w != want && orig[i][lang]) wr32(a, want);
        }
}

void game_flow_frame_start() {
    {   // the game sets its language flag (0x1A6168) once at start: a later
        // change in the settings is written there so the next text uses it
        static int applied = g_subtitles;
        if (g_subtitles != applied) {
            applied = g_subtitles;
            wr32(ram::LANGUAGE, rt_text_region(rd8(ram::SETTING_COUNTRY)));
        }
    }
    hide_stage_captions(g_subtitles == 0);
    if (g_frame >= 1000) {   // FREE PLAY, past the boot (game_ram.md: no setting written there)
        // The flag the game works out of its coin setting (0xFF when
        // 0x1225 = 0x1A, FREE PLAY in the test menu's COIN ASSIGNMENTS):
        // START alone starts and continues, a coin adds no credit and
        // the screen says FREE PLAY. With it the game never goes back
        // to its attract (a game over leads to the PUSH START screen,
        // as with credits left), so with the main menu on it is only
        // set while a player plays or is on the CONTINUE screen
        // (slot 0-2, a game mode other than the attract's 0 and 1):
        // a game over still ends in the attract, where the menu comes
        // back, and PLAY's own credit starts the game. Not in
        // survival or boss mode, which have no continue. Off, the
        // test menu's own setting stands (also over a kept state)
        uint32_t mode = rd32(ram::GAME_MODE);
        bool in_game = mode != 0 && mode != 1 && (rd8(ram::P1_SLOT) <= 2 || rd8(ram::P2_SLOT) <= 2);
        bool forced = g_free_play && !survival_enabled() && g_boss_maker < 0 && (!g_skip_attract || in_game);
        wr8(ram::FREE_PLAY, forced || rd8(ram::SETTING_COIN) == 0x1A ? 0xFF : 0);
    }
    for (int p = 0; p < 2; p++) {   // player slots (0x10 bytes each): +0xD = 1 or 2 while playing
        uint8_t state = rd8(ram::P1_SLOT + 0x10 * p);
        g_inputs.p[p].playing = state == 1 || state == 2;
    }
}

// A game that ends goes on to the attract. The game's mode word (0x1A6164)
// is 0xB from the route selection to the end of a game: through the CONTINUE
// count, TOTAL SCORE, the name entry and rankings and GAME OVER; it goes to
// 0 on the frame the attract's first screen (the parental warning, then
// FBI and SEGA logos, title, demo) starts (0 logo screens, 1 title, demo and
// rankings; 0xA a frame or two around a coin or START, and as a stage
// starts; 0x14 then 0x15 as a stage is cleared, on to the next one: only 0 and 1 end
// a game, not any value other than 0xB). The route selection
// (0x1C2AD8) stays at 0xC through the attract, so it cannot tell. On that
// frame, before its picture is taken, the game stands still, the main menu
// opens (g_game_over_menu, app.cpp) and the home state (the game at its SEGA
// logo) is put back, ready for PLAY. Not in survival, boss mode or
// --make-boss-state, which have their own ends, and only with the menu on
// (g_skip_attract)
static void attract_guard_frame() {
    static bool game_on = false;
    static uint32_t mode_was = 0;
    uint32_t mode = rd32(ram::GAME_MODE), select = rd32(ram::COURSE_SELECT);
    if (mode != mode_was && (mode == 0xB || mode_was == 0xB))
        printf("[play] game mode %X -> %X (route selection %X, frame %llu)\n", unsigned(mode_was), unsigned(mode),
               unsigned(select), (unsigned long long)g_frame);
    mode_was = mode;
    if (g_attract_watch_reset) {   // a state put back (QUIT TO MAIN MENU, survival, boss mode)
        g_attract_watch_reset = false;
        game_on = false;
    }
    if (survival_enabled() || g_boss_maker >= 0) {
        game_on = false;
        return;
    }
    if (mode == 0xB && select == 0xC) game_on = true;   // past the route selection: a game played
    if (mode != 0 && mode != 1) return;   // still a game (0xA, 0x14 and 0x15 between stages)
    if (!game_on) return;
    game_on = false;
    if (!g_skip_attract || g_arcade_entry || g_boot_skip) return;
    printf("[play] the game is over: back to the main menu instead of the attract (frame %llu)\n",
           (unsigned long long)g_frame);
    g_hold = true;   // it stands still, behind the menu, until PLAY
    game_state_load_home();
    g_game_over_menu = true;
}

// ---- No attract with the menu on ----
// The arcade game's attract (title, rankings, the demo that plays by itself)
// never shows: the main menu stands in for it, as on a game made for the PC.
//
// PLAY > CLASSIC (g_arcade_entry, set by app.cpp): the game, at its SEGA logo
// (the home state) or wherever the menu left it, runs on unseen and muted,
// with a credit and START every second, until the route selection opens
// (0x1C2AD8 1-2); then the menu closes on it. The credit is only put in
// once the home state is kept, so it is not in there (QUIT TO MAIN MENU
// would put it back as CREDIT 1), and only every 5 s: the route selection
// can take a few seconds to open after START, and a coin every second left
// CREDIT 1 on it
bool arcade_entry_frame() {
    static uint64_t from = 0, select_at = 0;
    PlayerInputs& p = g_inputs.p[0];
    auto finish = [&](const char* why) {
        p.coin = p.start = false;
        printf("[play] %s after %llu frames\n", why, (unsigned long long)(g_frame - from));
        g_arcade_entry = 0;
        from = select_at = 0;
        return true;
    };
    if (g_arcade_entry == 1) {
        g_arcade_entry = 2;
        from = g_frame, select_at = 0;
        p.trigger = p.offscreen = false;
    }
    uint32_t select = rd32(ram::COURSE_SELECT);   // 0 attract, 1-2 choosing, 0xC playing
    if (!g_logo_up || game_state_pending()) {   // the boot screens, or a state still being put back
        p.coin = p.start = false;
        if (g_frame - from > 20000) return finish("given up (no SEGA logo)");
        return false;
    }
    if (!game_state_has_home()) {   // the home state first (the game at its SEGA logo)
        game_state_keep_home();
        p.coin = p.start = false;
        return false;
    }
    if (select == 0xC) return finish("in a game already");
    if (select == 0) {
        if (g_frame - from > 9000) return finish("route selection not reached, given up");
        uint64_t k = (g_frame - from) % 300;
        p.coin = k < 6;
        p.start = k % 60 >= 30 && k % 60 < 36;
        return false;
    }
    p.coin = p.start = false;
    if (!select_at) select_at = g_frame;
    if (g_frame - select_at < 20) return false;   // its first frames drawn
    return finish("route selection");
}

void game_flow_modes(Ctx& c) {
    attract_guard_frame();   // before the frame's picture is taken
    {   // the attract: game mode 0 or 1 (attract_guard_frame), not survival or boss mode
        uint32_t mode = rd32(ram::GAME_MODE);
        g_in_attract = (mode == 0 || mode == 1) && !survival_enabled() && g_boss_maker < 0;
    }
    if (survival_frame(c)) g_hold = true;   // survival start and results screens
    stats_frame();
    random_spawns_frame();
    if (g_boss_maker >= 0) boss_maker_frame();
    {   // PLAY given up on its way (app.cpp, cancel_play): no coin or START left held
        static bool entry_was = false;
        if (entry_was && !g_arcade_entry) g_inputs.p[0].coin = g_inputs.p[0].start = false;
        entry_was = g_arcade_entry != 0;
    }
}
