// The Lost World running inside NicoEngine: what starting the game, holding
// it, giving it its controls and ending it are for this game (live_game.cpp).
// What each name is for, the engine's side says (its model3/live_source.cpp,
// which calls them). Compiled by the engine only (docs/NICOENGINE.md).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "controls.h"
#include "settings.h"
#include "source.h"

class R3DSceneBuilder;   // src/video/r3d_scene.h
class TexturePack;       // src/video/texture_pack.h
struct R3DScene;

namespace game {

// Loads the ROM files of `data_dir`, sets the boards up as the game's own
// program does for a cabinet (no menu of the PC edition, no records) and
// starts the game's thread. The cabinet's own memory is kept in `save_dir`.
// false with the reason in `error`: nothing runs
bool live_start(const std::string& data_dir, const std::string& save_dir, std::string& error);
// The game stands still at its next frame until let go
void live_hold(bool on);
// false while there is nothing to show yet: the game goes through its
// power-on screens unseen, at full speed, up to the SEGA logo
bool live_shown();
// The pads and the other devices the game is played with (nico::Live::devices)
void live_devices(nico::Devices* all);
// The cabinet's controls from the keyboard, the mouse and the devices
// (nico::Live::controls). `pointer`: where the mouse points, across and down
// the game's screen (nico::Live::pointer), nullptr: nowhere known
void live_controls(const std::function<bool(int)>* held, const float* pointer);
std::vector<std::string> live_keys();
// Right before each frame of the running game is built by the game's scene
// builder `b`, for a picture `wide` times as wide as the game's screen: the
// game is told how far beyond its own screen the guns reach
void live_before_build(R3DSceneBuilder& b, float wide, nico::Lighting lit);
// Once, when the game starts, to the builder that makes its frames: models of
// the game's own program with pictures of its own, loaded into `pictures`
// (Sega Rally 2's FOREST card)
void live_builder(R3DSceneBuilder& b, TexturePack& pictures);
// Right after it: `scene` is what the builder made of that frame. The game is
// told whether its stage select screen is up, where it reads the guns another way
void live_after_build(const R3DScene& scene);
// Whether the game's skies are to be drawn anew, with soft clouds that drift
// (game_scene.h, DRAWN_SKIES), or left as the cabinet's own pictures: a
// player's choice, as in the game's own program (its SKY setting)
bool live_skies();
// What the game has placed in its world by itself, from a table of its own
// (nico::Live::objects), and its words for a report (nico::Live::report):
// the game held. Empty: none (this game keeps no such table the engine
// knows). `find`: words to look for in the game's RAM (Sega Rally 2's)
std::vector<nico::WorldObject> live_objects();
std::string live_report(const std::vector<uint32_t>& find);
// What a player may set of the game (nico::Live::settings, set): the sound,
// the skies, the keys, the mouse, a pad's buttons, its aim assist and its vibration
std::vector<nico::Setting> live_settings();
bool live_set(const std::string& id, int value);
// The pages of a menu the game keeps by itself (nico::Live::pages, rows,
// prompt, ask, in_menu): none
std::vector<std::string> live_pages();
std::vector<nico::Row> live_rows(int page);
nico::Prompt live_prompt();
void live_ask(const nico::Ask& what);
std::vector<nico::Menu::Key> live_in_menu(bool in_front);
// The game's thread parked, the sound stopped, the cabinet's memory written
void live_stop();

}   // namespace game
