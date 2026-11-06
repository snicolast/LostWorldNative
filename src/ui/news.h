// news.txt: what is new in each version, written for the players. The main
// menu downloads it from GitHub at launch and, when it has entries newer than
// the copy in the game's folder, shows them with UPDATE NOW (update.bat).
//
// Format (UTF-8; see news.txt itself):
//   # a comment
//   [3] 2026-11-07          <- an entry: its number, then its title (a date, a name)
//   - a line of the entry   <- every other line belongs to the entry above it
//
// Only an entry with a higher number than any the player has seen brings the
// window up, so several commits can go out as one update: they are announced
// when the entry is written.
#pragma once

#include <string>
#include <vector>

struct NewsEntry {
    int number = 0;
    std::string title;               // what follows [N] on its line ("" if nothing)
    std::vector<std::string> lines;  // its text, blank lines left out
};

// Every entry of the text, highest number first. Lines before the first entry
// and comments (#) are left out; an entry number that repeats keeps the first
std::vector<NewsEntry> parse_news(const std::string& text);

// The highest entry number (0 when there is none)
int news_latest(const std::vector<NewsEntry>& entries);

// The entries numbered above `seen`, highest first
std::vector<NewsEntry> news_after(const std::vector<NewsEntry>& entries, int seen);
