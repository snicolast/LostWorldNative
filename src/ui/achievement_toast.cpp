#include "achievement_toast.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <mmsystem.h>

namespace {

// The menus' colours (ui/frontend.cpp)
constexpr uint32_t GOLD_TOP = 0xFFEA5C, GOLD_BOTTOM = 0xFF8A00;
constexpr uint32_t PALE_TOP = 0xE6DCC0, PALE_BOTTOM = 0xBFB294, AMBER = 0xFFA032;
constexpr double SHOW_SECONDS = 5, SLIDE_IN = 0.5, SLIDE_OUT = 0.4;
constexpr double SHINE_AT = 0.4, SHINE_TIME = 0.55;   // a light sweep once it has landed

using ui::mix;
using ui::over;

// Slide curves: overshoot a little on the way in, take a run up on the way out
double ease_out_back(double t) {
    const double c = 1.9;
    t -= 1;
    return 1 + (c + 1) * t * t * t + c * t * t;
}
double ease_in_back(double t) {
    const double c = 1.7;
    return (c + 1) * t * t * t - c * t * t;
}

// The chime: a quick rising arpeggio (C6 E6 G6) into a ringing C7, sines with
// a little third harmonic for an arcade edge, as a 16 bit mono WAV
std::vector<uint8_t> make_chime(float gain) {
    const int RATE = 44100;
    struct Note { double at, freq, decay, level; };
    static const Note NOTES[] = {{0.000, 1046.5, 0.10, 0.55}, {0.065, 1318.5, 0.10, 0.55},
                                 {0.130, 1568.0, 0.12, 0.55}, {0.195, 2093.0, 0.38, 0.80},
                                 {0.195, 3136.0, 0.20, 0.18}};
    const int n = int(RATE * 0.95);
    std::vector<float> mixbuf(size_t(n), 0.0f);
    for (const Note& note : NOTES) {
        int start = int(note.at * RATE);
        for (int i = start; i < n; i++) {
            double t = double(i - start) / RATE;
            double env = std::min(1.0, t / 0.004) * std::exp(-t / note.decay);
            if (env < 1e-4 && t > 0.01) break;
            double ph = 2 * 3.14159265358979 * note.freq * t;
            mixbuf[size_t(i)] += float(note.level * env * (std::sin(ph) + 0.22 * std::sin(3 * ph)));
        }
    }
    std::vector<uint8_t> wav(44 + size_t(n) * 2);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&wav[at], &v, 2); };
    memcpy(&wav[0], "RIFF", 4);
    put32(4, uint32_t(wav.size() - 8));
    memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);   // PCM
    put16(22, 1);   // mono
    put32(24, RATE);
    put32(28, RATE * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&wav[36], "data", 4);
    put32(40, uint32_t(n * 2));
    for (int i = 0; i < n; i++) {
        float v = std::clamp(mixbuf[size_t(i)] * gain, -1.0f, 1.0f);
        put16(44 + size_t(i) * 2, uint16_t(int16_t(v * 32767.0f)));
    }
    return wav;
}

}  // namespace

bool AchievementToast::init(const std::string& font_dir) {
    gdi_ = &GdiText::acquire(font_dir);   // the fonts of the menus
    jurassic_ = gdi_->loaded(GdiText::JURASSIC);
    return gdi_->ready();
}

void AchievementToast::shutdown() {
    if (gdi_) gdi_->release();
    gdi_ = nullptr;
    if (!wav_.empty()) PlaySoundW(nullptr, nullptr, 0);
    wav_.clear();
}

