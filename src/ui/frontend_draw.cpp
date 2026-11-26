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

// ---- Drawing primitives (premultiplied BGRA canvas) ----

// Horizontal band that fades out towards both ends
void Frontend::band(int cx, int y0, int y1, int half_w, uint32_t rgb, float alpha) {
    for (int x = std::max(cx - half_w, 0); x < std::min(cx + half_w, w_); x++) {
        float t = 1 - std::abs(float(x - cx)) / float(half_w);
        float a = alpha * std::min(1.0f, t * 2.5f);
        for (int y = std::max(y0, 0); y < std::min(y1, h_); y++) blend(x, y, rgb, a);
    }
}

// Small solid triangle pointing left (dir -1), right (1), up (-2) or down (2), centred on (x, y)
void Frontend::triangle(int x, int y, int size, int dir, uint32_t rgb, float alpha) {
    int half = size / 2;
    for (int dy = -half; dy <= half; dy++) {
        float len = float(size) * (1 - std::abs(float(dy)) / float(half + 1)) * 0.9f;
        for (int i = 0; i < int(len + 1); i++) {
            float a = std::clamp(len - float(i), 0.0f, 1.0f) * alpha;
            if (dir == 2 || dir == -2) {   // pointing down / up
                int py = dir > 0 ? y - half + i : y + half - i;
                blend(x + dy, py, rgb, a);
                continue;
            }
            int px = dir > 0 ? x - half + i : x + half - i;
            blend(px, y + dy, rgb, a);
        }
    }
}

void Frontend::hgrad(int x0, int y0, int x1, int y1, uint32_t rgb, float a0, float a1) {
    if (x1 <= x0) return;
    for (int x = std::max(x0, 0); x < std::min(x1, w_); x++) {
        float t = float(x - x0) / float(x1 - x0);
        int a = int(std::clamp(a0 + (a1 - a0) * t, 0.0f, 1.0f) * 256.0f + 0.5f);
        for (int y = std::max(y0, 0); y < std::min(y1, h_); y++) {
            uint32_t& d = px_[size_t(y) * w_ + x];
            d = over(d, rgb, a);
        }
    }
}

void Frontend::vgrad(int x0, int y0, int x1, int y1, uint32_t rgb, float a0, float a1) {
    if (y1 <= y0) return;
    for (int y = std::max(y0, 0); y < std::min(y1, h_); y++)
        fill(x0, y, x1, y + 1, rgb, a0 + (a1 - a0) * float(y - y0) / float(y1 - y0));
}

void Frontend::frame(int x0, int y0, int x1, int y1, int t, uint32_t rgb, float alpha) {
    fill(x0, y0, x1, y0 + t, rgb, alpha);
    fill(x0, y1 - t, x1, y1, rgb, alpha);
    fill(x0, y0 + t, x0 + t, y1 - t, rgb, alpha);
    fill(x1 - t, y0 + t, x1, y1 - t, rgb, alpha);
}

// A box with half-round ends, smoothed: switches, sliders, the gamepad's buttons
void Frontend::pill(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha) {
    float r = (y1 - y0) * 0.5f, cy = (y0 + y1) * 0.5f;
    float lx = x0 + r, rx = x1 - r;
    if (rx < lx) lx = rx = (x0 + x1) * 0.5f;
    for (int y = std::max(y0, 0); y < std::min(y1, h_); y++)
        for (int x = std::max(x0, 0); x < std::min(x1, w_); x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float dx = px < lx ? lx - px : px > rx ? px - rx : 0.0f, dy = py - cy;
            float cov = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            if (cov > 0) blend(x, y, rgb, cov * alpha);
        }
}

// A picture scaled to cover the box (bilinear), what overhangs cut off
void Frontend::picture(const Picture& p, int x0, int y0, int x1, int y1) {
    if (p.w <= 0 || x1 <= x0 || y1 <= y0) return;
    float sc = std::max(float(x1 - x0) / p.w, float(y1 - y0) / p.h);
    float ox = (p.w * sc - (x1 - x0)) * 0.5f, oy = (p.h * sc - (y1 - y0)) * 0.5f;
    for (int y = std::max(y0, 0); y < std::min(y1, h_); y++) {
        float v = std::clamp((y - y0 + oy + 0.5f) / sc - 0.5f, 0.0f, p.h - 1.001f);
        int iv = int(v);
        float fv = v - iv;
        for (int x = std::max(x0, 0); x < std::min(x1, w_); x++) {
            float u = std::clamp((x - x0 + ox + 0.5f) / sc - 0.5f, 0.0f, p.w - 1.001f);
            int iu = int(u);
            float fu = u - iu;
            const uint32_t* q = &p.px[size_t(iv) * p.w + iu];
            uint32_t c[4] = {q[0], q[1], q[p.w], q[p.w + 1]};
            uint32_t out = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                float a = chan(c[0], sh) + (chan(c[1], sh) - chan(c[0], sh)) * fu;
                float b = chan(c[2], sh) + (chan(c[3], sh) - chan(c[2], sh)) * fu;
                out |= uint32_t(std::clamp(a + (b - a) * fv, 0.0f, 1.0f) * 255.0f + 0.5f) << sh;
            }
            px_[size_t(y) * w_ + x] = out;
        }
    }
}

