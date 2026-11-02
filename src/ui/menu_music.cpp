#include "menu_music.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

bool mci(const std::wstring& command) { return mciSendStringW(command.c_str(), nullptr, 0, nullptr) == 0; }

}  // namespace

bool MenuMusic::open() {
    for (const wchar_t* path : {L"third_party/menu/music.mp3", L"third_party/menu/music.wav", L"res/menu/music.mp3"}) {
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
        // mpegvideo plays MP3 and WAV alike and can loop ("repeat")
        if (mci(L"open \"" + std::wstring(path) + L"\" type mpegvideo alias lw_menu_music")) {
            printf("[menu] music %ls\n", path);
            open_ = true;
            return true;
        }
        fprintf(stderr, "[menu] could not open the music %ls\n", path);
    }
    return false;
}

void MenuMusic::close() {
    if (open_) mci(L"close lw_menu_music");
    open_ = playing_ = false;
}

void MenuMusic::update(bool want, int volume, double dt) {
    if (!open_) return;
    // A second to come in, half a second to go when the game starts
    level_ = want ? std::min(1.0f, level_ + float(dt) / 1.0f) : std::max(0.0f, level_ - float(dt) / 0.5f);
    if (want && !playing_) {
        sent_ = -1;
        playing_ = mci(L"play lw_menu_music repeat");
    }
    if (!playing_) return;
    if (level_ <= 0) {   // faded out: it waits where it was
        mci(L"pause lw_menu_music");
        playing_ = false;
        return;
    }
    // 0..1000; the game's volume 5 (the default) plays it at half
    int v = int(std::clamp(level_ * float(volume) / 10.0f, 0.0f, 1.0f) * 1000.0f + 0.5f);
    if (v != sent_) {
        mci(L"setaudio lw_menu_music volume to " + std::to_wstring(v));
        sent_ = v;
    }
}
