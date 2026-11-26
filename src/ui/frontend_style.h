// What the menu's files share (frontend.cpp, frontend_draw.cpp,
// frontend_page.cpp, frontend_screens.cpp, frontend_gun_test.cpp): its
// colours, a picture loader and the records' number format. Internal to them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ui_canvas.h"

namespace fe {

// The game's letters: yellow at the top fading to orange, dark outline
constexpr uint32_t GOLD_TOP = 0xFFEA5C, GOLD_BOTTOM = 0xFF8A00, EDGE = 0x1C0A00;
constexpr uint32_t PALE_TOP = 0xE6DCC0, PALE_BOTTOM = 0xBFB294, DIM = 0x6A6559;
constexpr uint32_t AMBER = 0xFFA032, HINT = 0xC9B88E, FOOT = 0x8A8070;
// Screens and the key help
constexpr uint32_t WHITE_TOP = 0xFFF6E0, WHITE_BOTTOM = 0xFFE2A8, INK = 0x080806, PANEL = 0x090D0B;
constexpr uint32_t PAD_A = 0x6FD17A, PAD_B = 0xE8655A, PAD_Y = 0xE8C85A, PAD_X = 0x5AA8E8, KEYCAP = 0xD8D2C0;

using ui::chan;
using ui::mix;
using ui::over;

// Any picture format Windows can read (WIC), as BGRA (nothing when it fails; frontend.cpp)
bool load_image(const std::wstring& path, std::vector<uint32_t>& px, int& w_out, int& h_out);
// 12,345 (frontend.cpp)
std::wstring grouped(int64_t v);

}  // namespace fe

using namespace fe;
