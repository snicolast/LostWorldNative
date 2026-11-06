#include "news.h"

#include <algorithm>
#include <cctype>

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// "[12] title": true with the number and the title
bool entry_line(const std::string& line, int& number, std::string& title) {
    if (line.size() < 3 || line[0] != '[') return false;
    size_t i = 1;
    int n = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        if (n > 100000000) return false;
        n = n * 10 + (line[i] - '0');
        i++;
    }
    if (i == 1 || i >= line.size() || line[i] != ']' || n <= 0) return false;
    number = n;
    title = trim(line.substr(i + 1));
    return true;
}
}  // namespace

std::vector<NewsEntry> parse_news(const std::string& text) {
    std::vector<NewsEntry> entries;
    NewsEntry* on = nullptr;
    bool skip = false;   // a repeated number: its lines are left out too
    size_t from = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) from = 3;   // a UTF-8 BOM (Notepad)
    while (from <= text.size()) {
        size_t end = text.find('\n', from);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(from, end - from));
        from = end + 1;
        if (line.empty() || line[0] == '#') continue;
        int n;
        std::string title;
        if (entry_line(line, n, title)) {
            skip = std::any_of(entries.begin(), entries.end(), [n](const NewsEntry& e) { return e.number == n; });
            if (!skip) {
                entries.push_back({n, title, {}});
                on = &entries.back();
            }
            continue;
        }
        if (on && !skip) on->lines.push_back(line);
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const NewsEntry& a, const NewsEntry& b) { return a.number > b.number; });
    return entries;
}

int news_latest(const std::vector<NewsEntry>& entries) {
    int n = 0;
    for (const NewsEntry& e : entries) n = std::max(n, e.number);
    return n;
}

std::vector<NewsEntry> news_after(const std::vector<NewsEntry>& entries, int seen) {
    std::vector<NewsEntry> out;
    for (const NewsEntry& e : entries)
        if (e.number > seen) out.push_back(e);
    return out;
}
