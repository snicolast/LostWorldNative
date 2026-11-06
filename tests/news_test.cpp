// news.txt (src/ui/news.cpp): entries, comments, the order and what is new
#include <cstdio>
#include <string>

#include "news.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

int main() {
    const std::string text =
        "\xEF\xBB\xBF# comments and lines before the first entry are left out\r\n"
        "loose line\r\n"
        "[1] 2026-11-06\r\n"
        "- First line\r\n"
        "\r\n"
        "   - Second line   \r\n"
        "[3] 2026-11-08\n"
        "- Newest\n"
        "# not shown\n"
        "[2]\n"
        "- Middle\n"
        "[2] again\n"
        "- repeated number: left out\n"
        "[x] not an entry\n"
        "[0] not an entry either";
    auto e = parse_news(text);
    CHECK(e.size() == 3);
    if (e.size() == 3) {
        CHECK(e[0].number == 3 && e[0].title == "2026-11-08");
        CHECK(e[0].lines.size() == 1 && e[0].lines[0] == "- Newest");
        CHECK(e[1].number == 2 && e[1].title.empty());
        CHECK(e[1].lines.size() == 1 && e[1].lines[0] == "- Middle");   // the repeated [2] takes its lines with it
        CHECK(e[2].number == 1 && e[2].lines.size() == 2 && e[2].lines[1] == "- Second line");
    }
    CHECK(news_latest(e) == 3);
    CHECK(news_after(e, 1).size() == 2);
    CHECK(news_after(e, 3).empty());
    CHECK(news_latest(parse_news("")) == 0);
    CHECK(parse_news("no entries\nat all").empty());
    if (g_failed) return 1;
    printf("news: all passed\n");
    return 0;
}
