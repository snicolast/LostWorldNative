#include "survival_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

// Same colours as the menus (ui/frontend_style.h): the game's yellow-to-orange letters
constexpr uint32_t GOLD_TOP = 0xFFEA5C, GOLD_BOTTOM = 0xFF8A00;
constexpr uint32_t PALE_TOP = 0xE6DCC0, PALE_BOTTOM = 0xBFB294;
constexpr uint32_t BRIGHT_GOLD = 0xFFF27A;   // the golden BONUS target
constexpr uint32_t AMBER = 0xFFA032, FOOT = 0x8A8070, RED_TOP = 0xFF7A5C, RED_BOTTOM = 0xD02000;

std::wstring num(int v) { return std::to_wstring(v); }

}  // namespace

bool SurvivalUi::init(const std::string& font_dir) {
    gdi_ = &GdiText::acquire(font_dir);   // the fonts of the menus
    jurassic_ = gdi_->loaded(GdiText::JURASSIC);
    return gdi_->ready();
}

void SurvivalUi::shutdown() {
    if (gdi_) gdi_->release();
    gdi_ = nullptr;
}

// Dark box with a thin amber frame
void SurvivalUi::panel(int x0, int y0, int x1, int y1, float alpha) {
    fill(x0, y0, x1, y1, 0x0C0A06, alpha);
    int t = std::max(1, int(3 * u_));
    fill(x0, y0, x1, y0 + t, AMBER, 0.8f * alpha);
    fill(x0, y1 - t, x1, y1, AMBER, 0.8f * alpha);
    fill(x0, y0, x0 + t, y1, AMBER, 0.35f * alpha);
    fill(x1 - t, y0, x1, y1, AMBER, 0.35f * alpha);
}

