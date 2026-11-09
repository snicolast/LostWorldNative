// The notice of a new achievement: a small panel that slides in from the
// right edge to the bottom right corner with a short chime, stays a few
// seconds and slides out, over the game and the menus, one achievement after
// another. Drawn on the CPU like the survival screens (ui/survival_ui.h) and
// put on screen by video/ui_overlay.h.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "ui_canvas.h"

class AchievementToast : UiCanvas {
public:
    bool init(const std::string& font_dir);
    void shutdown();
    void push(const std::wstring& name, const std::wstring& text);
    // Another kind of notice: its own label, and it rises from the bottom middle
    void push_rising(const std::wstring& label, const std::wstring& name, const std::wstring& text);
    // The author's credit, two small lines in the bottom right corner, faded
    // in while `shown` (the attract and its INSERT COIN, with the menu off)
    void set_credit(const std::wstring& line1, const std::wstring& line2, bool shown) {
        credit_[0] = line1, credit_[1] = line2, credit_on_ = shown;
    }
    void set_volume(float v) { volume_ = v; }   // the game's gain, 0..10 (5 default); 0 = no chime
    // Redraws into `out` when the picture changes; true if it did. `now` in seconds
    bool draw(int w, int h, double now, std::vector<uint32_t>& out);

private:
    enum Family { TITLE, TEXT };
    const GdiText::Mask& mask(Family f, int px, const std::wstring& s);
    int text(Family f, int px, const std::wstring& s, int x, int y, int align, uint32_t top, uint32_t bottom,
             float alpha);
    void shine(int x0, int y0, int x1, int y1, float pos);
    void chime();
    RECT draw_credit(float alpha);

    struct Notice {
        std::wstring name, text;
        std::wstring label = L"ACHIEVEMENT UNLOCKED";
        bool rising = false;
    };
    std::deque<Notice> queue_;
    double shown_at_ = -1;   // when the front notice appeared
    bool drawn_ = false;     // the last picture has a panel
    RECT dirty_{};           // where it was
    float volume_ = 5;
    std::vector<uint8_t> wav_;   // the chime, played from memory
    std::wstring credit_[2];
    bool credit_on_ = false;
    float credit_alpha_ = 0;     // fades towards credit_on_
    double credit_at_ = -1;      // the last draw's time, for the fade
    RECT credit_dirty_{};        // where the credit was drawn

    GdiText* gdi_ = nullptr;
    bool jurassic_ = false;
};
