// achievements/achievements.csv for PlayniteAchievements (achievements_csv.h).
//
// What its Editor's Spreadsheet Import accepts (its wiki, "Editor"): the 14
// columns below, one separator throughout, booleans true / false, Unlock Time
// as yyyy-MM-dd HH:mm:ss in local time. Any bad value rejects the whole
// import: an Unlock Time on a locked row or in the future, Progress above
// Progress Total, an icon path to a missing file. So a locked row has no time,
// the time is never after now, progress is capped at the goal and the icons
// are written before the file that names them (or not named at all).
#include "achievements_csv.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "achievement_icons.h"   // ICON_unlocked, ICON_locked: res/achievements/*.png, by CMakeLists.txt

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

constexpr const char* DIR = "achievements";
constexpr const char* ICONS_DIR = "achievements/icons";
constexpr const char* CSV_FILE = "achievements/achievements.csv";
constexpr const char* UNLOCKED_ICON = "icons/unlocked.png";   // relative to the csv's folder
constexpr const char* LOCKED_ICON = "icons/locked.png";

void append_utf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

std::string utf8(const wchar_t* w) {
    std::string out;
    for (; w && *w; w++) {
        uint32_t c = uint32_t(*w);
        if (c >= 0xD800 && c < 0xDC00 && w[1] >= 0xDC00 && w[1] < 0xE000)   // a UTF-16 pair (Windows)
            c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t(*++w) - 0xDC00);
        append_utf8(out, c);
    }
    return out;
}

// A cell: quoted when it holds the separator, a quote or a line break
void cell(std::string& out, const std::string& s, bool last = false) {
    if (s.find_first_of(",\"\r\n") != std::string::npos) {
        out += '"';
        for (char c : s) {
            if (c == '"') out += '"';
            out += c;
        }
        out += '"';
    } else {
        out += s;
    }
    out += last ? "\r\n" : ",";
}

std::string local_time(int64_t t) {
    time_t tt = time_t(t);
    struct tm tm {};
#ifdef _WIN32
    if (localtime_s(&tm, &tt) != 0) return "";
#else
    if (!localtime_r(&tt, &tm)) return "";
#endif
    char b[32];
    return strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm) ? b : "";
}

bool replace_file(const std::string& path, const void* data, size_t size) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp.c_str(), path.c_str()) == 0;
#endif
    if (!ok) remove(tmp.c_str());
    return ok;
}

// An icon, written again only when missing or different
bool write_icon(const std::string& path, const unsigned char* data, size_t size) {
    if (FILE* f = fopen(path.c_str(), "rb")) {
        std::string have(size + 1, '\0');
        size_t n = fread(have.data(), 1, have.size(), f);
        fclose(f);
        if (n == size && std::equal(data, data + size, reinterpret_cast<const unsigned char*>(have.data()))) return true;
    }
    return replace_file(path, data, size);
}

void make_dir(const char* path) {
#ifdef _WIN32
    CreateDirectoryA(path, nullptr);
#else
    mkdir(path, 0755);
#endif
}

}  // namespace

std::string achievements_csv(const std::vector<CsvAchievement>& list, bool icons, int64_t now) {
    std::string out = "\xEF\xBB\xBF";   // the BOM: Excel then reads the file as UTF-8
    out += "ID,Title,Description,Points,Trophy Type,Hidden,Rarity,Category,Progress,Progress Total,Unlocked,Unlock Time,"
           "Unlocked Icon,Locked Icon\r\n";
    for (const CsvAchievement& a : list) {
        int64_t goal = std::max<int64_t>(a.goal, 1);
        int64_t progress = a.unlocked ? goal : std::clamp<int64_t>(a.progress, 0, goal);
        cell(out, a.id);
        cell(out, utf8(a.name));
        cell(out, utf8(a.description));
        cell(out, "");   // Points
        cell(out, "");   // Trophy Type
        cell(out, "");   // Hidden
        cell(out, "");   // Rarity
        cell(out, "");   // Category
        cell(out, std::to_string(progress));
        cell(out, std::to_string(goal));
        cell(out, a.unlocked ? "true" : "false");
        cell(out, a.unlocked ? local_time(std::min(a.unlocked, now)) : "");
        cell(out, icons ? UNLOCKED_ICON : "");
        cell(out, icons ? LOCKED_ICON : "", true);
    }
    return out;
}

void write_achievements_csv(const std::vector<CsvAchievement>& list) {
    make_dir(DIR);
    make_dir(ICONS_DIR);
    bool icons = write_icon(std::string(DIR) + "/" + UNLOCKED_ICON, ICON_unlocked, sizeof ICON_unlocked) &&
                 write_icon(std::string(DIR) + "/" + LOCKED_ICON, ICON_locked, sizeof ICON_locked);
    if (!icons) printf("[achievements] could not write the icons in %s\n", ICONS_DIR);
    std::string text = achievements_csv(list, icons, int64_t(time(nullptr)));
    if (!replace_file(CSV_FILE, text.data(), text.size())) printf("[achievements] could not save %s\n", CSV_FILE);
}
