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

// ---- Splash screens ----

namespace {
constexpr double SPLASH_GAP = 0.25;    // black before each one
constexpr double SPLASH_IN = 0.7;      // fading in
constexpr double SPLASH_HOLD = 1.8;    // on screen
constexpr double SPLASH_OUT = 0.7;     // fading out
constexpr double SPLASH_SKIP = 0.25;   // fading out when hurried
}  // namespace

void Frontend::skip_splash() {
    if (video_on_) {   // faded out with its sound, unless it is going anyway
        if (skip_from_ < 0 && video_end_ < 0) skip_level_ = video_level_, skip_from_ = now_seconds();
        return;
    }
    if (skip_from_ >= 0) {   // already going: the next one goes too
        skip_next_ = true;
        return;
    }
    double t = now_seconds() - splash_from_;
    if (t >= SPLASH_GAP + SPLASH_IN + SPLASH_HOLD) return;   // fading out anyway
    skip_level_ = float(std::clamp((t - SPLASH_GAP) / SPLASH_IN, 0.0, 1.0));
    skip_from_ = now_seconds();
}

bool Frontend::draw_splash(std::vector<uint32_t>& out) {
    if (video_on_) return draw_splash_video(out);
    double now = now_seconds();
    float level = 0;   // brightness: 0 black .. 1 the picture
    for (;;) {
        double t = now - splash_from_;
        bool ended;
        if (skip_from_ >= 0) {
            double k = (now - skip_from_) / SPLASH_SKIP;
            level = skip_level_ * float(1 - std::min(k, 1.0));
            ended = k >= 1;
        } else {
            double out_at = SPLASH_GAP + SPLASH_IN + SPLASH_HOLD;
            level = float(std::clamp(std::min((t - SPLASH_GAP) / SPLASH_IN, 1 - (t - out_at) / SPLASH_OUT), 0.0, 1.0));
            ended = t >= out_at + SPLASH_OUT;
        }
        if (!ended) break;
        if (++splash_on_ >= int(splash_.size())) return false;
        splash_from_ = now;
        skip_from_ = -1;
        if (skip_next_) {   // pressed twice: this one is gone before it shows
            skip_next_ = false;
            skip_from_ = now - SPLASH_SKIP;
            skip_level_ = 0;
        }
    }
    // The picture as big as fits the window, white around it (a wider or
    // taller window than the picture), scaled once per size
    const Picture& p = splash_[splash_on_];
    if (splash_px_on_ != splash_on_ || splash_px_w_ != w_ || splash_px_h_ != h_) {
        splash_px_.assign(size_t(w_) * h_, 0xFFFFFFFFu);
        float sc = std::min(float(w_) / p.w, float(h_) / p.h);
        int pw = std::max(1, int(p.w * sc + 0.5f)), ph = std::max(1, int(p.h * sc + 0.5f));
        int x0 = (w_ - pw) / 2, y0 = (h_ - ph) / 2;
        px_ = splash_px_.data();
        picture(p, x0, y0, x0 + pw, y0 + ph);   // the box has its shape: nothing cropped
        splash_px_on_ = splash_on_, splash_px_w_ = w_, splash_px_h_ = h_;
    }
    out.resize(splash_px_.size());
    uint32_t k = uint32_t(level * 256 + 0.5f);
    if (k >= 256) out = splash_px_;
    else
        for (size_t i = 0; i < out.size(); i++) {   // towards black, opaque
            uint32_t c = splash_px_[i];
            out[i] = 0xFF000000u | ((((c & 0xFF00FFu) * k) >> 8) & 0xFF00FFu) | ((((c & 0x00FF00u) * k) >> 8) & 0x00FF00u);
        }
    px_ = out.data();
    return true;
}