// A picture with transparency, area-averaged down to w x h at x0, y0
void Frontend::sprite(const Picture& p, int x0, int y0, int w, int h) {
    if (p.w <= 0 || w <= 0 || h <= 0) return;
    float sx = float(p.w) / w, sy = float(p.h) / h;
    for (int y = std::max(0, y0); y < std::min(h_, y0 + h); y++)
        for (int x = std::max(0, x0); x < std::min(w_, x0 + w); x++) {
            int u0 = int((x - x0) * sx), u1 = std::max(u0 + 1, std::min(p.w, int((x - x0 + 1) * sx)));
            int v0 = int((y - y0) * sy), v1 = std::max(v0 + 1, std::min(p.h, int((y - y0 + 1) * sy)));
            float rgb[3] = {0, 0, 0}, a = 0;   // colours weighted by their alpha
            for (int v = v0; v < v1; v++)
                for (int u = u0; u < u1; u++) {
                    uint32_t c = p.px[size_t(v) * p.w + u];
                    float ca = chan(c, 24);
                    for (int m = 0; m < 3; m++) rgb[m] += chan(c, 16 - 8 * m) * ca;
                    a += ca;
                }
            if (a <= 0) continue;
            uint32_t c = 0;
            for (int m = 0; m < 3; m++) c |= uint32_t(std::clamp(rgb[m] / a, 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * m);
            blend(x, y, c, a / ((u1 - u0) * (v1 - v0)));
        }
}

int Frontend::measure(Family f, int px, const std::wstring& s) {
    if (f == JURASSIC) px = int(px * 1.34f + 0.5f);   // as text() draws it
    return mask(f, px, s, 0).text_w;
}

// The rows the letters of s cover (their ink, not the font's box), counted
// from the y that text() is given: to centre a word in a box with any font
void Frontend::ink_rows(Family f, int px, const std::wstring& s, int& top, int& bottom) {
    int shift = 0;
    if (f == JURASSIC) {   // as text() draws it
        shift = int(px * 0.42f + 0.5f);
        px = int(px * 1.34f + 0.5f);
    }
    const Mask& m = mask(f, px, s, 0);
    top = m.h, bottom = 0;
    for (int j = 0; j < m.h; j++)
        for (int i = 0; i < m.w; i++)
            if (m.a[size_t(j) * m.w + i]) {
                top = std::min(top, j), bottom = std::max(bottom, j + 1);
                break;
            }
    if (top >= bottom) top = m.pad, bottom = m.pad + m.text_h;   // nothing drawn
    top += shift - m.pad, bottom += shift - m.pad;
}

int Frontend::extent(Family f, int px, const std::wstring& s) { return gdi_->extent(GdiText::Face(f), px, s); }

// Lines of at most max_w pixels, broken between words
std::vector<std::wstring> Frontend::wrap(Family f, int px, const std::wstring& s, int max_w) {
    std::vector<std::wstring> lines;
    std::wstring line, word;
    auto put = [&] {
        if (word.empty()) return;
        std::wstring longer = line.empty() ? word : line + L" " + word;
        if (!line.empty() && extent(f, px, longer) > max_w) {
            lines.push_back(line);
            line = word;
        } else {
            line = longer;
        }
        word.clear();
    };
    for (wchar_t c : s) {
        if (c == L' ') put();
        else word += c;
    }
    put();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Draws text with its box's top at y; align 0 = left of x, 1 = centred, 2 = right
int Frontend::text(Family f, int px, const std::wstring& s, int x, int y, int align, const Style& st) {
    if (f == JURASSIC) {   // same capital height and position as Saira at this size
        y += int(px * 0.42f + 0.5f);
        px = int(px * 1.34f + 0.5f);
    }
    const Mask& m = mask(f, px, s, st.edge_px);
    int x0 = x - (align == 1 ? m.text_w / 2 : align == 2 ? m.text_w : 0) - m.pad;
    int y0 = y - m.pad;
    auto paint = [&](const std::vector<uint8_t>& cov, int ox, int oy, bool gradient, uint32_t rgb, float alpha) {
        for (int j = 0; j < m.h; j++) {
            uint32_t row = rgb;
            if (gradient) {
                float t = std::clamp(float(j - m.grad0) / float(std::max(1, m.grad1 - m.grad0)), 0.0f, 1.0f);
                row = mix(st.top, st.bottom, t);
            }
            for (int i = 0; i < m.w; i++) {
                uint8_t c = cov[size_t(j) * m.w + i];
                if (c) blend(x0 + ox + i, y0 + oy + j, row, float(c) / 255.0f * alpha);
            }
        }
    };
    const auto& outer = st.edge_px > 0 ? m.e : m.a;
    if (st.shadow) paint(outer, st.shadow / 2, st.shadow, false, 0x000000, 0.55f * st.alpha);
    if (st.edge_px > 0) paint(m.e, 0, 0, false, st.edge, st.alpha);
    paint(m.a, 0, 0, true, 0, st.alpha);
    return m.text_w;
}

// Poster placement (fractions of the poster's height): where its logo starts
// and ends, and where its plain stone ends (credits below). The logo is drawn
// LOGO_SCREEN_H of the window high, starting LOGO_SCREEN_TOP from the top,
// centred LOGO_CENTER_X layout units from the left (above the menu's column).
constexpr float POSTER_LOGO_TOP = 0.16f, POSTER_LOGO_END = 0.53f, POSTER_STONE_END = 0.88f;
constexpr float LOGO_SCREEN_TOP = 0.08f, LOGO_SCREEN_H = 0.36f, LOGO_CENTER_X = 450;
constexpr float LOGO_PICTURE_X = 350;   // logo.png's centre from the right edge: lined up with the menu's column
constexpr float LOGO_W = 400;           // logo.png at most this wide (layout units), the tagline under it

void Frontend::poster_layout(float& scale, float& x0, float& y0) const {
    scale = LOGO_SCREEN_H * h_ / ((POSTER_LOGO_END - POSTER_LOGO_TOP) * poster_h_);   // window px per poster px
    float cx = has_backdrop() ? w_ - LOGO_CENTER_X * u_ : LOGO_CENTER_X * u_;   // above the menu's column
    x0 = cx - poster_w_ * scale * 0.5f;
    y0 = LOGO_SCREEN_TOP * h_ - POSTER_LOGO_TOP * poster_h_ * scale;
}

// The poster at the top left; around and below it, its plain stone repeated in
// mirror image with the brightness of the poster's edges at that height,
// cross-faded into the poster and darkened away from it. stone gets the same
// without the poster: both come from one pass, as they share the stone samples
void Frontend::poster_background(std::vector<uint32_t>& dst, std::vector<uint32_t>& stone) {
    dst.resize(size_t(w_) * h_);
    stone.resize(size_t(w_) * h_);
    float s, x0, y0;
    poster_layout(s, x0, y0);
    float pw = poster_w_ * s;
    float stone0 = poster_h_ * 0.60f, stone_h = poster_h_ * (POSTER_STONE_END - 0.60f);
    auto texel = [&](float u, float v, float* rgb) {   // bilinear, poster pixels
        u = std::clamp(u, 0.0f, poster_w_ - 1.001f);
        v = std::clamp(v, 0.0f, poster_h_ - 1.001f);
        int iu = int(u), iv = int(v);
        float fu = u - iu, fv = v - iv;
        const uint32_t* p = &poster_[size_t(iv) * poster_w_ + iu];
        uint32_t c[4] = {p[0], p[1], p[poster_w_], p[poster_w_ + 1]};
        for (int k = 0; k < 3; k++) {
            int sh = 16 - 8 * k;
            float a = chan(c[0], sh) + (chan(c[1], sh) - chan(c[0], sh)) * fu;
            float b = chan(c[2], sh) + (chan(c[3], sh) - chan(c[2], sh)) * fu;
            rgb[k] += a + (b - a) * fv;
        }
    };
    auto pingpong = [](float t, float len) {
        float m = std::fmod(std::abs(t), 2 * len);
        return m < len ? m : 2 * len - m;
    };
    // Brightness of the poster's outer columns per row (the stone is lighter at
    // the top), against that of the stone used for the sides
    std::vector<float> edge(poster_h_);
    int band_w = std::max(1, poster_w_ / 20);
    for (int y = 0; y < poster_h_; y++) {
        float sum = 0;
        for (int x = 0; x < band_w; x++)
            for (uint32_t c : {poster_[size_t(y) * poster_w_ + x], poster_[size_t(y) * poster_w_ + poster_w_ - 1 - x]})
                sum += chan(c, 16) + chan(c, 8) + chan(c, 0);
        edge[y] = sum / (band_w * 6);
    }
    std::vector<float> smooth(poster_h_);
    int r = std::max(1, poster_h_ / 30);
    for (int y = 0; y < poster_h_; y++) {
        float sum = 0;
        int n = 0;
        for (int k = std::max(0, y - r); k <= std::min(poster_h_ - 1, y + r); k++) sum += edge[k], n++;
        smooth[y] = sum / n;
    }
    float stone_lum = 0;
    for (int y = int(stone0); y < int(stone0 + stone_h); y++) stone_lum += smooth[y];
    stone_lum = std::max(stone_lum / std::max(1, int(stone_h)), 0.01f);

    float step = 0.5f / s;   // 2x2 samples per window pixel
    float seam = pw * 0.2f, fade_h = poster_h_ * 0.05f;
    // Rows are split between threads: it is a lot of samples at 4K
    auto rows_job = [&](int ya, int yb) {
    for (int y = ya; y < yb; y++) {
        float py = (y - y0) / s;
        // the poster's own picture stops before its credits
        float rows = std::clamp((POSTER_STONE_END * poster_h_ - py) / fade_h, 0.0f, 1.0f);
        float gain = std::clamp(smooth[std::clamp(int(std::min(py, stone0 + stone_h)), 0, poster_h_ - 1)] / stone_lum,
                                0.6f, 1.8f);
        for (int x = 0; x < w_; x++) {
            float poster_rgb[3] = {0, 0, 0}, stone_rgb[3] = {0, 0, 0};
            const float none[3] = {0, 0, 0};
            float px = (x - x0) / s;
            float inside = std::clamp(std::min(x - x0, x0 + pw - x) / seam, 0.0f, 1.0f) * rows;
            if (inside > 0)
                for (int k = 0; k < 4; k++) texel(px + (k & 1) * step, py + (k >> 1) * step, poster_rgb);
            for (int k = 0; k < 4; k++)
                texel(pingpong(px + (k & 1) * step, float(poster_w_ - 1)),
                      stone0 + pingpong(py + (k >> 1) * step, stone_h), stone_rgb);
            float side = std::min(1.0f, std::abs(x - (x0 + pw * 0.5f)) / float(w_));
            float dark = 1 - 0.4f * side * side;
            auto pixel = [&](const float* poster, float in) {
                uint32_t out = 0xFF000000u;
                for (int k = 0; k < 3; k++) {
                    float v = (poster[k] * in + stone_rgb[k] * gain * (1 - in)) * 0.25f * dark;
                    out |= uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f) << (16 - 8 * k);
                }
                return out;
            };
            dst[size_t(y) * w_ + x] = pixel(poster_rgb, inside);
            stone[size_t(y) * w_ + x] = pixel(none, 0.0f);
        }
    }
    };
    int n = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int i = 0; i < n; i++) pool.emplace_back(rows_job, h_ * i / n, h_ * (i + 1) / n);
    for (auto& t : pool) t.join();
}

