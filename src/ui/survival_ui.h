// Screens of the survival mode: the loading and start panel with the rules and
// the record, the lap and wave counters, banners and the golden BONUS target
// during the hunt, and the results screen that invites to play again.
//
// Drawn on the CPU into a full-window BGRA picture (premultiplied alpha) with
// the same OFL fonts and letter colours as the menus (ui/frontend.cpp);
// video/ui_overlay.h puts it on screen, under the pause menu.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../runtime/survival.h"
#include "ui_canvas.h"

// Where the 4:3 game screen is in the window (ScreenLayout)
struct SurvivalLayout {
    float scale = 1, ox = 0, oy = 0, game_x0 = 0;
};

class SurvivalUi : UiCanvas {
public:
    bool init(const std::string& font_dir);
    void shutdown();
    // Redraws into `out` when the picture changes; true if it did. `now` in
    // seconds (same clock as SurvivalView::wave_started)
    bool draw(int w, int h, const SurvivalView& v, double now, std::vector<uint32_t>& out,
              const SurvivalLayout& layout = {});

private:
    enum Family { TITLE, TEXT, NUMBER };   // NUMBER: titles with digits (the Jurassic font has none)
    // Text with the top of its box at y; align 0 left, 1 centre, 2 right.
    // Gradient from top to bottom colour, with a dark drop shadow
    int text(Family f, int px, const std::wstring& s, int x, int y, int align, uint32_t top, uint32_t bottom,
             float alpha = 1);
    void panel(int x0, int y0, int x1, int y1, float alpha);
    void bar(int x0, int y0, int x1, int y1, float fraction);

    void draw_intro(const SurvivalView& v, double now);
    void draw_hud(const SurvivalView& v, double now);
    void draw_bonus(const SurvivalView& v, const SurvivalLayout& layout, double now);
    void ring(float x, float y, float r, float width, uint32_t rgb, float alpha);
    void draw_results(const SurvivalView& v, double now);

    GdiText* gdi_ = nullptr;
    bool jurassic_ = false;
    float u_ = 1;   // pixels per unit of a 1920x1080 layout
    std::wstring last_key_;   // what the last picture showed
};