// The video: in from black as it starts, played to the end (Windows plays the
// sound), its last picture faded out. Hurried: picture and sound fade out
// together. As big as fits the window, its own background colour around it
bool Frontend::draw_splash_video(std::vector<uint32_t>& out) {
    double now = now_seconds(), t = now - splash_from_;
    if (video_.failed() || (t > 5 && video_.position() <= 0)) return false;   // it will not play: to the menu
    if (video_end_ < 0 && video_.ended()) video_end_ = now;
    float level = float(std::clamp(t / SPLASH_IN, 0.0, 1.0));
    if (skip_from_ >= 0) {
        double k = std::min((now - skip_from_) / SPLASH_SKIP, 1.0);
        if (k >= 1) return false;
        level = skip_level_ * float(1 - k);
        video_.set_volume(float(settings.volume) / 10.0f * float(1 - k));
    } else if (video_end_ >= 0) {
        double k = (now - video_end_) / SPLASH_OUT;
        if (k >= 1) return false;
        level = std::min(level, float(1 - k));
    }
    video_level_ = level;

    out.assign(size_t(w_) * h_, 0xFF000000u);
    px_ = out.data();
    int vw = 0, vh = 0;
    if (!video_.size(vw, vh)) return true;   // still loading: black
    float sc = std::min(float(w_) / vw, float(h_) / vh);
    int pw = std::max(1, int(vw * sc + 0.5f)), ph = std::max(1, int(vh * sc + 0.5f));
    bool resized = pw != video_pw_ || ph != video_ph_;
    if (video_.frame(pw, ph, video_px_, resized)) video_pw_ = pw, video_ph_ = ph;
    if (video_px_.size() != size_t(video_pw_) * video_ph_ || video_pw_ <= 0 || video_pw_ > w_ || video_ph_ > h_) return true;
    uint32_t k = uint32_t(level * 256 + 0.5f);
    auto dim = [k](uint32_t c) {   // towards black, opaque
        if (k >= 256) return c | 0xFF000000u;
        return 0xFF000000u | ((((c & 0xFF00FFu) * k) >> 8) & 0xFF00FFu) | ((((c & 0x00FF00u) * k) >> 8) & 0x00FF00u);
    };
    uint32_t around = dim(video_px_[video_pw_ / 2]);   // the middle of its top edge: its background
    int x0 = (w_ - video_pw_) / 2, y0 = (h_ - video_ph_) / 2;
    for (int y = 0; y < h_; y++) {
        uint32_t* row = &out[size_t(y) * w_];
        if (y < y0 || y >= y0 + video_ph_) {
            std::fill(row, row + w_, around);
            continue;
        }
        const uint32_t* src = &video_px_[size_t(y - y0) * video_pw_];
        std::fill(row, row + x0, around);
        for (int x = 0; x < video_pw_; x++) row[x0 + x] = dim(src[x]);
        std::fill(row + x0 + video_pw_, row + w_, around);
    }
    return true;
}

// SURVIVAL with every arena locked: how they unlock, over the list
void Frontend::draw_locked_notice() {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    fill(0, 0, w_, h_, 0x000000, 0.55f);
    int pw = U(900), ph = U(400), x0 = (w_ - pw) / 2, y0 = (h_ - ph) / 2, cx = w_ / 2;
    fill(x0, y0, x0 + pw, y0 + ph, PANEL, 0.96f);
    frame(x0, y0, x0 + pw, y0 + ph, std::max(1, U(2)), AMBER, 0.9f);
    text(display(), U(64), L"ARENAS LOCKED", cx, y0 + U(36), 1, {GOLD_TOP, GOLD_BOTTOM, EDGE, U(3), 1, U(4)});
    int y = y0 + U(140);
    for (const std::wstring& line : wrap(BODY, U(26),
             L"Each survival arena opens when you reach it in the arcade game. Play CLASSIC ARCADE: "
             L"when you get to one, ARENA UNLOCKED shows up and it is ready here.", pw - U(100))) {
        text(BODY, U(26), line, cx, y, 1, {0xE8C890, 0xE8C890});
        y += U(38);
    }
    text(ITEM, U(22), L"PRESS ENTER OR CLICK", cx, y0 + ph - U(56), 1, {HINT, HINT});
}

