// Text for the pictures drawn on the CPU (ui/frontend.h, ui/survival_ui.h,
// ui/achievement_toast.h): the OFL fonts in third_party/fonts, loaded once for
// the whole program, and each text turned into a coverage mask with GDI. One
// for all of them (they all draw on the window's thread), so the masks are
// shared too.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

class GdiText {
public:
    // Saira and Barlow are bold; Jurassic Park is optional (free for personal
    // use, not in the repository) and without Arimo Windows' own sans-serif
    // stands in. The last three are the settings page's (as Sega Rally 2's):
    // its rows, its group headings and its descriptions
    enum Face { SAIRA, BARLOW, MONO, JURASSIC, ARIMO, BARLOW_SEMIBOLD, BARLOW_MEDIUM, BARLOW_REGULAR, FACE_COUNT };
    struct Mask {   // text coverage (0..255) with `pad` pixels around the text box
        int w = 0, h = 0, pad = 0, text_w = 0, text_h = 0;
        int ascent = 0;             // the font's, from the top of the text box to the baseline
        int grad0 = 0, grad1 = 0;   // rows of a letter gradient (top of the capitals to the baseline)
        std::vector<uint8_t> a, e;  // letters, letters grown by the outline width
    };

    // The one for everybody: the first call loads the fonts in font_dir.
    // Each acquire has its release; the last one frees them
    static GdiText& acquire(const std::string& font_dir);
    void release();

    bool ready() const { return dc_ != nullptr; }
    bool loaded(Face f) const { return loaded_[f]; }
    static const char* file(Face f);   // its file in the fonts folder
    HFONT font(Face f, int px);
    // The mask of a text; edge_px > 0 also makes its outline (Mask::e)
    const Mask& mask(Face f, int px, const std::wstring& s, int edge_px = 0);
    int extent(Face f, int px, const std::wstring& s);   // GDI's width of the text as it is

private:
    int users_ = 0;
    bool loaded_[FACE_COUNT] = {};
    std::vector<HANDLE> font_handles_;
    std::vector<std::vector<char>> font_data_;
    std::map<std::pair<int, int>, HFONT> fonts_;
    std::map<std::tuple<int, int, int, std::wstring>, Mask> masks_;
    HDC dc_ = nullptr;
};
