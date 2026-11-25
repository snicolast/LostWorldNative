#include "gdi_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const char* const FILES[GdiText::FACE_COUNT] = {"SairaExtraCondensed-Bold.ttf", "BarlowCondensed-Bold.ttf",
                                                "ShareTechMono-Regular.ttf", "Jurassic_Park.ttf", "Arimo[wght].ttf",
                                                "BarlowCondensed-SemiBold.ttf", "BarlowCondensed-Medium.ttf",
                                                "Barlow-Regular.ttf"};
const wchar_t* const FACES[GdiText::FACE_COUNT] = {L"Saira ExtraCondensed ExCond", L"Barlow Condensed",
                                                   L"Share Tech Mono", L"Jurassic Park", L"Arimo",
                                                   L"Barlow Condensed SemiBold", L"Barlow Condensed Medium", L"Barlow"};
const int WEIGHTS[GdiText::FACE_COUNT] = {FW_BOLD, FW_BOLD, FW_NORMAL, FW_NORMAL, FW_NORMAL, FW_SEMIBOLD, FW_MEDIUM, FW_NORMAL};

std::vector<char> read_file(const std::string& path) {
    std::vector<char> data;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        data.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        fclose(f);
    }
    return data;
}

}  // namespace

const char* GdiText::file(Face f) { return FILES[f]; }

GdiText& GdiText::acquire(const std::string& font_dir) {
    static GdiText text;
    if (text.users_++ == 0) {
        for (int f = 0; f < FACE_COUNT; f++) {
            text.font_data_.push_back(read_file(font_dir + "/" + FILES[f]));
            auto& data = text.font_data_.back();
            DWORD count = 0;
            HANDLE h = data.empty() ? nullptr : AddFontMemResourceEx(data.data(), DWORD(data.size()), nullptr, &count);
            if (h) text.font_handles_.push_back(h);
            text.loaded_[f] = h != nullptr;
        }
        text.dc_ = CreateCompatibleDC(nullptr);
    }
    return text;
}

void GdiText::release() {
    if (users_ == 0 || --users_ > 0) return;
    for (auto& [key, f] : fonts_) DeleteObject(f);
    fonts_.clear();
    masks_.clear();
    for (HANDLE h : font_handles_) RemoveFontMemResourceEx(h);
    font_handles_.clear();
    font_data_.clear();
    std::fill(std::begin(loaded_), std::end(loaded_), false);
    if (dc_) DeleteDC(dc_);
    dc_ = nullptr;
}

HFONT GdiText::font(Face f, int px) {
    auto key = std::make_pair(int(f), px);
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second;
    HFONT h = CreateFontW(-px, 0, 0, 0, WEIGHTS[f], FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                          FACES[f]);
    fonts_[key] = h;
    return h;
}

const GdiText::Mask& GdiText::mask(Face f, int px, const std::wstring& s, int edge_px) {
    auto key = std::make_tuple(int(f), px, edge_px, s);
    auto it = masks_.find(key);
    if (it != masks_.end()) return it->second;
    if (masks_.size() > 400) masks_.clear();   // window resized many times
    Mask& m = masks_[key];
    if (!dc_) return m;
    SelectObject(dc_, font(f, px));
    std::wstring t = s;   // the Jurassic font's space is very narrow: words ran together
    if (f == JURASSIC) {
        t.clear();
        for (wchar_t c : s) t += c == L' ' ? std::wstring(L"   ") : std::wstring(1, c);
    }
    SIZE sz{};
    GetTextExtentPoint32W(dc_, t.c_str(), int(t.size()), &sz);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc_, &tm);
    m.pad = edge_px + 2 + px / 6;   // room for the outline and overhanging letters
    m.text_w = sz.cx;
    m.text_h = sz.cy;
    m.w = sz.cx + 2 * m.pad;
    m.h = sz.cy + 2 * m.pad;
    m.ascent = tm.tmAscent;
    m.grad0 = m.pad + tm.tmInternalLeading + (tm.tmAscent - tm.tmInternalLeading) / 8;
    m.grad1 = m.pad + tm.tmAscent;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = m.w;
    bi.bmiHeader.biHeight = -m.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return m;
    HGDIOBJ old = SelectObject(dc_, bmp);
    memset(bits, 0, size_t(m.w) * m.h * 4);
    SetBkMode(dc_, TRANSPARENT);
    SetTextColor(dc_, RGB(255, 255, 255));
    TextOutW(dc_, m.pad, m.pad, t.c_str(), int(t.size()));
    GdiFlush();
    m.a.resize(size_t(m.w) * m.h);
    const uint8_t* p = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < m.a.size(); i++) m.a[i] = p[i * 4 + 1];
    SelectObject(dc_, old);
    DeleteObject(bmp);
    if (edge_px > 0) {   // outline: the letters grown by a round brush
        m.e.assign(m.a.size(), 0);
        float r = float(edge_px);
        for (int dy = -edge_px - 1; dy <= edge_px + 1; dy++)
            for (int dx = -edge_px - 1; dx <= edge_px + 1; dx++) {
                float wgt = std::clamp(r + 1 - std::sqrt(float(dx * dx + dy * dy)), 0.0f, 1.0f);
                if (wgt <= 0) continue;
                for (int y = std::max(0, -dy); y < std::min(m.h, m.h - dy); y++) {
                    const uint8_t* src = &m.a[size_t(y + dy) * m.w];
                    uint8_t* dst = &m.e[size_t(y) * m.w];
                    for (int x = std::max(0, -dx); x < std::min(m.w, m.w - dx); x++) {
                        uint8_t v = uint8_t(float(src[x + dx]) * wgt);
                        if (v > dst[x]) dst[x] = v;
                    }
                }
            }
    }
    return m;
}

int GdiText::extent(Face f, int px, const std::wstring& s) {
    SelectObject(dc_, font(f, px));
    SIZE sz{};
    GetTextExtentPoint32W(dc_, s.c_str(), int(s.size()), &sz);
    return sz.cx;
}