// The background picture: covering the window, darker towards the right where
// the menu goes, with the poster's logo above it; blurred for the options
void Frontend::backdrop_background() {
    uint32_t* keep = px_;
    bg_.assign(size_t(w_) * h_, 0xFF000000u);
    px_ = bg_.data();
    picture(backdrop_, 0, 0, w_, h_);
    // The blur: the picture averaged down by 16 in each direction, smoothed a
    // few times and stretched back
    const int F = 16;
    Picture blur;
    blur.w = std::max(4, w_ / F);
    blur.h = std::max(4, h_ / F);
    std::vector<float> acc(size_t(blur.w) * blur.h * 3, 0.0f);
    for (int y = 0; y < blur.h * F && y < h_; y++)
        for (int x = 0; x < blur.w * F && x < w_; x++) {
            uint32_t c = bg_[size_t(y) * w_ + x];
            float* d = &acc[(size_t(y / F) * blur.w + x / F) * 3];
            for (int m = 0; m < 3; m++) d[m] += chan(c, 16 - 8 * m) / (F * F);
        }
    for (int pass = 0; pass < 3; pass++) {   // 3x3 box, three times
        std::vector<float> src = acc;
        for (int y = 0; y < blur.h; y++)
            for (int x = 0; x < blur.w; x++)
                for (int m = 0; m < 3; m++) {
                    float sum = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = std::clamp(x + dx, 0, blur.w - 1), yy = std::clamp(y + dy, 0, blur.h - 1);
                            sum += src[(size_t(yy) * blur.w + xx) * 3 + m];
                        }
                    acc[(size_t(y) * blur.w + x) * 3 + m] = sum / 9;
                }
    }
    blur.px.resize(size_t(blur.w) * blur.h);
    for (size_t i = 0; i < blur.px.size(); i++) {
        uint32_t out = 0xFF000000u;
        for (int m = 0; m < 3; m++)
            out |= uint32_t(std::clamp(acc[i * 3 + m], 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * m);
        blur.px[i] = out;
    }
    bg_stone_.assign(size_t(w_) * h_, 0xFF000000u);
    px_ = bg_stone_.data();
    picture(blur, 0, 0, w_, h_);
    fill(0, 0, w_, h_, 0x000000, 0.3f);
    px_ = bg_.data();
    shade_backdrop();
    // The same lit by a strike: brighter and bluer, its brightest parts (the
    // bolt, the sky) the most
    bg_lit_.resize(bg_.size());
    static const float TINT[3] = {0.80f, 0.88f, 1.0f};
    for (size_t i = 0; i < bg_.size(); i++) {
        float c[3] = {chan(bg_[i], 16), chan(bg_[i], 8), chan(bg_[i], 0)};
        float lum = (c[0] + c[1] + c[2]) / 3, glow = lum * std::sqrt(lum) * 2.2f + 0.1f;
        uint32_t o = 0xFF000000u;
        for (int m = 0; m < 3; m++)
            o |= uint32_t(std::clamp(c[m] * 2.6f + glow * TINT[m], 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * m);
        bg_lit_[i] = o;
    }
    px_ = keep;
}

// Darker at the right (under the menu's column) and the bottom, the logo above
// the column: on px_
void Frontend::shade_backdrop() {
    hgrad(int(w_ * 0.40f), 0, int(w_ * 0.64f), h_, 0x040508, 0.0f, 0.6f);
    fill(int(w_ * 0.64f), 0, w_, h_, 0x040508, 0.6f);
    hgrad(int(w_ * 0.64f), 0, w_, h_, 0x040508, 0.0f, 0.6f);
    vgrad(0, int(h_ * 0.72f), w_, h_, 0x000000, 0.0f, 0.6f);
    if (has_logo()) {
        // The logo picture (area-averaged down), LOGO_W wide at most, and
        // under it a thin amber rule and what this is, in three lines of
        // amber typewriter capitals; all of it centred in the room above the column
        auto U = [&](float v) { return int(v * u_ + 0.5f); };
        static const wchar_t* const TAGLINE[] = {L"AN UNOFFICIAL, FAN-MADE RECOMPILATION", L"OF THE 1997 ARCADE GAME",
                                                 L"THE LOST WORLD: JURASSIC PARK"};
        const int lines = int(std::size(TAGLINE));
        const int tag_px = U(20), tag_line = U(28), gap = U(14), rule_gap = U(14);
        const int below = gap + rule_gap + lines * tag_line;
        const int band0 = int(0.05f * h_), band1 = int((LOGO_SCREEN_TOP + LOGO_SCREEN_H) * h_) + U(40) - U(50);
        int lw = U(LOGO_W), lh = lw * logo_.h / logo_.w;
        const int max_h = band1 - band0 - below;   // a tall logo: as high as fits
        if (lh > max_h && max_h > 0) lh = max_h, lw = lh * logo_.w / logo_.h;
        const int cx = int(w_ - LOGO_PICTURE_X * u_);
        int top = band0 + std::max(0, (band1 - band0 - (lh + below)) / 2);
        sprite(logo_, cx - lw / 2, top, lw, lh);
        int ty = top + lh + gap, rule_w = U(330);
        fill(cx - rule_w / 2, ty, cx + rule_w / 2, ty + std::max(1, U(1.5f)), AMBER, 0.7f);
        ty += rule_gap;
        for (const wchar_t* line : TAGLINE) {
            text(MONO, tag_px, line, cx, ty, 1, {0xF0C890, 0xF0C890, 0, 0, 1, std::max(1, U(2))});
            ty += tag_line;
        }
    } else if (has_poster()) {   // the logo cut out of the poster: its disc and its banner (cover.png's shapes)
        float s, x0, y0;
        poster_layout(s, x0, y0);
        const float CX = 0.497f, CY = 0.331f, R = 0.203f;          // the disc, fractions of the poster's width / height
        const float BU0 = 0.092f, BU1 = 0.903f, BV0 = 0.333f, BV1 = 0.437f;   // the banner across it
        float cx = CX * poster_w_, cy = CY * poster_h_, r = R * poster_h_;
        int sx0 = std::max(0, int(x0 + std::min(cx - r, BU0 * poster_w_) * s) - 2);
        int sx1 = std::min(w_, int(x0 + std::max(cx + r, BU1 * poster_w_) * s) + 2);
        int sy0 = std::max(0, int(y0 + (cy - r) * s) - 2), sy1 = std::min(h_, int(y0 + (cy + r) * s) + 2);
        float feather = 2.5f;   // poster pixels
        for (int y = sy0; y < sy1; y++)
            for (int x = sx0; x < sx1; x++) {
                float u = (x + 0.5f - x0) / s, v = (y + 0.5f - y0) / s;
                float in_disc = r - std::sqrt((u - cx) * (u - cx) + (v - cy) * (v - cy));
                float in_banner = std::min(std::min(u - BU0 * poster_w_, BU1 * poster_w_ - u),
                                           std::min(v - BV0 * poster_h_, BV1 * poster_h_ - v));
                float a = std::clamp(std::max(in_disc, in_banner) / feather + 0.5f, 0.0f, 1.0f);
                if (a <= 0) continue;
                float rgb[3] = {0, 0, 0};   // 2x2 samples per window pixel
                for (int k = 0; k < 4; k++) {
                    int pu = std::clamp(int((x + 0.25f + 0.5f * (k & 1) - x0) / s), 0, poster_w_ - 1);
                    int pv = std::clamp(int((y + 0.25f + 0.5f * (k >> 1) - y0) / s), 0, poster_h_ - 1);
                    uint32_t c = poster_[size_t(pv) * poster_w_ + pu];
                    for (int m = 0; m < 3; m++) rgb[m] += chan(c, 16 - 8 * m) * 0.25f;
                }
                blend(x, y, uint32_t(rgb[0] * 255) << 16 | uint32_t(rgb[1] * 255) << 8 | uint32_t(rgb[2] * 255), a);
            }
    }
}

// Main menu backdrop: dark jungle green, a warm glow behind the title and a
// faint scanline pattern
void Frontend::background() {
    bg_.resize(size_t(w_) * h_);
    if (has_backdrop()) {
        backdrop_background();
        return;
    }
    if (has_poster()) {   // both versions at once: switching screens must not stall
        poster_background(bg_, bg_stone_);   // the long options list goes on plain stone
        return;
    }
    float cx = LOGO_CENTER_X * u_, gy = 250 * u_;   // the glow behind the title
    for (int y = 0; y < h_; y++) {
        float t = std::pow(float(y) / float(h_), 0.8f);
        uint32_t base = mix(0x13291A, 0x040806, t);
        float br = chan(base, 16), bgc = chan(base, 8), bb = chan(base, 0);
        float scan = (y % 3 == 0) ? 0.93f : 1.0f;
        for (int x = 0; x < w_; x++) {
            float dx = (x - cx) / (950 * u_), dy = (y - gy) / (430 * u_);
            float glow = std::exp(-(dx * dx + dy * dy) * 1.6f) * 0.17f;
            float vx = (x - cx) / (w_ * 0.62f), vy = (y - h_ * 0.5f) / (h_ * 0.8f);
            float vig = 1 - 0.6f * std::min(1.0f, vx * vx + vy * vy);
            float r = (br + glow * 1.0f) * vig * scan, g = (bgc + glow * 0.6f) * vig * scan,
                  b = (bb + glow * 0.22f) * vig * scan;
            bg_[size_t(y) * w_ + x] = 0xFF000000u | uint32_t(std::min(r, 1.0f) * 255) << 16 |
                                      uint32_t(std::min(g, 1.0f) * 255) << 8 | uint32_t(std::min(b, 1.0f) * 255);
        }
    }
}


// ---- Screens ----


// One entry of the key help: the gamepad's button (its picture from res/pad,
// else a coloured disc, or a grey pill for the shoulders) or the key ("<>": the left / right arrows), then
// what it does. x moves past it; the rectangle is for clicks
RECT Frontend::prompt(int& x, int y, const wchar_t* pad_button, const wchar_t* key, const wchar_t* what) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int h = U(38), x0 = x;
    if (pad_prompts && pad_button && pad_icon(pad_button).w > 0) {
        const Picture& icon = pad_icon(pad_button);
        int iw = h * icon.w / icon.h;
        sprite(icon, x, y, iw, h);
        x += iw + U(10);
    } else if (pad_prompts && pad_button) {
        std::wstring b = pad_button;
        uint32_t c = b == L"A" ? PAD_A : b == L"B" ? PAD_B : b == L"Y" ? PAD_Y : b == L"X" ? PAD_X : KEYCAP;
        int bw = std::max(h, extent(ITEM, U(20), b) + U(20));
        pill(x, y, x + bw, y + h, c, 1);
        text(ITEM, U(20), b, x + bw / 2, y + h / 2 - U(11), 1, {0x111111, 0x111111});
        x += bw + U(10);
    } else if (key) {
        std::wstring k = key;
        bool arrows = k == L"<>", updown = k == L"UD";
        int kw = arrows || updown ? U(70) : std::max(h, extent(MONO, U(18), k) + U(22));
        fill(x, y, x + kw, y + h, 0x000000, 0.45f);
        frame(x, y, x + kw, y + h, std::max(1, U(1.5f)), KEYCAP, 0.75f);
        if (arrows) {
            triangle(x + kw / 2 - U(14), y + h / 2, U(14), -1, 0xE8E0CC, 1);
            triangle(x + kw / 2 + U(14), y + h / 2, U(14), +1, 0xE8E0CC, 1);
        } else if (updown) {
            triangle(x + kw / 2 - U(14), y + h / 2, U(14), -2, 0xE8E0CC, 1);
            triangle(x + kw / 2 + U(14), y + h / 2, U(14), +2, 0xE8E0CC, 1);
        } else {
            text(MONO, U(18), k, x + kw / 2, y + h / 2 - U(9), 1, {0xE8E0CC, 0xE8E0CC});
        }
        x += kw + U(10);
    }
    int right = x - U(10);
    if (what && *what) {
        right = x + text(ITEM, U(22), what, x, y + h / 2 - U(13), 0, {0xCFC8B4, 0xCFC8B4});
        x = right + U(44);
    }
    return {x0, y, right, y + h};
}

