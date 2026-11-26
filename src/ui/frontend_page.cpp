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

// ---- The settings page (OPTIONS, SETTINGS, RECORDS), as Sega Rally 2's ----

namespace {
// Under each section's name in the column at the left, and in the panel when
// the column is chosen
const wchar_t* const TAB_ABOUT[] = {L"PC edition or the arcade as it was", L"Screen, frame rate, picture",
                                    L"Lighting, ray tracing, weather", L"Volume, speakers, sound", L"Gamepad, sight, guns",
                                    L"Free play, subtitles"};
const wchar_t* const TAB_MORE[] = {
    L"How the game starts: the PC version with the menu and every mode, or the cabinet as it was",
    L"Window or full screen, resolution, widescreen, vsync and the frame rate, the edge smoothing and the colours",
    L"The sun and its shadows, ray tracing, glow and rim light, the sky, haze, lightning and rain",
    L"The volume, the menu music, stereo, surround or headphones, the rear speakers and the weather",
    L"The gamepad's sticks, gyro and rumble, the aim assist, the crosshairs and the light guns",
    L"The arcade's own free play and the subtitles"};
const wchar_t* const RECORD_TAB_ABOUT[] = {L"Unlocked and still to get", L"Totals, best scores and times"};
const wchar_t* const RECORD_TAB_MORE[] = {L"Newest first, then the ones still to get with how far you are",
                                          L"What you have played, your best scores and your fastest stages and bosses"};
const wchar_t* const RECORD_GROUPS[] = {L"UNLOCKED", L"LOCKED", L"TOTALS", L"BEST SCORES", L"BEST STAGES", L"BOSSES", L"SURVIVAL"};
constexpr int RECORD_GROUP_COUNT = int(std::size(RECORD_GROUPS));
// The page's own tones, in the menus' amber (Sega Rally 2's are red and grey-blue)
constexpr uint32_t PAGE_TEXT = 0xF2EAD6, PAGE_DIM = 0x9C9280, PAGE_FAINT = 0x655E50, PAGE_LINE = 0x4A4234;
constexpr uint32_t GROUP = 0xF0A040, VALUE = 0xF2D07A, TAG = 0x3A2E1A, TAG_TEXT = 0xF0C890, ACCENT = 0xE8861C;
constexpr uint32_t ABOUT = 0xE2D8C4, LIT_ABOUT = 0xDCD2BE, POINTER = 0xFFE08A;

std::wstring title_case(const wchar_t* s) {   // DISPLAY -> Display
    std::wstring t = s;
    for (size_t i = 1; i < t.size(); i++)
        if (t[i - 1] != L' ') t[i] = wchar_t(towlower(t[i]));
    return t;
}
}  // namespace

Frontend::Family Frontend::page_font(const std::wstring& s) const {
    if (!jurassic_) return TITLE;
    for (wchar_t c : s)   // the Jurassic font has no digits
        if (c >= L'0' && c <= L'9') return TITLE;
    return JURASSIC;
}

void Frontend::ribbon(int x0, int y0, int x1, int y1, int slant, float a0, float a1, int gap, int sw, float alpha) {
    int h = std::max(1, y1 - y0);
    for (int y = y0; y < y1; y++) {
        int s = (y1 - 1 - y) * slant / h;
        hgrad(x0 + s, y, x1 + s, y + 1, ACCENT, a0 * alpha, a1 * alpha);
        if (sw > 0) fill(x0 + s - gap - sw, y, x0 + s - gap, y + 1, ACCENT, 0.9f * alpha);
    }
}