void AchievementToast::chime() {
    if (volume_ <= 0) return;
    PlaySoundW(nullptr, nullptr, 0);   // the buffer is about to change
    wav_ = make_chime(0.3f * std::min(volume_ / 5.0f, 2.0f));
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav_.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void AchievementToast::push(const std::wstring& name, const std::wstring& text) { queue_.push_back({name, text}); }
void AchievementToast::push_rising(const std::wstring& label, const std::wstring& name, const std::wstring& text) {
    queue_.push_back({name, text, label, true});
}

const GdiText::Mask& AchievementToast::mask(Family f, int px, const std::wstring& s) {
    return gdi_->mask(f == TEXT ? GdiText::BARLOW : jurassic_ ? GdiText::JURASSIC : GdiText::SAIRA, px, s);
}

int AchievementToast::text(Family f, int px, const std::wstring& s, int x, int y, int align, uint32_t top,
                           uint32_t bottom, float alpha) {
    return shadowed_text(mask(f, px, s), px, x, y, align, top, bottom, alpha);
}

// A soft diagonal band of light crossing the panel, pos 0..1 from left to right
void AchievementToast::shine(int x0, int y0, int x1, int y1, float pos) {
    int band = std::max(8, (y1 - y0) / 2), slant = (y1 - y0) / 2;
    float centre = x0 - band - slant + pos * float(x1 - x0 + 2 * band + 2 * slant);
    for (int y = std::max(y0, 0); y < std::min(y1, h_); y++) {
        uint32_t* row = px_ + size_t(y) * w_;
        float c = centre + float(y1 - y) * slant / float(y1 - y0);
        for (int x = std::max(x0, int(c) - band); x < std::min({x1, int(c) + band + 1, w_}); x++) {
            float d = 1.0f - std::fabs(float(x) - c) / band;
            if (d > 0) row[x] = over(row[x], 0xFFF4D0, int(d * d * 0.28f * 256.0f));
        }
    }
}

// The credit: right aligned in the bottom right corner, over the game's own
// picture, small and a little see-through. Returns where it drew
RECT AchievementToast::draw_credit(float alpha) {
    float u = std::min(h_ / 1080.0f, w_ / 1440.0f);
    auto U = [&](float v) { return std::max(1, int(v * u + 0.5f)); };
    int px1 = U(26), px2 = U(22);
    int x = w_ - U(28);
    int y2 = h_ - U(22) - px2 - U(4), y1 = y2 - px1 - U(4);
    int w1 = text(TEXT, px1, credit_[0], x, y1, 2, PALE_TOP, PALE_BOTTOM, 0.85f * alpha);
    int w2 = credit_[1].empty() ? 0 : text(TEXT, px2, credit_[1], x, y2, 2, AMBER, AMBER, 0.75f * alpha);
    int m = U(10);
    return {std::clamp(x - std::max(w1, w2) - m, 0, w_), std::clamp(y1 - m, 0, h_), std::clamp(x + m, 0, w_),
            std::clamp(y2 + px2 * 2 + m, 0, h_)};
}

bool AchievementToast::draw(int w, int h, double now, std::vector<uint32_t>& out) {
    while (!queue_.empty() && shown_at_ >= 0 && now - shown_at_ > SHOW_SECONDS) {   // the next one
        queue_.pop_front();
        shown_at_ = -1;
    }
    // The credit fades in and out over 0.4 s
    double dt = credit_at_ < 0 ? 0 : std::clamp(now - credit_at_, 0.0, 0.1);
    credit_at_ = now;
    float target = credit_on_ && !credit_[0].empty() ? 1.0f : 0.0f, was = credit_alpha_;
    float step = float(dt / 0.4);
    credit_alpha_ = target > was ? std::min(target, was + step) : std::max(target, was - step);
    bool credit_changed = credit_alpha_ != was;

    bool resized = w != w_ || h != h_ || out.size() != size_t(w) * h;
    double t = 0;
    bool toast_redraw = drawn_;   // an empty queue: wipe the last panel
    if (!queue_.empty()) {
        if (shown_at_ < 0) {
            shown_at_ = now;
            chime();
        }
        t = now - shown_at_;
        bool moving = t < SHINE_AT + SHINE_TIME || t > SHOW_SECONDS - SLIDE_OUT;
        toast_redraw = moving || !drawn_;   // resting in its corner: nothing to do
    }
    if (!resized && !credit_changed && !toast_redraw) return false;
    if (resized) {
        w_ = w, h_ = h;
        out.assign(size_t(w) * h, 0);
    } else {   // wipe only where the panel and the credit were
        for (const RECT& r : {dirty_, credit_dirty_})
            for (LONG y = r.top; y < r.bottom; y++)
                std::fill(out.begin() + y * w_ + r.left, out.begin() + y * w_ + r.right, 0u);
    }
    px_ = out.data();
    dirty_ = credit_dirty_ = {};
    drawn_ = false;
    if (credit_alpha_ > 0) credit_dirty_ = draw_credit(credit_alpha_);
    if (queue_.empty()) return true;
    float u = std::min(h / 1080.0f, w / 1440.0f);
    auto U = [&](float v) { return int(v * u + 0.5f); };
    const auto& n = queue_.front();
    int title_px = U(46), text_px = U(28), label_px = U(22);
    int inner = std::max({mask(TITLE, title_px, n.name).text_w, mask(TEXT, text_px, n.text).text_w,
                          mask(TEXT, label_px, n.label).text_w, U(300)});
    int rest_x1 = w - U(40), pw = inner + U(56), y1 = h - U(48), y0 = y1 - U(170);
    if (n.rising) rest_x1 = (w + pw) / 2;   // the bottom middle
    // Off screen to the right (or below) -> resting place -> off screen again
    double off = 0;
    if (t < SLIDE_IN) off = 1 - ease_out_back(t / SLIDE_IN);
    else if (t > SHOW_SECONDS - SLIDE_OUT) off = ease_in_back(std::min(1.0, (t - (SHOW_SECONDS - SLIDE_OUT)) / SLIDE_OUT));
    int x1 = rest_x1, x0 = x1 - pw;
    if (n.rising) {
        int dy = int(off * (y1 - y0 + U(48) + U(20)));
        y0 += dy, y1 += dy;
    } else {
        x1 += int(off * (pw + U(40) + U(20))), x0 = x1 - pw;
    }
    float alpha = float(std::clamp(t / 0.12, 0.0, 1.0));
    fill(x0, y0, x1, y1, 0x0C0A06, 0.88f * alpha);
    int th = std::max(1, U(3));
    fill(x0, y0, x1, y0 + th, AMBER, 0.85f * alpha);
    fill(x0, y1 - th, x1, y1, AMBER, 0.85f * alpha);
    fill(x0, y0, x0 + th, y1, AMBER, 0.35f * alpha);
    fill(x1 - th, y0, x1, y1, AMBER, 0.35f * alpha);
    // The accent bar on the left edge flashes white as it lands
    float flash = float(std::clamp(1.0 - std::fabs(t - SLIDE_IN * 0.7) / 0.25, 0.0, 1.0));
    fill(x0, y0, x0 + U(8), y1, mix(AMBER, 0xFFFFFF, flash), 0.95f * alpha);
    int x = x0 + U(32);
    text(TEXT, label_px, n.label, x, y0 + U(18), 0, AMBER, AMBER, alpha);
    text(TITLE, title_px, n.name, x, y0 + U(52), 0, GOLD_TOP, GOLD_BOTTOM, alpha);
    text(TEXT, text_px, n.text, x, y0 + U(114), 0, PALE_TOP, PALE_BOTTOM, alpha);
    if (t > SHINE_AT && t < SHINE_AT + SHINE_TIME) shine(x0, y0, x1, y1, float((t - SHINE_AT) / SHINE_TIME));
    // Text shadows and the overshoot reach a little past the panel
    int m = U(12);
    dirty_ = {std::clamp(x0 - m, 0, w), std::clamp(y0 - m, 0, h), std::clamp(x1 + m, 0, w), std::clamp(y1 + m, 0, h)};
    drawn_ = true;
    return true;
}