int Frontend::prompt_width(const wchar_t* pad_button, const wchar_t* key, const wchar_t* what) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int h = U(38), wd = 0;
    const Picture* icon = pad_prompts && pad_button ? &pad_icon(pad_button) : nullptr;
    if (icon && icon->w > 0) wd = h * icon->w / icon->h + U(10);
    else if (icon) wd = std::max(h, extent(ITEM, U(20), pad_button) + U(20)) + U(10);
    else if (key) wd = (std::wstring(key) == L"<>" || std::wstring(key) == L"UD" ? U(70) : std::max(h, extent(MONO, U(18), key) + U(22))) + U(10);
    if (what && *what) wd += measure(ITEM, U(22), what) + U(44);
    return wd;
}

// Main, Play, Boss and Survival on the backdrop, and Pause (or the boss list)
// over the game: the entries in a column at the left, under the logo or the title
// ---- Lightning ----
//
// The Lost World's own entrance for the side menus, in place of Sega Rally 2's
// slide. The main menu coming up (after the logos, back from a game) waits a
// moment in the dark, then a double strike lights the backdrop, its bolt and
// sky the most, and the entries come on with it, their letters white for a
// moment. From one side menu to another (PLAY and the lists under it) one
// smaller strike brings the new entries. While a side menu sits still, now
// and then a faint strike far off. A key or a click ends a strike at once
// (and does its job).

