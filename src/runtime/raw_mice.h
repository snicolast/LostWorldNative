// Several mice at once, through raw input: each one reports on its own, so a
// second mouse (or a light gun that shows up as a mouse) can be player 2's gun
// while the first stays player 1's. Until a second mouse joins, the game uses
// the ordinary pointer and nothing here changes how player 1 plays.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace raw_mice {

void init(HWND hwnd);              // registers the window for raw mouse input
void on_input(HRAWINPUT input);    // WM_INPUT

// Each poll: whether a click from a mouse other than player 1's may make it
// player 2's (player 1 plays and player 2 does not), and the window's client
// area, which keeps the mice inside it and confines the pointer while two play
void update(HWND hwnd, bool join_allowed, bool in_game);
bool two_mice();                   // a second mouse is player 2's gun
void reset();                      // back to one mouse for player 1 (the option turned off)

// Guns picked in Controls > GUN TEST (settings.ini gun_p1 / gun_p2: device
// paths, "" = none): such a device is that player's from the moment Windows
// lists it, also after it is plugged in again, without a click to join
void set_saved(const std::string& p1, const std::string& p2);

struct Gun {
    POINT pos;          // client pixels
    bool trigger, reload;
    bool clicked;       // the left button went down since the last call
};
Gun gun(int player, HWND hwnd);

// GUN TEST: every mouse Windows lists and what it is doing
struct Device {
    std::string path;     // the device's path (lower case): what settings.ini keeps
    std::wstring name;    // its product name, or its vendor and product ids
    bool absolute = false;   // reports positions over the screen (light guns, tablets)
    bool has_pos = false;
    POINT pos{};          // client pixels
    bool trigger = false, reload = false;
    unsigned presses = 0; // left button presses so far
    double last_input = 0;   // now_seconds() of its last report (0: none yet)
    int player = -1;      // whose gun it is now (0, 1; -1 none)
};
std::vector<Device> devices(HWND hwnd);

}  // namespace raw_mice