void Frontend::glide(Glide& g, long key, int y, int h, int& gy, int& gh) {
    constexpr double GLIDE = 0.13;   // seconds from one row to the next
    double now = menu_clock();
    auto at = [&](float& cy, float& ch) {
        float k = g.t0 < 0 ? 1.0f : float(std::clamp((now - g.t0) / GLIDE, 0.0, 1.0));
        float e = 1 - (1 - k) * (1 - k) * (1 - k);
        cy = g.from + (g.to - g.from) * e;
        ch = g.from_h + (g.to_h - g.from_h) * e;
        return k;
    };
    if (g.key != key) {   // another page (or size): straight there
        g = Glide{float(y), float(y), float(h), float(h), -1, key};
    } else if (float(y) != g.to || float(h) != g.to_h) {   // a new target: from where it is now (a fast repeat retargets)
        float cy, ch;
        at(cy, ch);
        g.from = cy, g.from_h = ch, g.to = float(y), g.to_h = float(h), g.t0 = now;
    }
    float cy, ch;
    if (at(cy, ch) >= 1) g.t0 = -1;
    else gliding_ = true;
    gy = int(std::lround(cy)), gh = int(std::lround(ch));
}

// The sections in a column at the left with what each holds, the open one's
// rows in groups under amber headings (they scroll when they do not all fit),
// a panel at the right with what the chosen row is and does, and the keys
// along the bottom. The same page shows the records, with nothing to change
void Frontend::draw_options_page(bool over_game, const std::vector<Item>& list) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    const int w = w_, h = h_, n = int(list.size());
    const int n_opts = n - 2;   // list: the sections, the rows of the open one, BACK
    const bool rec = screen_ == Screen::Records;
    const int tabs = tab_count();
    auto tab_label = [&](int t) { return rec ? RECORD_TAB_NAMES[t] : TAB_NAMES[t]; };
    auto tab_about = [&](int t) { return rec ? RECORD_TAB_ABOUT[t] : TAB_ABOUT[t]; };
    auto tab_more = [&](int t) { return rec ? RECORD_TAB_MORE[t] : TAB_MORE[t]; };
    // The page darkens what is behind it: the blurred backdrop, or the paused
    // game (nearly black: its HUD must not show through)
    fill(0, 0, w, h, over_game ? 0x000000 : PANEL, over_game ? 0.9f : 0.6f);

    // Columns: as wide as the window allows, up to 1728 units (1080p: a 96 pixel margin)
    const int mx = std::max(U(56), (w - U(1728)) / 2);
    const bool wide = float(w - 2 * mx) / u_ >= 1600;
    const int rail_w = U(wide ? 300.0f : 250.0f), gap = U(wide ? 48.0f : 32.0f), panel_w = U(wide ? 600.0f : 430.0f);
    const int rail_x = mx, rows_x0 = mx + rail_w + gap, panel_x0 = w - mx - panel_w, rows_x1 = panel_x0 - gap;
    const int top = U(196), bottom = h - U(100);   // the columns, between the title and the key help

    // Title, the amber mark under it, where this is, and the logo at the top right
    const std::wstring title = rec ? L"RECORDS" : over_game ? L"SETTINGS" : L"OPTIONS";
    text(display(), U(80), title, mx, U(34), 0, {GOLD_TOP, GOLD_BOTTOM, EDGE, U(3), 1, U(4)});
    fill(mx + U(2), U(134), mx + U(74), U(139), ACCENT, 1);
    std::wstring crumb = std::wstring(over_game ? L"Pause" : L"Main menu") + L"  ›  " +
                         (rec ? L"Records" : over_game ? L"Settings" : L"Options") + L"  ›  " + title_case(tab_label(tab_));
    text(ROW_MEDIUM, U(24), crumb, mx + U(92), U(120), 0, {PAGE_DIM, PAGE_DIM});
    if (logo_.w > 0) {
        int lw = U(wide ? 250.0f : 200.0f), lh = lw * logo_.h / logo_.w;
        sprite(logo_, w - mx - lw, U(18), lw, lh);
    }

    hit_.assign(size_t(n), RECT{});
    values_.assign(size_t(n), ValueArea{-1, 0, 0});

    // The sections, one under the other; the open one with a ribbon
    const int sec_h = U(92);
    const Style lit_name{WHITE_TOP, WHITE_BOTTOM, INK, U(2), 1, U(3)}, dim_name{PAGE_DIM, PAGE_DIM, INK, U(2)};
    tab_rects_.clear();
    {   // the open section's ribbon, gliding there from the last one
        int ty = top + tab_ * sec_h;
        int gy, gh;
        glide(glide_tab_, (long(screen_) * 2 + long(options_from_pause_)) * 100003 + h_, ty - U(8), sec_h - U(8), gy, gh);
        int sl = gh * 30 / 100;
        ribbon(rail_x - U(20) - sl / 2, gy, rail_x + rail_w, gy + gh, sl, 0.85f, 0.0f, U(10), U(8), 1);
        if (!gliding_) triangle(rail_x + U(8), ty + U(28), U(18), +1, POINTER, 0.95f);
    }
    for (int t = 0; t < tabs; t++) {
        int y = top + t * sec_h;
        bool open_tab = t == tab_;
        RECT r{rail_x - U(20), y - U(8), rail_x + rail_w, y + sec_h - U(16)};
        if (!open_tab && selected_ == 0 && hovered_ == 0 && mouse_y_ >= r.top && mouse_y_ < r.bottom)
            hgrad(r.left, r.top, r.right, r.bottom, 0xFFFFFF, 0.08f, 0.0f);   // under the pointer
        text(display(), U(44), tab_label(t), rail_x + U(26), y - U(6), 0, open_tab ? lit_name : dim_name);
        text(PROSE, U(17), tab_about(t), rail_x + U(28), y + U(46), 0,
             {open_tab ? LIT_ABOUT : PAGE_FAINT, open_tab ? LIT_ABOUT : PAGE_FAINT});
        tab_rects_.push_back(r);
    }
    hit_[0] = {rail_x - U(20), top - U(8), rail_x + rail_w, top + tabs * sec_h - U(16)};
    {   // BACK, under the sections
        int y = top + tabs * sec_h + U(28);
        bool sel = selected_ == n - 1;
        RECT r{rail_x - U(20), y - U(6), rail_x + rail_w, y + U(58)};
        if (sel) {
            int hh = r.bottom - r.top, sl = hh * 30 / 100;
            ribbon(r.left - sl / 2, r.top, r.right, r.bottom, sl, 0.85f, 0.0f, U(10), U(8), 1);
        }
        triangle(rail_x + U(20), y + U(26), U(16), -1, sel ? GOLD_BOTTOM : PAGE_DIM, 1);
        text(display(), U(40), L"BACK", rail_x + U(40), y - U(2), 0,
             sel ? Style{GOLD_TOP, GOLD_BOTTOM, EDGE, U(2)} : dim_name);
        hit_[size_t(n - 1)] = r;
        back_rect_ = r;
    }

    // The rows in their groups. Rows that do not fit scroll, keeping the chosen one in view
    const int row_h = U(46), head_h = U(38), head_gap = U(10), label_px = U(30);
    std::vector<const wchar_t*> group(size_t(std::max(0, n_opts)), nullptr);   // the heading over each row, if any
    std::vector<const wchar_t*> group_name(size_t(std::max(0, n_opts)), L"");  // the group each row is in
    const wchar_t* current = L"";
    for (int k = 0; k < n_opts; k++) {
        const Item& it = list[size_t(1 + k)];
        const wchar_t* g = rec ? (k > 0 && list[size_t(k)].index == it.index   // records: a heading where the group changes
                                      ? nullptr
                                      : RECORD_GROUPS[std::clamp(it.index, 0, RECORD_GROUP_COUNT - 1)])
                               : group_of(it.id);
        if (g) current = group[size_t(k)] = g;
        group_name[size_t(k)] = current;
    }
    auto height = [&](int k, bool first) {   // a row with its heading (the first one shown always has one)
        bool head = first ? *group_name[size_t(k)] != 0 : group[size_t(k)] != nullptr;
        return row_h + (head ? head_h + (first ? 0 : head_gap) : 0);
    };
    auto fits = [&](int from, int to) {
        int sum = 0;
        for (int k = from; k <= to; k++) sum += height(k, k == from);
        return sum <= bottom - top;
    };
    int k_sel = selected_ - 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, n_opts - 1));
    if (k_sel >= 0 && k_sel < n_opts) {
        if (k_sel < scroll_) scroll_ = k_sel;
        while (scroll_ < k_sel && !fits(scroll_, k_sel)) scroll_++;
    }
    while (scroll_ > 0 && fits(scroll_ - 1, n_opts - 1)) scroll_--;   // no room left empty at the bottom
    options_scroll_ = n_opts > 0 && (scroll_ > 0 || !fits(0, n_opts - 1));

    int cap0, cap1;
    ink_rows(ROW, label_px, L"H", cap0, cap1);   // the capitals centred in the row
    const Style gold{GOLD_TOP, GOLD_BOTTOM, EDGE, U(2)};
    bool row_ribbon = false;
    if (k_sel >= scroll_ && k_sel < n_opts) {   // the chosen row's ribbon, gliding there from the last one
        int ry = top, k = scroll_;
        for (; k <= k_sel; k++) {
            if (ry + height(k, k == scroll_) > bottom) break;
            ry += height(k, k == scroll_);
        }
        if (k > k_sel) {   // shown
            ry -= row_h;
            const Item& it = list[size_t(1 + k_sel)];
            int gy, gh;
            glide(glide_row_, (long(screen_) * 8 + long(tab_)) * 100003 + h_ + long(options_from_pause_) * 7, ry, row_h, gy, gh);
            int sl = row_h * 40 / 100;
            ribbon(rows_x0 - U(30) - sl / 2, gy, rows_x1 + U(18) - sl, gy + gh, sl, 0.30f, 0.58f, U(6), U(5),
                   it.enabled ? 1.0f : 0.4f);
            if (!gliding_) triangle(rows_x0 - U(16), ry + row_h / 2, U(14), +1, POINTER, it.enabled ? 0.95f : 0.4f);
            row_ribbon = true;
        }
    }
    if (!row_ribbon) glide_row_.key = -1;   // next time straight there
    int y = top, last_shown = scroll_ - 1;
    for (int k = scroll_; k < n_opts; k++) {
        if (y + height(k, k == scroll_) > bottom) break;
        const Item& it = list[size_t(1 + k)];
        const wchar_t* head = k == scroll_ ? (*group_name[size_t(k)] ? group_name[size_t(k)] : nullptr) : group[size_t(k)];
        if (head) {
            if (k != scroll_) y += head_gap;
            int gw = text(ROW_MEDIUM, U(20), head, rows_x0, y + U(6), 0, {GROUP, GROUP});
            fill(rows_x0 + gw + U(14), y + U(20), rows_x1, y + U(20) + std::max(1, U(1)), PAGE_LINE, 1);
            y += head_h;
        }
        bool sel = 1 + k == selected_;
        int cy = y + row_h / 2, ty = cy - (cap0 + cap1) / 2;
        uint32_t lc = it.enabled ? PAGE_TEXT : PAGE_FAINT;
        int lw = text(ROW, label_px, it.label, rows_x0, ty, 0,
                      sel && it.enabled ? Style{WHITE_TOP, WHITE_BOTTOM, 0, 0, 1, U(2)} : Style{lc, lc, 0, 0, 1, it.enabled ? U(2) : 0});
        auto tag = [&](const wchar_t* t, uint32_t back, uint32_t ink) {   // a small label after the name
            int tx = rows_x0 + lw + U(14), tw = measure(ROW, U(15), t) + U(16);
            fill(tx, cy - U(11), tx + tw, cy + U(11), back, 1);
            text(ROW, U(15), t, tx + tw / 2, cy - U(10), 1, {ink, ink});
        };
        if ((it.id == OPT_AA && aa_waits()) || (it.id == OPT_EXPERIENCE && settings.experience != (arcade ? 1 : 0)))
            tag(L"RESTART", 0x6A3414, 0xFFE0C8);   // changed, waiting for a new start
        else if (it.id == OPT_RT && it.enabled) tag(L"RT", TAG, TAG_TEXT);   // a graphics card with ray tracing
        int right = rows_x1 - U(20), vx0;
        uint32_t vc = it.enabled ? VALUE : PAGE_FAINT;
        const Style value_style = sel && it.enabled ? gold : Style{vc, vc, 0, 0, 1, it.enabled ? U(2) : 0};
        if (it.id == OPT_VOLUME) {   // ten segments
            int seg_w = U(15), sgap = U(5), seg_h = U(20);
            int lit_n = int(it.frac * 10 + 0.5f);
            vx0 = right - 10 * seg_w - 9 * sgap;
            for (int s = 0; s < 10; s++) {
                int x0 = vx0 + s * (seg_w + sgap);
                bool on = s < lit_n;
                uint32_t c = on ? mix(0xFFE8A8, ACCENT, float(s) / 9) : 0x3A3428;
                fill(x0 - 1, cy - seg_h / 2 - 1, x0 + seg_w + 1, cy + seg_h / 2 + 1, 0x000000, 0.8f);
                fill(x0, cy - seg_h / 2, x0 + seg_w, cy + seg_h / 2, c, sel || !on ? 1.0f : 0.8f);
            }
        } else if (it.kind == SLIDER) {   // a bar, then its value (the bars lined up)
            text(ROW, label_px, it.value, right, ty, 2, value_style);
            int bx1 = right - measure(ROW, label_px, L"200%") - U(18), bx0 = bx1 - U(wide ? 230.0f : 150.0f);
            int bh = U(12), f = bx0 + int(float(bx1 - bx0) * std::clamp(it.frac, 0.0f, 1.0f) + 0.5f);
            uint32_t bar = !it.enabled ? 0x5A4A30 : sel ? 0xFFB040 : ACCENT;
            fill(bx0 - 1, cy - bh / 2 - 1, bx1 + 1, cy + bh / 2 + 1, 0x000000, 0.8f);
            fill(bx0, cy - bh / 2, bx1, cy + bh / 2, 0x3A3428, 1);
            fill(bx0, cy - bh / 2, f, cy + bh / 2, bar, 1);
            fill(bx0, cy - bh / 2, f, cy - bh / 2 + std::max(1, bh / 4), 0xFFE0A0, it.enabled ? 0.35f : 0.1f);   // a light along its top
            vx0 = bx0;
        } else {
            vx0 = right - text(ROW, label_px, it.value, right, ty, 2, value_style);
        }
        bool changes = !rec && it.kind != BUTTON;   // (a record, and a button, have nothing to step through)
        if (sel && it.enabled && changes) {
            triangle(vx0 - U(20), cy, U(14), -1, GOLD_BOTTOM, 1);
            triangle(right + U(13), cy, U(14), +1, GOLD_BOTTOM, 1);
        }
        hit_[size_t(1 + k)] = {rows_x0 - U(18), y, rows_x1 + U(4), y + row_h};
        if (changes) values_[size_t(1 + k)] = {vx0 - U(40), vx0 - U(4), right + U(2)};
        y += row_h;
        last_shown = k;
    }
    if (scroll_ > 0) {   // more rows above / below: arrows past the right end of the rows
        triangle(rows_x1 + U(30), top + U(14), U(18), -2, GOLD_BOTTOM, 1);
        more_up_ = {rows_x1 + U(10), top - U(8), rows_x1 + U(52), top + U(36)};
    }
    if (last_shown < n_opts - 1) {
        triangle(rows_x1 + U(30), bottom - U(14), U(18), 2, GOLD_BOTTOM, 1);
        more_down_ = {rows_x1 + U(10), bottom - U(36), rows_x1 + U(52), bottom + U(8)};
    }

    // The panel at the right: what the chosen row is and does
    const int px1 = w - mx, pad = U(wide ? 36.0f : 26.0f);
    fill(panel_x0, top - U(8), px1, bottom, PANEL, over_game ? 0.95f : 0.62f);
    frame(panel_x0, top - U(8), px1, bottom, std::max(1, U(1)), PAGE_LINE, 1);
    fill(panel_x0, top - U(8), px1, top - U(8) + std::max(2, U(4)), ACCENT, 1);
    if (selected_ >= 0 && selected_ < n) {
        const Item& it = list[size_t(selected_)];
        bool section = it.id == TABS, back = it.id == BACK;
        int x = panel_x0 + pad, py = top + U(22), inner = px1 - pad - x;
        const wchar_t* kicker = section ? L"SECTION" : back || n_opts <= 0 ? L"" : group_name[size_t(std::clamp(selected_ - 1, 0, n_opts - 1))];
        if (*kicker) text(ROW_MEDIUM, U(20), kicker, x, py, 0, {GROUP, GROUP});
        py += U(30);
        std::wstring name = section ? std::wstring(tab_label(tab_)) : it.label;
        Family nf = page_font(name);
        int name_px = U(60);
        while (name_px > U(36) && measure(nf, name_px, name) > inner) name_px -= U(4);
        text(nf, name_px, name, x, py, 0, {WHITE_TOP, WHITE_BOTTOM, INK, U(2), 1, U(3)});
        py += name_px + U(22);
        if (it.id == OPT_VOLUME) {
            text(ROW, U(34), std::to_wstring(settings.volume) + L" / 10", x, py, 0, gold);
            py += U(52);
        } else if (!section && !back && !it.value.empty()) {
            text(ROW, U(34), it.value, x, py, 0, it.enabled ? gold : Style{PAGE_FAINT, PAGE_FAINT});
            py += U(52);
        }
        fill(x, py, px1 - pad, py + std::max(1, U(1)), PAGE_LINE, 1);
        py += U(24);
        int text_end = bottom - U(16);
        if (it.id == REC_ROW && it.index <= 1) {   // an achievement: its badge at the bottom
            const Picture& badge = achievement_badge(it.enabled);
            if (badge.w > 0) {
                int s = U(120);
                sprite(badge, x, bottom - U(24) - s, s, s);
                text_end = bottom - U(24) - s - U(20);
            }
        }
        std::wstring about = section ? std::wstring(tab_more(tab_)) + L".  " + it.hint : it.hint;
        for (const std::wstring& line : wrap(PROSE, U(24), about, inner)) {
            if (py + U(34) > text_end) break;
            text(PROSE, U(24), line, x, py, 0, {ABOUT, ABOUT});
            py += U(34);
        }
    }

    // The key help along the bottom
    fill(0, h - U(76), w, h, 0x000000, 0.5f);
    fill(0, h - U(76), w, h - U(76) + std::max(1, U(1)), PAGE_LINE, 1);
    struct Key { const wchar_t *pad, *key, *what; };
    std::vector<Key> keys;
    if (pad_prompts) {
        keys = {{L"LS", nullptr, rec ? L"SELECT" : L"SELECT / CHANGE"}};
        if (!rec) keys.push_back({L"A", nullptr, L"LIST"});
        keys.insert(keys.end(), {{L"LB", nullptr, L""}, {L"RB", nullptr, L"SECTION"}});
        if (!rec) keys.push_back({L"Y", nullptr, L"RESET TO DEFAULTS"});
        keys.push_back({L"B", nullptr, L"BACK"});
    } else {
        keys = {{nullptr, L"UD", L"SELECT"}};
        if (!rec) keys.push_back({nullptr, L"<>", L"CHANGE"});
        if (!rec) keys.push_back({nullptr, L"ENTER", L"LIST"});
        keys.push_back({nullptr, L"Q / E", L"SECTION"});
        if (!rec) keys.push_back({nullptr, L"R", L"RESET TO DEFAULTS"});
        keys.push_back({nullptr, L"ESC", L"BACK"});
    }
    int kx = mx, ky = h - U(38) - U(19);
    for (const Key& k : keys) {
        RECT r = prompt(kx, ky, k.pad, k.key, k.what);
        if (k.what == std::wstring(L"RESET TO DEFAULTS")) reset_rect_ = r;
        if (k.what == std::wstring(L"BACK")) back_rect_ = r;
    }
}

// The achievements' badges (res/achievements, as the pop-up's): lit, or locked
const Frontend::Picture& Frontend::achievement_badge(bool lit) {
    Picture& p = badges_[lit ? 0 : 1];
    if (!badges_loaded_[lit ? 0 : 1]) {
        badges_loaded_[lit ? 0 : 1] = true;
        load_image(lit ? L"res/achievements/unlocked.png" : L"res/achievements/locked.png", p.px, p.w, p.h);
    }
    return p;
}