namespace {
struct Bolt { double at; float amp, decay; };   // one flash: when, how bright, how fast it dies
constexpr Bolt INTRO_BOLTS[] = {{0.0, 0.55f, 0.06f}, {0.13, 1.0f, 0.22f}, {0.36, 0.35f, 0.12f}};
constexpr Bolt SWAP_BOLTS[] = {{0.0, 0.6f, 0.16f}, {0.11, 0.25f, 0.08f}};
constexpr Bolt IDLE_BOLTS[] = {{0.0, 0.5f, 0.07f}, {0.16, 1.0f, 0.18f}};   // times strike_amp_
constexpr double INTRO_DARK = 0.35;                  // the main menu in the dark before the strike
constexpr double INTRO_LEN = INTRO_DARK + 1.3, SWAP_LEN = 0.6, IDLE_LEN = 0.9;

template <size_t N>
float flash_at(const Bolt (&bolts)[N], double t) {
    float v = 0;
    for (const Bolt& b : bolts)
        if (t >= b.at) v = std::max(v, b.amp * float(std::exp(-(t - b.at) / b.decay)));
    return v;
}

float ease_out(double x) {
    float k = 1 - float(std::clamp(x, 0.0, 1.0));
    return 1 - k * k * k;
}
}  // namespace

double Frontend::menu_clock() const {
    static const double fps = getenv("LW_MENU_FPS") ? atof(getenv("LW_MENU_FPS")) : getenv("LW_MENU_FILM") ? 30 : 0;
    return fps > 0 ? double(menu_frames_) / fps : now_seconds();
}

bool Frontend::side_menu(Screen s) {
    return s == Screen::Main || s == Screen::Play || s == Screen::Classic || s == Screen::Boss || s == Screen::Survival ||
           s == Screen::Weather;
}

void Frontend::start_strike(Strike kind) {
    // (not while a debugging aid takes screenshots: they would catch it half way)
    static const bool still = (getenv("LW_MENU_KEYS") || getenv("LW_MENU_SHOT")) && !getenv("LW_MENU_FILM");
    if (still || !has_backdrop()) return;
    strike_kind_ = kind;
    strike_from_ = menu_clock();
    strike_amp_ = kind == STRIKE_IDLE ? 0.3f + 0.25f * float(std::rand() % 1000) / 1000.0f : 1.0f;
    next_idle_strike_ = -1;
    dirty_ = true;
}

void Frontend::end_strike() {
    if (strike_from_ < 0) return;
    strike_from_ = -1;
    dirty_ = true;
}

bool Frontend::strike_light(float& shade, float& flash) {
    shade = 1, flash = 0, reveal_ = 1, glare_ = 0;
    if (strike_from_ < 0 || !side_menu(screen_)) return false;
    double t = menu_clock() - strike_from_;
    switch (strike_kind_) {
    case STRIKE_INTRO: {
        if (t >= INTRO_LEN) break;
        double k = t - INTRO_DARK;
        if (k < 0) {   // dark, the entries not there yet
            shade = 0.5f, reveal_ = 0;
            return true;
        }
        flash = flash_at(INTRO_BOLTS, k);
        shade = 0.5f + 0.5f * ease_out(k / 0.9);   // the eye getting used to the dark again
        reveal_ = k < 0.13 ? 0.4f : 0.4f + 0.6f * ease_out((k - 0.13) / 0.35);
        glare_ = flash * 0.6f;
        return true;
    }
    case STRIKE_SWAP:
        if (t >= SWAP_LEN) break;
        flash = flash_at(SWAP_BOLTS, t);
        reveal_ = t < 0.03 ? 0.0f : 0.45f + 0.55f * ease_out((t - 0.03) / 0.3);
        glare_ = flash * 0.6f;
        return true;
    case STRIKE_IDLE:
        if (t >= IDLE_LEN) break;
        flash = flash_at(IDLE_BOLTS, t) * strike_amp_;
        glare_ = flash * 0.25f;
        return true;
    }
    strike_from_ = -1;   // over
    return false;
}

