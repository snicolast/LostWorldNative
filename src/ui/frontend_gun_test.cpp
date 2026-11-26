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
