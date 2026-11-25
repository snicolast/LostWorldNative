// Debugging aids driven by environment variables (app.cpp): the menu played
// and filmed without hands, and logs and pictures of the game's frames. None
// of them does anything unless its variable is set.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct R3DScene;
struct R3DFrameState;

namespace debug_aids {

// LW_MENU_KEYS="40,13,27" presses those keys (one every 10 presents; Esc in
// the game pauses; "cX/Y" clicks at window pixel X,Y), then saves
// out/menu.bmp and quits. menu_keys_step is called once per menu update and
// says what to do this time
struct MenuStep {
    enum Kind { NONE, CLICK, KEY, DONE } kind = NONE;   // DONE: capture out/menu.bmp and close
    int x = 0, y = 0;   // CLICK
    int vk = 0;         // KEY
};
MenuStep menu_keys_step();

// LW_MENU_FILM=N saves the menu's next N pictures at half size as
// out/menu_film/000.bmp.. (the menu's clock at LW_MENU_FPS, 30 if not set:
// one step a picture, none skipped), then quits.
// LW_MENU_FILM_KEYS="40:13,75:27" presses key 13 after picture 40, ...
// LW_MENU_SHOT=path saves every menu picture there.
// menu_film_on: a film is being made (the menu draws a picture every refresh);
// menu_picture takes each picture the menu draws, presses the film's keys
// with `key`, and is true when the film's last picture was taken
bool menu_film_on();
bool menu_picture(const uint32_t* pixels, int w, int h, const std::function<void(int vk)>& key);

// LW_SPOTLOG=path writes the world viewport's spotlight every game frame
void spot_log(const R3DScene& scene, uint64_t frame, bool back_black, float night_level);

// LW_FILM=first,count[,every] records that many game frames as snapshots in
// out/film (debug_snapshot.h), to replay as a sequence; every: keep one
// frame in that many
void film_record(const R3DFrameState& built, const std::vector<uint8_t>& vram);

// LW_SHOTS=N[,FROM[,TO]] saves one game frame in N as out/shots/<frame>.bmp:
// the path for the frame shown, or empty when it is not one of them
std::string shot_path(uint64_t shown);

}  // namespace debug_aids
