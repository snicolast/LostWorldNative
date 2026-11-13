// achievements/achievements.csv: the achievements as a spreadsheet that
// PlayniteAchievements imports from its Editor (Spreadsheet Import), with the
// two icons it names in achievements/icons/. Written from the same list and at
// the same moments as save/achievements.json; nothing reads it back, so the
// folder can be deleted at any time and comes back at the next launch.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct CsvAchievement {
    const char* id;
    const wchar_t* name;
    const wchar_t* description;
    int64_t goal;       // 1 for yes / no ones
    int64_t progress;   // up to goal
    int64_t unlocked;   // Unix time (UTC), 0 = locked
};

// The file's text: UTF-8 with a BOM, comma separated, every achievement in
// the list's order. Unlock Time is local time, never after `now`; with
// icons false the two icon columns stay empty.
std::string achievements_csv(const std::vector<CsvAchievement>& list, bool icons, int64_t now);

// Writes achievements/icons/ first, then achievements/achievements.csv (each
// file replaced in one step). Logs and gives up quietly when it cannot.
void write_achievements_csv(const std::vector<CsvAchievement>& list);
