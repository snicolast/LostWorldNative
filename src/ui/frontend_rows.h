// The settings table's rows (Frontend::Setting): what each setting keeps in
// save/settings.ini and how its row in the options shows and changes it.
// Private to the menu: frontend.cpp (the rows), frontend_table.cpp (the
// table) and frontend_settings.cpp (the file), which the tests build without
// the rest of the menu.
#pragma once
#include "frontend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

// One setting: how save/settings.ini keeps it, and its row in the options
// (the tab, how it shows and how Left / Right change it). The table is the one
// place to put a new option; settings_bridge.cpp then hands it to the game
// (options_from_settings) and back (settings_from_options).
struct Frontend::Setting {
    Id id;                    // its row (NO_ROW: only kept in the file)
    int tab = GAME_TAB;
    const wchar_t* label = L"";
    const char* key = nullptr;                        // in the file (nullptr: not kept)
    bool FrontendSettings::*flag = nullptr;           // what it sets: on / off,
    int FrontendSettings::*number = nullptr;          // a number
    std::string FrontendSettings::*path = nullptr;    // or a device path ("none" in the file when empty)
    // In the file a flag is 0 / 1 and a number is itself, unless there are
    // words: then false / true or 0, 1, 2... A word not among them reads as
    // `other`, or as one of `also` (older or looser spellings)
    std::vector<const char*> words = {};
    int other = 0;
    std::vector<std::pair<const char*, int>> also = {};
    int lo = 0, hi = 0;       // a number's range
    int grain = 1;            // and the steps it is read in (REAR LEVEL: 3 dB)
    bool hundredths = false;  // kept as a decimal (GAMMA 1.00)
    // The row
    Kind kind = PLAIN;
    std::vector<const wchar_t*> shown = {};   // CHOICE: each value's name; BUTTON: what it says
    std::vector<int> steps = {};              // CHOICE or SLIDER over these numbers
    int step = 1;                             // SLIDER: how far Left / Right go
    bool levels = false;                      // CHOICE: OFF, then higher and higher
    std::wstring (*text)(int) = nullptr;      // how the number reads (instead of `shown` or the number)
    const wchar_t* hint = L"";
    std::vector<const wchar_t*> hints = {};   // a hint for each value instead

    int get(const FrontendSettings& s) const { return flag ? int(s.*flag) : number ? s.*number : 0; }
    void set(FrontendSettings& s, int v) const {
        if (flag) s.*flag = v != 0;
        else if (number) s.*number = v;
    }
    // CHOICE: how many values, and which one is on
    int count() const { return int(steps.empty() ? shown.size() : steps.size()); }
    int pos(const FrontendSettings& s) const {
        if (steps.empty()) return get(s);
        return int(std::find(steps.begin(), steps.end(), get(s)) - steps.begin()) % count();
    }

    void read(FrontendSettings& s, const std::string& v) const {
        if (path) {
            s.*path = v == "none" ? "" : v;
        } else if (!words.empty()) {
            int n = other;
            auto w = std::find_if(words.begin(), words.end(), [&](const char* x) { return v == x; });
            if (w != words.end()) n = int(w - words.begin());
            else
                for (const auto& [word, value] : also)
                    if (v == word) n = value;
            set(s, n);
        } else if (flag) {
            s.*flag = v == "1" || v == "on" || v == "true";
        } else if (hundredths) {
            s.*number = std::clamp(int(atof(v.c_str()) * 100 + 0.5), lo, hi);
        } else {
            s.*number = std::clamp(atoi(v.c_str()) / grain * grain, lo, hi);
        }
    }
    std::string write(const FrontendSettings& s) const {
        if (path) return (s.*path).empty() ? "none" : s.*path;
        if (!words.empty()) return words[std::clamp(get(s), 0, int(words.size()) - 1)];
        char out[32];
        if (hundredths) snprintf(out, sizeof(out), "%.2f", get(s) / 100.0);
        else snprintf(out, sizeof(out), "%d", get(s));
        return out;
    }

    // Left (-1) or Right (+1) on the row; `wrap`: a click on the label, which
    // goes round past the top of a slider
    void change(FrontendSettings& s, int dir, bool wrap) const {
        int v = get(s);
        if (kind == TOGGLE) {
            set(s, !v);
        } else if (kind == CHOICE) {
            int p = (pos(s) + dir + count()) % count();
            set(s, steps.empty() ? p : steps[p]);
        } else if (kind == SLIDER && !steps.empty()) {   // the next step from the nearest one at or below, round
            int n = int(steps.size()), i = 0;
            while (i < n - 1 && steps[i + 1] <= v) i++;
            set(s, steps[(i + dir + n) % n]);
        } else if (kind == SLIDER) {
            set(s, dir > 0 && v >= hi && wrap ? lo : std::clamp(v + step * dir, lo, hi));
        }
    }
};

// How the rows' numbers read, and the words the file keeps for levels
namespace setting_text {
inline std::wstring percent(int v) { return std::to_wstring(v) + L"%"; }
inline std::wstring percent_or_off(int v) { return v ? percent(v) : std::wstring(L"OFF"); }
inline std::wstring decimal(int v) {   // GAMMA
    wchar_t text[16];
    swprintf(text, 16, L"%.2f", v / 100.0);
    return text;
}
inline std::wstring decibels(int v) { return (v > 0 ? L"+" : L"") + std::to_wstring(v) + L" DB"; }
inline std::wstring seconds(int v) { return v ? L"AFTER " + std::to_wstring(v) + L" S" : std::wstring(L"NEVER"); }

inline const std::vector<const char*> LEVEL_WORDS = {"off", "low", "medium", "high"};
inline const std::vector<const wchar_t*> LEVELS = {L"OFF", L"LOW", L"MEDIUM", L"HIGH"};
inline const std::vector<std::pair<const char*, int>> ON_IS_2 = {{"1", 2}, {"on", 2}, {"true", 2}};   // once a switch
}  // namespace setting_text