// RESTART NEEDED: a card over the darkened settings naming what waits for
// a new start, the two buttons and what each does
void Frontend::draw_restart() {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int w = w_, h = h_, cx = w / 2;
    fill(0, 0, w, h, 0x000000, 0.62f);
    int half = std::min(U(520), w / 2 - U(16));
    std::wstring what = restore_ask_ ? std::wstring(L"The arcade's original look and sound: no anti-aliasing or new effects, the "
                                                    L"original sky, letters and spotlight, 4:3, FREE PLAY.  The screen, the "
                                                    L"speakers, the controls and the crosshairs stay as they are")
                        : !restart_note_.empty() ? restart_note_
                                                 : restart_list() + L" needs a restart";
    std::vector<std::wstring> lines = wrap(BODY, U(28), what, 2 * half - U(120));
    if (over_game() && !restore_ask_) lines.push_back(L"");   // (the warning below)
    int bh = U(66);
    int card_h = U(150) + int(lines.size()) * U(40) + U(30) + bh + U(26) + U(30) + U(34);
    int top = std::max(U(16), (h - card_h) / 2), bottom = top + card_h;
    fill(cx - half, top, cx + half, bottom, PANEL, 0.96f);
    frame(cx - half, top, cx + half, bottom, std::max(1, U(2)), AMBER, 0.9f);
    text(display(), U(64), restore_ask_ ? L"RESTORE ORIGINAL" : L"RESTART NEEDED", cx, top + U(36), 1, {GOLD_TOP, GOLD_BOTTOM, EDGE, U(3), 1, U(4)});
    int y = top + U(150);
    for (size_t i = 0; i < lines.size(); i++, y += U(40)) {
        if (over_game() && !restore_ask_ && i + 1 == lines.size())
            text(ITEM, U(24), L"THE GAME IN PROGRESS WILL BE LOST.  LATER KEEPS IT", cx, y + U(4), 1, {0xFF9A8E, 0xFF9A8E});
        else text(BODY, U(28), lines[i], cx, y, 1, {0xE8C890, 0xE8C890});
    }
    // The buttons side by side, the chosen one lit
    const wchar_t* const LABELS[2] = {restore_ask_ ? L"RESTORE" : L"RESTART NOW", restore_ask_ ? L"CANCEL" : L"LATER"};
    int bw = std::min(U(340), half - U(40)), gap = U(40), by = y + U(30);
    int ink0 = INT_MAX, ink1 = INT_MIN;   // both labels' letters, centred in the boxes whatever the font
    for (const wchar_t* l : LABELS) {
        int t, b;
        ink_rows(display(), U(40), l, t, b);
        ink0 = std::min(ink0, t), ink1 = std::max(ink1, b);
    }
    int label_y = by + bh / 2 - (ink0 + ink1) / 2;
    for (int b = 0; b < 2; b++) {
        int x0 = b == 0 ? cx - gap / 2 - bw : cx + gap / 2;
        restart_buttons_[b] = {x0, by, x0 + bw, by + bh};
        bool sel = b == restart_button_;
        fill(x0, by, x0 + bw, by + bh, sel ? AMBER : 0x000000, sel ? 0.28f : 0.45f);
        frame(x0, by, x0 + bw, by + bh, std::max(1, U(sel ? 3.0f : 1.5f)), sel ? WHITE_TOP : AMBER, sel ? 1.0f : 0.55f);
        const Style st = sel ? Style{GOLD_TOP, GOLD_BOTTOM, EDGE, U(2), 1, U(2)} : Style{PALE_TOP, PALE_BOTTOM, INK, U(2), 0.95f};
        text(display(), U(40), LABELS[b], x0 + bw / 2, label_y, 1, st);
    }
    const wchar_t* about = restore_ask_         ? (restart_button_ == 0 ? L"Puts the original settings back now" : L"Back to the pause")
                           : restart_button_ == 0 ? L"Saves the settings, closes the game and starts it again"
                                                  : L"Back as before.  The change applies the next time the game starts";
    text(ITEM, U(22), about, cx, by + bh + U(26), 1, {HINT, HINT});
}

// A setting's list: a card over the darkened settings with the row's name and its
// values (as many as fit, arrows for the rest), the one in use marked
void Frontend::draw_drop() {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int w = w_, h = h_, cx = w / 2;
    fill(0, 0, w, h, 0x000000, 0.62f);
    int n = int(drop_.size());
    int half = std::min(U(360), w / 2 - U(16)), row = U(50), head = U(130), foot = U(80);
    int shown = std::max(1, std::min(n, (h - U(32) - head - foot) / row));
    if (drop_sel_ < drop_top_) drop_top_ = drop_sel_;
    if (drop_sel_ >= drop_top_ + shown) drop_top_ = drop_sel_ - shown + 1;
    drop_top_ = std::clamp(drop_top_, 0, std::max(0, n - shown));
    int card_h = head + shown * row + foot;
    int top = std::max(U(16), (h - card_h) / 2), bottom = top + card_h;
    fill(cx - half, top, cx + half, bottom, PANEL, 0.96f);
    frame(cx - half, top, cx + half, bottom, std::max(1, U(2)), AMBER, 0.9f);
    std::wstring title;
    for (const Item& it : option_items(settings))
        if (it.id == drop_id_) title = it.label;
    text(display(), U(56), title, cx, top + U(32), 1, {GOLD_TOP, GOLD_BOTTOM, EDGE, U(3), 1, U(4)});
    drop_rects_.clear();
    int row_px = U(30);
    for (int k = 0; k < shown && drop_top_ + k < n; k++) {
        int i = drop_top_ + k, y = top + head + k * row;
        bool sel = i == drop_sel_;
        if (sel) band(cx, y, y + row, half - U(24), AMBER, 0.22f);
        int ty = y + (row - row_px) / 2 - U(4);
        text(ITEM, row_px, drop_[size_t(i)].label, cx - half + U(60), ty, 0,
             sel ? Style{GOLD_TOP, GOLD_BOTTOM, EDGE, U(3)} : Style{PALE_TOP, PALE_BOTTOM, INK, U(2), 0.95f});
        if (i == drop_in_use_) text(ITEM, U(22), L"IN USE", cx + half - U(60), y + (row - U(22)) / 2 - U(2), 2, {AMBER, AMBER});
        drop_rects_.push_back({cx - half, y, cx + half, y + row});
    }
    if (drop_top_ > 0) triangle(cx, top + head - U(18), U(18), -2, GOLD_BOTTOM, 1);
    if (drop_top_ + shown < n) triangle(cx, top + head + shown * row + U(12), U(18), 2, GOLD_BOTTOM, 1);
    const wchar_t* about = pad_prompts ? L"A: use it at once.  B: close" : L"Enter: use it at once.  Esc: close";
    text(ITEM, U(22), about, cx, bottom - U(46), 1, {HINT, HINT});
}