int SurvivalUi::text(Family f, int px, const std::wstring& s, int x, int y, int align, uint32_t top,
                     uint32_t bottom, float alpha) {
    if (f == TITLE && jurassic_ && std::any_of(s.begin(), s.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        f = NUMBER;   // its digits are blank strokes
    GdiText::Face face = f == TITLE ? (jurassic_ ? GdiText::JURASSIC : GdiText::SAIRA)
                       : f == NUMBER ? GdiText::SAIRA : GdiText::BARLOW;
    return shadowed_text(gdi_->mask(face, px, s), px, x, y, align, top, bottom, alpha);
}

void SurvivalUi::bar(int x0, int y0, int x1, int y1, float fraction) {
    fill(x0, y0, x1, y1, 0x000000, 0.6f);
    int t = std::max(1, int(2 * u_));
    fill(x0 + t, y0 + t, x0 + t + int(float(x1 - x0 - 2 * t) * std::clamp(fraction, 0.0f, 1.0f)), y1 - t, AMBER, 0.9f);
}

void SurvivalUi::draw_intro(const SurvivalView& v, double now) {
    if (v.mode == SurvivalMode::Loading) fill(0, 0, w_, h_, 0x050403, 1.0f);   // hides the way in
    else fill(0, 0, w_, h_, 0x000000, 0.45f);
    auto U = [&](float a) { return int(a * u_ + 0.5f); };
    int cx = w_ / 2, top = h_ / 2 - U(320);
    panel(cx - U(560), top, cx + U(560), top + U(640), 0.85f);
    text(TITLE, U(120), L"SURVIVAL", cx, top + U(30), 1, GOLD_TOP, GOLD_BOTTOM);
    const wchar_t* name = survival_zone_name(v.stage, v.zone);
    std::wstring where = L"STAGE " + num(v.stage) + L"   \u00B7   ZONE " + num(v.zone);
    if (name) where += L"   \u00B7   " + std::wstring(name);
    text(TEXT, U(40), where, cx, top + U(170), 1, PALE_TOP, PALE_BOTTOM);
    const wchar_t* rules[5] = {L"The park takes you through the zone, stop after stop: survive every stop.",
                               L"Keep your eyes open on the way: dinosaurs can ambush you.",
                               L"Shoot the golden BONUS target when it shows up, for an extra life.",
                               L"After the last stop, a new lap begins. Every lap is harder.",
                               L"No continues. When your last life is gone, the hunt is over."};
    for (int i = 0; i < 5; i++)
        text(TEXT, U(31), rules[i], cx, top + U(228.0f + 40.0f * float(i)), 1, PALE_TOP, PALE_BOTTOM, 0.9f);
    std::wstring rec = v.best_wave <= 0 ? std::wstring(L"NO RECORD YET \u2014 SET THE FIRST ONE")
                     : (v.best_lap > 0 ? L"YOUR RECORD:  LAP " + num(v.best_lap) + L"   \u00B7   " : std::wstring(L"YOUR RECORD:  ")) +
                           L"WAVE " + num(v.best_wave) + L"   \u00B7   " + num(v.best_score) + L" POINTS";
    text(TEXT, U(40), rec, cx, top + U(v.mode == SurvivalMode::Loading ? 425 : 435), 1, AMBER, AMBER);
    if (v.mode == SurvivalMode::Loading) {   // what is going on while the game gets there
        std::wstring dots(size_t(int(now * 3) % 4), L'.');
        std::wstring what = v.step == SurvivalStep::Coin     ? L"Inserting a coin"
                          : v.step == SurvivalStep::Course   ? L"Choosing the course"
                          : v.step == SurvivalStep::Travelling ? L"Travelling to " + std::wstring(name ? name : L"the arena")
                                                             : L"Getting the park ready";
        text(TITLE, U(46), L"PREPARING THE HUNT", cx, top + U(485), 1, GOLD_TOP, GOLD_BOTTOM);
        bar(cx - U(380), top + U(540), cx + U(380), top + U(556), v.progress);
        text(TEXT, U(30), what + dots, cx, top + U(566), 1, FOOT, FOOT);
        if (v.step == SurvivalStep::Travelling)   // or it looks stuck
            text(TEXT, U(24), L"The game is racing through the stage, muted, to reach the arena. This can take a moment.",
                 cx, top + U(602), 1, FOOT, FOOT, 0.9f);
    } else {
        if (int(now * 2.5) % 2 == 0) text(TITLE, U(64), L"SHOOT TO START", cx, top + U(510), 1, GOLD_TOP, GOLD_BOTTOM);
        text(TEXT, U(28), L"ESC: PAUSE", cx, top + U(596), 1, FOOT, FOOT);
    }
}

void SurvivalUi::draw_hud(const SurvivalView& v, double now) {
    auto U = [&](float a) { return int(a * u_ + 0.5f); };
    text(TITLE, U(56), L"LAP " + num(v.lap) + L"   \u00B7   WAVE " + num(v.wave), w_ / 2, U(14), 1, GOLD_TOP, GOLD_BOTTOM,
         0.9f);
    double rt = now - v.reward_at;   // the BONUS target's reward
    if (v.reward && rt >= 0 && rt < 2.5) {
        float a = rt < 1.8 ? 1.0f : float((2.5 - rt) / 0.7);
        text(TITLE, U(110), v.reward == 1 ? L"+1 LIFE" : L"+" + num(5000), w_ / 2, h_ / 2 + U(60), 1, BRIGHT_GOLD, GOLD_BOTTOM, a);
    }
    if (v.total > 0 && !v.resting)
        text(TEXT, U(32), L"DINOSAURS " + num(v.left) + L" / " + num(v.total), w_ / 2, U(80), 1, PALE_TOP, PALE_BOTTOM, 0.9f);
    // The countdown before a wave (and a new lap's title over it)
    double t = now - v.wave_started, lt = now - v.lap_started;
    if (!v.resting && !v.ambush && t >= 0 && t < SURVIVAL_COUNTDOWN + 1.0 && v.fade < 0.5f) {
        bool new_lap = v.lap > 1 && lt >= 0 && lt < SURVIVAL_COUNTDOWN + 1.0;
        std::wstring title = new_lap ? L"LAP " + num(v.lap) + L"   \u00B7   WAVE " + num(v.wave) : L"WAVE " + num(v.wave);
        if (t < SURVIVAL_COUNTDOWN) {
            int digit = int(std::ceil(SURVIVAL_COUNTDOWN - t));
            float pop = float(t - std::floor(t));   // each digit starts big and settles
            text(TITLE, U(110), title, w_ / 2, h_ / 2 - U(330), 1, GOLD_TOP, GOLD_BOTTOM);
            text(NUMBER, U(300.0f - 60.0f * std::min(pop * 4, 1.0f)), num(digit), w_ / 2, h_ / 2 - U(190), 1, GOLD_TOP,
                 GOLD_BOTTOM, 0.6f + 0.4f * std::min(pop * 4, 1.0f));
            text(TEXT, U(44), new_lap ? L"BACK TO THE START. THEY ARE ANGRIER NOW!" : L"GET READY", w_ / 2,
                 h_ / 2 + U(120), 1, PALE_TOP, PALE_BOTTOM);
        } else {
            float a = float(1.0 - (t - SURVIVAL_COUNTDOWN));
            text(TITLE, U(220), L"GO!", w_ / 2, h_ / 2 - U(170), 1, GOLD_TOP, GOLD_BOTTOM, a);
        }
        return;
    }
    if (v.ambush) {
        double at = now - v.ambush_started;
        if (at >= 0 && at < 2.5)
            text(TITLE, U(150), L"AMBUSH!", w_ / 2, h_ / 2 - U(180), 1, RED_TOP, RED_BOTTOM, at < 1.8 ? 1.0f : float((2.5 - at) / 0.7));
        return;
    }
    if (v.resting) {   // the stop is cleared: the pause, then the walk on
        if (v.fade > 0) return;
        text(TITLE, U(96), v.zone_done ? std::wstring(L"ZONE CLEARED") : L"WAVE " + num(v.wave) + L" CLEARED", w_ / 2,
             U(150), 1, GOLD_TOP, GOLD_BOTTOM);
        if (v.walking)
            text(TEXT, U(40), L"ON THE MOVE...  STAY SHARP", w_ / 2, U(272), 1, PALE_TOP, PALE_BOTTOM);
        else if (v.zone_done)
            text(TEXT, U(40), L"NEXT: LAP " + num(v.lap + 1), w_ / 2, U(272), 1, PALE_TOP, PALE_BOTTOM);
        else
            text(TEXT, U(40), L"MOVING ON IN " + num(std::max(int(std::ceil(v.rest_left)), 1)), w_ / 2, U(272), 1, PALE_TOP,
                 PALE_BOTTOM);
        return;
    }
}

void SurvivalUi::ring(float x, float y, float r, float width, uint32_t rgb, float alpha) {
    int x0 = int(x - r - width - 1), x1 = int(x + r + width + 1), y0 = int(y - r - width - 1), y1 = int(y + r + width + 1);
    for (int j = y0; j <= y1; j++)
        for (int i = x0; i <= x1; i++) {
            float d = std::fabs(std::hypot(float(i) - x, float(j) - y) - r);
            float a = std::clamp(width / 2 + 0.5f - d, 0.0f, 1.0f);
            if (a > 0) blend(i, j, rgb, a * alpha);
        }
}

// The golden BONUS target: rings over the game screen, a shrinking one for the time left
void SurvivalUi::draw_bonus(const SurvivalView& v, const SurvivalLayout& lay, double now) {
    if (!v.bonus_on) return;
    float x = lay.ox + (lay.game_x0 + v.bonus_x * 496.0f) * lay.scale, y = lay.oy + v.bonus_y * 384.0f * lay.scale;
    float r = 26.0f * lay.scale;   // about what the game takes as a hit (BONUS_HIT, survival.cpp)
    float pulse = 1.0f + 0.08f * float(std::sin(now * 9.0));
    ring(x, y, r * pulse + 4 * u_, 7 * u_, 0x000000, 0.5f);
    ring(x, y, r * pulse, 6 * u_, BRIGHT_GOLD, 0.95f);
    ring(x, y, r * 0.55f, 4 * u_, GOLD_TOP, 0.9f);
    ring(x, y, r * 0.15f, 5 * u_, BRIGHT_GOLD, 1.0f);
    ring(x, y, r * (1.25f + 0.6f * v.bonus_left), 3 * u_, BRIGHT_GOLD, 0.5f);
    text(TITLE, int(44 * u_), L"BONUS", int(x), int(y - r - 60 * u_), 1, BRIGHT_GOLD, GOLD_BOTTOM);
}

void SurvivalUi::draw_results(const SurvivalView& v, double now) {
    fill(0, 0, w_, h_, 0x000000, 0.6f);
    auto U = [&](float a) { return int(a * u_ + 0.5f); };
    int cx = w_ / 2, top = h_ / 2 - U(320);
    panel(cx - U(560), top, cx + U(560), top + U(640), 0.9f);
    text(TITLE, U(110), L"THE HUNT IS OVER", cx, top + U(30), 1, RED_TOP, RED_BOTTOM);
    text(TEXT, U(44), L"STAGE " + num(v.stage) + L"   ·   ZONE " + num(v.zone), cx, top + U(165), 1, PALE_TOP,
         PALE_BOTTOM, 0.8f);
    text(TEXT, U(42), L"LAP", cx - U(40), top + U(222), 2, PALE_TOP, PALE_BOTTOM);
    text(TITLE, U(60), num(v.lap), cx + U(40), top + U(210), 0, GOLD_TOP, GOLD_BOTTOM);
    text(TEXT, U(42), L"WAVE REACHED", cx - U(40), top + U(282), 2, PALE_TOP, PALE_BOTTOM);
    text(TITLE, U(60), num(v.wave), cx + U(40), top + U(270), 0, GOLD_TOP, GOLD_BOTTOM);
    text(TEXT, U(42), L"SCORE", cx - U(40), top + U(342), 2, PALE_TOP, PALE_BOTTOM);
    text(TITLE, U(60), num(v.score), cx + U(40), top + U(330), 0, GOLD_TOP, GOLD_BOTTOM);
    bool blink = int(now * 2.5) % 2 == 0;
    if (v.new_record) {
        if (blink) text(TITLE, U(72), L"NEW RECORD!", cx, top + U(405), 1, GOLD_TOP, GOLD_BOTTOM);
    } else {
        text(TEXT, U(40),
             (v.best_lap > 0 ? L"RECORD:  LAP " + num(v.best_lap) + L"   ·   WAVE " : std::wstring(L"RECORD:  WAVE ")) +
                 num(v.best_wave) + L"   ·   " + num(v.best_score) + L" POINTS",
             cx, top + U(410), 1, AMBER, AMBER);
        int gap = v.best_wave - v.wave;
        std::wstring push = gap <= 0 ? L"So close! A few more points next time."
                          : gap == 1 ? L"Just one more wave to beat it!"
                                     : L"" + num(gap) + L" more waves to beat it. You can do it!";
        text(TEXT, U(36), push, cx, top + U(462), 1, PALE_TOP, PALE_BOTTOM, 0.9f);
    }
    double since = now - v.results_at - SURVIVAL_RESULTS_DELAY;   // shots count from here on: fades in
    if (since >= 0 && (blink || v.new_record || since < 0.6))
        text(TITLE, U(60), L"SHOOT TO PLAY AGAIN", cx, top + U(525), 1, GOLD_TOP, GOLD_BOTTOM,
             float(std::min(1.0, since / 0.6)));
    text(TEXT, U(28), L"ESC: MAIN MENU", cx, top + U(655), 1, FOOT, FOOT);
}

bool SurvivalUi::draw(int w, int h, const SurvivalView& v, double now, std::vector<uint32_t>& out,
                      const SurvivalLayout& layout) {
    if (w <= 0 || h <= 0) return false;
    // The BONUS target moves and pulses: redrawn every time it is up
    std::wstring bonus_key;
    if (v.mode == SurvivalMode::Playing && v.bonus_on)
        bonus_key = std::to_wstring(int(v.bonus_x * 2000)) + L"," + std::to_wstring(int(v.bonus_y * 2000)) + L"," +
                    std::to_wstring(int(now * 30)) + L"," + std::to_wstring(int(layout.scale * 100));
    // What the picture shows; it is redrawn only when that changes
    double t = now - v.wave_started;
    int banner = v.mode == SurvivalMode::Playing && t >= 0 && t < SURVIVAL_COUNTDOWN + 1.0 ? 1 + int(t * 20) : 0;
    int phase = int(now * (v.mode == SurvivalMode::Loading ? 3 : 2.5));
    wchar_t key[160];
    swprintf(key, 160, L"%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", w, h, int(v.mode), v.wave, v.score,
             v.best_wave, v.new_record, banner, v.mode == SurvivalMode::Playing ? 0 : phase, v.lives, int(v.step),
             int(v.progress * 200), v.left, v.total, v.resting * (v.walking ? 2 : 1) + 4 * v.ambush + 8 * v.zone_done,
             int(std::ceil(v.rest_left)) +
                 (v.mode == SurvivalMode::Results ? 100 * int(std::clamp((now - v.results_at - SURVIVAL_RESULTS_DELAY) * 20, -1.0, 13.0)) : 0));
    double rt = now - v.reward_at;
    double lt = now - v.lap_started, at = now - v.ambush_started;
    std::wstring full_key = std::wstring(key) + L"|" + bonus_key + L"|" + std::to_wstring(v.lap) + L"|" +
                            std::to_wstring(int(v.fade * 64)) + L"|" +
                            std::to_wstring(v.reward && rt >= 0 && rt < 2.5 ? 1 + int(rt * 20) : 0) + L"|" +
                            std::to_wstring(lt >= 0 && lt < 3.0 ? 1 + int(lt * 20) : 0) + L"|" +
                            std::to_wstring(v.ambush && at >= 0 && at < 2.5 ? 1 + int(at * 20) : 0);
    if (full_key == last_key_ && out.size() == size_t(w) * h) return false;
    last_key_ = full_key;
    w_ = w, h_ = h;
    u_ = std::min(float(w) / 1920.0f, float(h) / 1080.0f);
    out.assign(size_t(w) * h, 0);
    px_ = out.data();
    switch (v.mode) {
    case SurvivalMode::Loading:
    case SurvivalMode::Ready: draw_intro(v, now); break;
    case SurvivalMode::Playing:
        draw_bonus(v, layout, now);
        draw_hud(v, now);
        if (v.fade > 0) fill(0, 0, w_, h_, 0x000000, v.fade);   // the way back to the first stop
        break;
    case SurvivalMode::Results: {
        draw_results(v, now);
        float fade = float(std::clamp((now - v.results_at) / 0.5, 0.0, 1.0));   // after the last attack
        if (fade < 1)
            for (uint32_t& p : out) {   // premultiplied: every channel scales
                uint32_t r = 0;
                for (int sh = 0; sh < 32; sh += 8) r |= uint32_t(float((p >> sh) & 0xFF) * fade) << sh;
                p = r;
            }
        break;
    }
    default: break;
    }
    return true;
}
