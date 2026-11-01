// The options menu's settings (FrontendSettings, save/settings.ini) and the
// options the game runs with (Options, also set by the command line): one
// place that turns each into the other, so the two directions and the first
// start's QUALITY cannot drift apart (tests/settings_bridge_test.cpp)
#pragma once
#include "options.h"

struct FrontendSettings;

// Menu levels as option values
extern const float MAX_ASPECT[3];       // WIDESCREEN 4:3, 16:9, 21:9 (up to 2.4)
extern const float BLOOM_STRENGTH[4];   // BLOOM OFF, LOW, MEDIUM, HIGH
int aspect_level(float max_aspect);

// The menu's settings as options: from save/settings.ini at the start (the
// command line then has the last word) and from the menu while the game
// runs. The anti-aliasing is only read at the start
void options_from_settings(const FrontendSettings& s, Options& o);
// Only the options QUALITY sets (FrontendSettings::set_quality): the first
// start picks a level for the graphics card over what the command line gave
void quality_options(const FrontendSettings& s, Options& o);
// And back: what the menu shows at the start, the file's settings with the
// command line's options over them (rounded to the menu's steps)
void settings_from_options(const Options& o, FrontendSettings& s);
