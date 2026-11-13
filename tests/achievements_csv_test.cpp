// achievements/achievements.csv (src/runtime/achievements_csv.cpp): the cells
// PlayniteAchievements would reject the whole import for, and the files
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "achievements_csv.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

static std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    size_t at = 0;
    for (size_t end; (end = text.find("\r\n", at)) != std::string::npos; at = end + 2) lines.push_back(text.substr(at, end - at));
    CHECK(at == text.size());   // every line ends with CRLF
    return lines;
}

static std::string read_file(const char* path) {
    std::string s;
    if (FILE* f = fopen(path, "rb")) {
        char b[4096];
        for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
        fclose(f);
    }
    return s;
}

static std::string local(int64_t t) {
    time_t tt = time_t(t);
    struct tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

int main() {
    const int64_t now = 1763000000;
    std::vector<CsvAchievement> list = {
        {"locked", L"LOCKED", L"Not yet", 1, 0, 0},
        {"half", L"HALF WAY", L"Drive 100 km, then more", 100, 250, 0},   // progress over the goal: capped
        {"done", L"DONE", L"Say \"hi\"", 3, 1, now - 3600},               // unlocked: progress is the goal
        {"future", L"CAFÉ", L"Line one\nline two", 1, 1, now + 86400},   // a clock set back: not after now
    };
    std::string csv = achievements_csv(list, true, now);
    CHECK(csv.rfind("\xEF\xBB\xBF", 0) == 0);
    auto lines = lines_of(csv.substr(3));
    CHECK(lines.size() == 5);   // the line break inside a quoted cell is a plain \n, not a new row
    if (lines.size() != 5) return 1;
    CHECK(lines[0] ==
          "ID,Title,Description,Points,Trophy Type,Hidden,Rarity,Category,Progress,Progress Total,Unlocked,Unlock Time,"
          "Unlocked Icon,Locked Icon");
    CHECK(lines[1] == "locked,LOCKED,Not yet,,,,,,0,1,false,,icons/unlocked.png,icons/locked.png");
    CHECK(lines[2] == "half,HALF WAY,\"Drive 100 km, then more\",,,,,,100,100,false,,icons/unlocked.png,icons/locked.png");
    CHECK(lines[3] == "done,DONE,\"Say \"\"hi\"\"\",,,,,,3,3,true," + local(now - 3600) +
                          ",icons/unlocked.png,icons/locked.png");
    CHECK(lines[4] == "future,CAF\xC3\x89,\"Line one\nline two\",,,,,,1,1,true," + local(now) +
                          ",icons/unlocked.png,icons/locked.png");

    // Without icons the two columns stay empty
    std::string bare = achievements_csv({list[0]}, false, now);
    CHECK(bare.find("locked,LOCKED,Not yet,,,,,,0,1,false,,,\r\n") != std::string::npos);

    // The files: the icons and the csv that names them, in the current folder
    write_achievements_csv(list);
    std::string png = read_file("achievements/icons/unlocked.png");
    CHECK(png.size() > 8 && png.compare(1, 3, "PNG") == 0);
    CHECK(read_file("achievements/icons/locked.png").compare(1, 3, "PNG") == 0);
    std::string written = read_file("achievements/achievements.csv");
    CHECK(written.rfind("\xEF\xBB\xBF" "ID,Title,", 0) == 0);
    CHECK(written.find("done,DONE,") != std::string::npos);
    write_achievements_csv({list[0]});   // rebuilt in full: the others are gone
    written = read_file("achievements/achievements.csv");
    CHECK(written.find("done,DONE,") == std::string::npos && written.find("locked,LOCKED,") != std::string::npos);

    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    printf("achievements_csv: all checks passed\n");
    return 0;
}
