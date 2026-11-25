// save/settings.ini (frontend_settings.h): read and written through the
// settings table (frontend_table.cpp), with the few keys that have a format
// of their own.
#include "frontend_settings.h"

#include <cstring>

#include "frontend_rows.h"

namespace {
// Only in the file's first section: they belong to the whole game, not to an experience
bool whole_game_key(const char* key) {
    static const char* const KEYS[] = {"experience", "gun_p1", "gun_p2", "splash_screens", "run_weather"};
    for (const char* k : KEYS)
        if (strcmp(k, key) == 0) return true;
    return false;
}
}  // namespace

bool FrontendSettings::load(const char* path, const char* section) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    const bool top = !section || !*section;
    bool found = top;
    std::string in_section;   // "" the first section, else the name of the last [name] line
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char name[64];
        if (sscanf(line, " [%63[^]]]", name) == 1) {
            in_section = name;
            found |= in_section == section;
            continue;
        }
        if (top ? !in_section.empty() : in_section != section) continue;
        char key[64], value[400];   // device paths are long
        if (sscanf(line, " %63[^= ] = %399s", key, value) != 2) continue;
        if (!top && whole_game_key(key)) continue;
        for (const Frontend::Setting& d : Frontend::setting_table())
            if (d.key && strcmp(d.key, key) == 0) d.read(*this, value);
        if (strcmp(key, "aa") == 0) aa_saved = true;   // (the first start chooses it, app.cpp)
        if (strcmp(key, "display_mode") == 0) display_mode_saved = true;
        if (strcmp(key, "max_fps") == 0) {
            max_fps = strcmp(value, "unlimited") == 0 ? -1 : strcmp(value, "monitor") == 0 ? 0 : std::clamp(atoi(value), 0, 1000);
            max_fps_saved = true;
        }
        if (strcmp(key, "half_rate") == 0) half_rate = strcmp(value, "1") == 0 || strcmp(value, "on") == 0;
        if (strcmp(key, "resolution") == 0 && (sscanf(value, "%dx%d", &res_w, &res_h) != 2 || res_w <= 0 || res_h <= 0))
            res_w = res_h = 0;
    }
    fclose(f);
    if (!display_mode_saved) display_mode = fullscreen ? 1 : 0;   // an older version's file: its full screen was borderless
    if (!max_fps_saved && half_rate) max_fps = 60;   // ...its FRAME RATE: HALF (every other refresh above 100 Hz)
    return found;
}

std::string FrontendSettings::text(bool section) const {
    FrontendSettings s = *this;
    s.fullscreen = display_mode != 0;   // (for older versions)
    std::string out;
    for (const Frontend::Setting& d : Frontend::setting_table())
        if (d.key && !(section && whole_game_key(d.key))) out += std::string(d.key) + " = " + d.write(s) + "\n";
    out += "resolution = " + std::to_string(res_w) + "x" + std::to_string(res_h) + "\n";
    out += "max_fps = " + (max_fps < 0 ? std::string("unlimited") : max_fps == 0 ? std::string("monitor") : std::to_string(max_fps)) + "\n";
    return out;
}

void FrontendSettings::save_both(const char* path, const FrontendSettings& pc, const FrontendSettings* arcade,
                                 const FrontendSettings& whole) {
    FrontendSettings top = pc;
    top.copy_whole_game(whole);
    std::string out = top.text();
    if (arcade) out += "[arcade]\n" + arcade->text(true);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs(out.c_str(), f);
    fclose(f);
}