// out = bg_ darkened to shade, then flash of the way to bg_lit_ (a few
// threads: the whole window, every refresh while it lasts)
void Frontend::relight(std::vector<uint32_t>& out, float shade, float flash) const {
    out.resize(bg_.size());
    const int S = int(shade * 256 + 0.5f), F = int(flash * 256 + 0.5f);
    auto rows = [&](size_t i0, size_t i1) {
        for (size_t i = i0; i < i1; i++) {
            uint32_t a = bg_[i], b = bg_lit_[i], r = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                int ca = int((a >> sh) & 0xFF), cb = int((b >> sh) & 0xFF);
                r |= uint32_t(std::clamp((ca * S + (cb - ca) * F) >> 8, 0, 255)) << sh;
            }
            out[i] = r;
        }
    };
    const size_t n = bg_.size();
    const unsigned parts = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> pool;
    for (unsigned p = 1; p < parts; p++) pool.emplace_back(rows, n * p / parts, n * (p + 1) / parts);
    rows(0, n / parts);
    for (std::thread& t : pool) t.join();
}

void Frontend::draw_menu(bool over_game, const std::vector<Item>& list) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int w = w_, h = h_;
    bool right = has_backdrop() && !over_game;   // the column at the right, clear of the picture's subject
    int left = U(110), col_w = U(720);
    int tx = right ? w - left : left, talign = right ? 2 : 0;   // where the column's text lines up
    int band0 = right ? w - left - col_w : 0, band1 = right ? w : left + col_w;
    bool boss = screen_ == Screen::Boss || screen_ == Screen::Survival;
    int y0;
    if (over_game) {
        fill(0, 0, w, h, 0x000000, 0.5f);   // the frozen game shows through, darkened
        hgrad(0, 0, U(1100), h, 0x000000, 0.75f, 0.0f);
        text(display(), U(96), screen_ == Screen::Boss ? L"BOSSES" : L"PAUSED", left, U(70), 0,
             {GOLD_TOP, GOLD_BOTTOM, EDGE, U(5), 1, U(5)});
        y0 = U(300);
    } else {
        if (!right) hgrad(0, 0, U(1100), h, 0x030504, 0.5f, 0.0f);   // the column a little darker than the stone
        if (has_logo()) {
            y0 = int((LOGO_SCREEN_TOP + LOGO_SCREEN_H) * h_) + U(40);
        } else if (has_poster()) {   // the poster is the title
            float ps, px0, py0;
            poster_layout(ps, px0, py0);
            y0 = int(py0 + POSTER_LOGO_END * poster_h_ * ps) + U(40);
        } else {
            if (jurassic_) text(JURASSIC, U(54), L"JURASSIC PARK", tx, U(84), talign, {0xD8C49A, 0xB09A70, EDGE, U(2)});
            else text(MONO, U(30), L"J U R A S S I C    P A R K", tx, U(104), talign, {0xD8C49A, 0xB09A70});
            text(display(), jurassic_ ? U(150) : U(180), L"THE LOST WORLD", tx, jurassic_ ? U(122) : U(112), talign,
                 {GOLD_TOP, GOLD_BOTTOM, EDGE, U(7), 1, U(8)});
            y0 = U(430);
        }
        const wchar_t* sub = screen_ == Screen::Play ? L"SELECT MODE" : screen_ == Screen::Survival ? L"SELECT ARENA"
                             : screen_ == Screen::Classic ? L"CLASSIC ARCADE"
                             : screen_ == Screen::Weather ? L"WEATHER"
                             : boss ? L"SELECT BOSS" : nullptr;
        if (sub) {
            text(MONO, U(24), sub, tx, y0, talign, {0xF0C070, 0xE0A050});
            y0 += U(50);
        }
    }
    if (screen_ == Screen::Main && !over_game) {   // how far the player has got, in the top corner away from the logo
        int got = 0;
        for (int64_t t : records.unlocked) got += t != 0;
        int best = *std::max_element(std::begin(records.stats.best_score), std::end(records.stats.best_score));
        std::vector<std::pair<std::wstring, std::wstring>> chips = {
            {L"ACHIEVEMENTS", std::to_wstring(got) + L"/" + std::to_wstring(achievement_table().size())}};
        if (best > 0) chips.push_back({L"BEST SCORE", grouped(best)});
        int x1 = w - U(70), cy0 = U(56), ch = U(56);
        if (right) {   // from the left edge: the total width first
            x1 = U(70) - U(16);
            for (const auto& c : chips)
                x1 += U(22) + extent(ITEM, U(21), c.first) + U(12) + extent(ITEM, U(26), c.second) + U(22) + U(16);
        }
        for (int i = int(chips.size()) - 1; i >= 0; i--) {
            int lw = extent(ITEM, U(21), chips[i].first), vw = extent(ITEM, U(26), chips[i].second);
            int cw = U(22) + lw + U(12) + vw + U(22), x0 = x1 - cw;
            fill(x0, cy0, x1, cy0 + ch, PANEL, 0.8f);
            frame(x0, cy0, x1, cy0 + ch, std::max(1, U(1.5f)), AMBER, 0.3f);
            text(ITEM, U(21), chips[i].first, x0 + U(22), cy0 + ch / 2 - U(12), 0, {PALE_TOP, PALE_BOTTOM});
            text(ITEM, U(26), chips[i].second, x1 - U(22), cy0 + ch / 2 - U(15), 2, {GOLD_TOP, GOLD_BOTTOM});
            x1 = x0 - U(16);
        }
    }

    // The entries share what is left of the height; the description of the
    // chosen one opens right under it and the entries below make room. The
    // main menu on the backdrop as Sega Rally 2's: PLAY big, the rest smaller
    int n = int(list.size());
    int desc_line = U(30), desc_pad = U(14);
    int desc_room = 3 * desc_line + desc_pad;   // up to three lines without squeezing the entries
    int spacing = std::clamp((h - U(190) - y0 - desc_room) / std::max(1, n), U(46), U(86));
    int item_px = std::min(U(60), spacing * 72 / 100);
    int desc_w = col_w - U(20);
    const bool hero_menu = right && screen_ == Screen::Main;
    auto row_h = [&](int i) { return !hero_menu ? spacing : list[size_t(i)].id == PLAY ? U(104) : U(62); };
    auto row_px = [&](int i) { return !hero_menu ? item_px : list[size_t(i)].id == PLAY ? U(92) : U(46); };

    // Animation: each description eases open or shut, the ribbon glides to the
    // chosen entry.  A new screen (or a new list) starts settled
    double now = menu_clock();
    bool snap = anim_screen_ != screen_ || int(desc_open_.size()) != n || anim_last_ < 0 || now - anim_last_ > 0.25;
    float dt = snap ? 0.0f : float(now - anim_last_);
    anim_last_ = now, anim_screen_ = screen_;
    desc_open_.resize(size_t(n), 0.0f);
    float ease = 1.0f - std::exp(-dt * 16.0f);
    for (int i = 0; i < n; i++) {
        float target = i == selected_ ? 1.0f : 0.0f;
        float& o = desc_open_[size_t(i)];
        o = snap ? target : o + (target - o) * ease;
        if (std::fabs(target - o) < 0.003f) o = target;
        else animating_ = true;
    }
    std::vector<std::vector<std::wstring>> desc(static_cast<size_t>(n));
    std::vector<int> row_y(static_cast<size_t>(n));
    int y = y0;
    for (int i = 0; i < n; i++) {
        row_y[size_t(i)] = y;
        y += row_h(i);
        if (desc_open_[size_t(i)] > 0.0f && !list[size_t(i)].hint.empty()) {
            desc[size_t(i)] = wrap(BODY, U(23), list[size_t(i)].hint, desc_w);
            float o = desc_open_[size_t(i)], k = o * o * (3.0f - 2.0f * o);   // smoothstep
            y += int(k * float(int(desc[size_t(i)].size()) * desc_line + desc_pad) + 0.5f);
        }
    }
    int y_end = y;

    // The highlight, gliding from the last entry to the chosen one: on the
    // backdrop Sega Rally 2's slanted ribbon (in amber) with a thin stripe
    // before it, over the game a band
    bool settled = true;
    if (selected_ < n) {
        float target = float(row_y[size_t(selected_)]), target_h = float(row_h(selected_));
        float k = 1.0f - std::exp(-dt * 20.0f);
        band_y_ = snap || band_y_ < 0 ? target : band_y_ + (target - band_y_) * k;
        band_h_ = snap || band_h_ < 0 ? target_h : band_h_ + (target_h - band_h_) * k;
        if (std::fabs(target - band_y_) < 0.5f && std::fabs(target_h - band_h_) < 0.5f) band_y_ = target, band_h_ = target_h;
        else animating_ = true, settled = false;
        int by = int(band_y_ + 0.5f), bh = int(band_h_ + 0.5f);
        bool en = list[size_t(selected_)].enabled;
        float a = (en ? 1.0f : 0.45f) * reveal_;
        int edge = std::max(3, U(8));
        if (right) {   // as in Star Wars Trilogy: a straight band rising out of nothing to the right edge, a bright blade along it
            int x0 = hero_menu ? tx - U(620) : band0;
            hgrad(x0, by, w, by + bh, AMBER, 0.0f, a * 0.55f);
            fill(w - edge - U(5), by, w, by + bh, AMBER, a * 0.75f);
            fill(w - edge, by, w, by + bh, 0xFFE3A8, a);
        } else {
            hgrad(band0, by, band1, by + bh, AMBER, a * 0.34f, 0.0f);
            fill(0, by, edge, by + bh, AMBER, a);
        }
    } else band_y_ = band_h_ = -1;

    for (int i = 0; i < n; i++) {
        const Item& it = list[i];
        bool sel = i == selected_;
        int ry = row_y[size_t(i)], rh = row_h(i), px = row_px(i);
        // Lit by a strike: the letters whiter for a moment, the entries coming on
        auto lit = [&](uint32_t c) { return glare_ > 0 ? mix(c, 0xFFFFFF, glare_) : c; };
        const Style st = !it.enabled ? Style{DIM, DIM, INK, U(2), 0.9f * reveal_, U(3)}
                         : sel       ? Style{lit(WHITE_TOP), lit(WHITE_BOTTOM), INK, U(2), reveal_, U(3)}
                                     : Style{lit(PALE_TOP), lit(PALE_BOTTOM), INK, U(2), 0.95f * reveal_, U(3)};
        // The capitals' ink centred in the row (and so in the ribbon), whatever
        // the font: every entry by the same capital, so they all sit alike
        int cap0, cap1;
        ink_rows(TITLE, px, L"H", cap0, cap1);
        int ty = ry + rh / 2 - (cap0 + cap1) / 2;
        int tw = reveal_ > 0 ? text(TITLE, px, it.label, tx, ty, talign, st) : measure(TITLE, px, it.label);
        bool chevron = sel && right && it.enabled && settled;   // before the chosen one, once the ribbon is there
        if (chevron && reveal_ > 0) triangle(tx - tw - U(28), ry + rh / 2, U(22), +1, 0xFFE08A, 0.95f * reveal_);
        if (!it.value.empty() && reveal_ > 0) {   // e.g. the stage of a boss, "NOT READY" or a record
            int v0, v1;
            ink_rows(ITEM, U(24), L"H", v0, v1);
            text(ITEM, U(24), it.value, right ? tx - tw - U(chevron ? 70.0f : 30.0f) : left + col_w - U(60),
                 ry + rh / 2 - (v0 + v1) / 2, 2, {0xF4D27A, 0xE0A848, 0, 0, reveal_});
        }
        hit_.push_back({band0, ry, band1, ry + rh});
        values_.push_back({-1, 0, 0});
        // Its description: fading in as it opens, sliding down a little; the
        // lines that do not fit the opening yet are left out
        float o = desc_open_[size_t(i)];
        if (o > 0.02f && !desc[size_t(i)].empty() && reveal_ > 0) {
            float fade = (sel ? o * o : o * o * o) * reveal_;   // a closing one goes quicker
            int room = (i + 1 < n ? row_y[size_t(i) + 1] : y_end) - (ry + rh);
            int dy = ry + rh + U(2) - int((1.0f - o) * U(10));
            for (const std::wstring& line : desc[size_t(i)]) {
                if (dy + desc_line - (ry + rh) > room + U(6)) break;
                text(BODY, U(23), line, tx, dy, talign, {0xE8C890, 0xE8C890, 0, 0, fade});
                dy += desc_line;
            }
        }
    }
    y = y_end;
    if (starting && n > 0 && reveal_ > 0) {   // LOADING: a light going to and fro under its line (as Virtua Cop 2's)
        const int ly = row_y[0] + row_h(0) + U(2) + int(desc[0].size()) * desc_line + U(14);
        const int span = U(600), half = U(60), x0 = right ? tx - span : tx;
        const double k = 0.5 - 0.5 * std::cos(menu_clock() * 2.6);
        const int cx = x0 + half + int(k * double(span - 2 * half));
        const int glow = std::max(4, U(14)), core = std::max(1, U(1.6f));
        for (int lx = cx - half; lx < cx + half; lx++) {
            float a = std::min(1.0f, (1.0f - std::fabs(float(lx - cx)) / float(half)) * 3.0f) * reveal_;
            for (int dy = -glow; dy <= glow; dy++) {
                float d = std::fabs(float(dy)) / float(glow);
                blend(lx, ly + dy, AMBER, a * 0.55f * std::exp(-d * d * 5.0f));
                if (std::abs(dy) <= core) blend(lx, ly + dy, 0xFFE3A8, a);
            }
        }
        animating_ = true;
    }

    vgrad(0, h - U(120), w, h, 0x000000, 0.0f, 0.85f);
    const wchar_t* back_what = starting ? L"BACK" : screen_ == Screen::Main ? nullptr : screen_ == Screen::Pause ? L"RESUME" : L"BACK";
    int py = h - U(64), x = U(80);
    if (!over_game) {   // the key help at the bottom right; at the left, the credit to the makers of the game
        x = w - U(80) + U(44) - (starting ? 0 : prompt_width(L"A", L"ENTER", L"SELECT")) -
            (back_what ? prompt_width(L"B", L"ESC", back_what) : 0);
        if (sega_.w > 0) {   // their logo, its own transparency kept
            int lh = U(52), lw = lh * sega_.w / sega_.h, lx = U(80), ly = h - U(46) - lh;
            for (int j = 0; j < lh; j++)
                for (int i = 0; i < lw; i++) {
                    uint32_t q = sega_.px[size_t(j * sega_.h / lh) * sega_.w + size_t(i * sega_.w / lw)];
                    blend(lx + i, ly + j, q & 0xFFFFFF, float(q >> 24) / 255.0f);
                }
            text(ITEM, U(18), L"\u00A9 1997 SEGA", lx + lw + U(16), ly + lh / 2 - U(10), 0, {FOOT, FOOT});
        } else {
            text(ITEM, U(22), L"ORIGINAL ARCADE GAME \u00A9 1997 SEGA", U(80), py + U(6), 0, {0xB8B09A, 0xA09884});
        }
    }
    if (!over_game && screen_ == Screen::Main && credit_.w > 0) {   // the remaster's credit, small at the bottom centre
        int lw = U(270), lh = lw * credit_.h / credit_.w, ly = h - U(36) - lh;
        sprite(credit_, (w - lw) / 2, ly, lw, lh);
        text(ITEM, U(18), L"PC REMASTER BY", w / 2, ly - U(28), 1, {FOOT, FOOT});
    }
    if (!starting) prompt(x, py, L"A", L"ENTER", L"SELECT");
    if (back_what) back_rect_ = prompt(x, py, L"B", L"ESC", back_what);
}


