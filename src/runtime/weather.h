// Weather (not in the original) for CLASSIC > RANDOM SPAWNS and survival,
// picked on the menu before each run: random (a new one each stage, and each
// survival run), one of sunny, cloudy, fog, rain or a storm, or the game's
// own. Drawn by the renderer over the game's own light (video/r3d_weather.h,
// whose presets go in this order).
#pragma once

enum Weather { WEATHER_SUNNY, WEATHER_CLOUDY, WEATHER_FOG, WEATHER_RAIN, WEATHER_STORM, WEATHER_COUNT };
constexpr int WEATHER_NONE = -1;   // the game's own
constexpr int WEATHER_RANDOM = -2;   // weather_choose: rolled

const char* weather_name(int w);   // "SUNNY" ...; nullptr for WEATHER_NONE
int weather_from_name(const char* s);   // "sunny" ... (any case), WEATHER_NONE if unknown
// A new weather at random (weighted, never the same twice in a row) for the
// modes that have one: any thread
void weather_roll();
int weather_rolled();   // the last one rolled (or chosen)
// The run's weather, from the menu: WEATHER_RANDOM (the default), one of
// them, or WEATHER_NONE. The next weather_roll gives it
void weather_choose(int w);
void weather_force(int w);   // --weather: this one instead (the rolls are not logged)