bool Frontend::draw(int w, int h, std::vector<uint32_t>& out) {
    if (!active() || w <= 0 || h <= 0) return false;
    // Now and then, while a side menu sits still, a faint strike far off
    static const bool idle_off = getenv("LW_MENU_KEYS") || getenv("LW_MENU_SHOT") || getenv("LW_MENU_FILM");
    if (side_menu(screen_) && !over_game() && has_backdrop() && !idle_off && !locked_notice_) {
        double now = menu_clock();
        if (strike_from_ >= 0) next_idle_strike_ = -1;
        else if (next_idle_strike_ < 0) next_idle_strike_ = now + 12.0 + 14.0 * (std::rand() % 1000) / 1000.0;
        else if (now >= next_idle_strike_) start_strike(STRIKE_IDLE);
    } else {
        next_idle_strike_ = -1;
    }
    if (!dirty_ && w == w_ && h == h_ && out.size() == size_t(w) * h) return false;
    if (w != w_ || h != h_) {
        w_ = w;
        h_ = h;
        u_ = std::min(h / 1080.0f, w / 1440.0f);
        bg_.clear();
        bg_stone_.clear();
        bg_lit_.clear();
    }
    partial_ = false;
    gliding_ = false;
    menu_frames_++;
    if (screen_ == Screen::Splash) {
        if (draw_splash(out)) {
            dirty_ = true;   // it fades: drawn again every refresh
            return true;
        }
        open(after_splash);   // the last one ended
        if (!active()) return false;
    }
    if (screen_ != Screen::GunTest) gun_page_.clear();
    else if (gun_page_.size() == size_t(w) * h && out.size() == gun_page_.size() && gun_page_key_ == gun_key()) {
        // Only the marks moved: put the page back where they were, draw them where they are
        px_ = out.data();
        int y0 = h, y1 = 0;
        auto restore = [&](const RECT& r) {
            int x0 = std::max(0, int(r.left)), x1 = std::min(w, int(r.right));
            int r0 = std::max(0, int(r.top)), r1 = std::min(h, int(r.bottom));
            if (x0 >= x1 || r0 >= r1) return;
            for (int y = r0; y < r1; y++)
                memcpy(&out[size_t(y) * w + x0], &gun_page_[size_t(y) * w + x0], size_t(x1 - x0) * 4);
            y0 = std::min(y0, r0), y1 = std::max(y1, r1);
        };
        for (const RECT& r : gun_marks_) restore(r);
        draw_gun_marks();
        for (const RECT& r : gun_marks_) y0 = std::min(y0, std::max(0, int(r.top))), y1 = std::max(y1, std::min(h, int(r.bottom)));
        partial_ = true;
        partial_y0_ = y0, partial_y1_ = std::max(y0, y1);
        dirty_ = false;
        return true;
    }
    bool over_game = this->over_game();
    bool page = screen_ == Screen::Options || screen_ == Screen::Records || screen_ == Screen::GunTest;
    float shade = 1, flash = 0;
    const bool lit = strike_light(shade, flash);   // (also the entries' reveal_ and glare_)
    if (lit) animating_ = true;   // drawn again every refresh while it lasts
    if (over_game) out.assign(size_t(w) * h, 0);
    else {
        if (bg_.empty()) background();
        if (lit && !page && bg_lit_.size() == bg_.size()) relight(out, shade, flash);
        else out = page && !bg_stone_.empty() ? bg_stone_ : bg_;   // the page: on the backdrop blurred
    }
    px_ = out.data();
    hit_.clear();
    values_.clear();
    more_up_ = more_down_ = back_rect_ = reset_rect_ = RECT{};
    auto list = items();
    if (screen_ == Screen::GunTest) {
        draw_gun_test(over_game);
        gun_page_ = out;   // without the marks
        gun_page_key_ = gun_key();
        draw_gun_marks();
    } else if (page) draw_options_page(over_game, list);
    else if (starting && !over_game) {   // LOADING (as Virtua Cop 2's): the main menu's look, LOADING where PLAY was
        const Screen keep_screen = screen_;
        const int keep = selected_;
        screen_ = Screen::Main, selected_ = 0;
        draw_menu(false, {{.id = PLAY, .label = L"LOADING", .hint = L"The credit goes in and the game starts: pick your route"}});
        screen_ = keep_screen, selected_ = keep;
        hit_.clear();   // (nothing to click)
        values_.clear();
    } else draw_menu(over_game, list);
    if (locked_notice_ && screen_ == Screen::Survival) draw_locked_notice();
    if (restart_shown()) draw_restart();
    if (drop_open_ && screen_ == Screen::Options) draw_drop();
    dirty_ = (animating_ && !page && screen_ != Screen::GunTest) || gliding_;   // still moving: again next refresh
    animating_ = false;
    return true;
}
