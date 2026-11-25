// When each image will be on screen, and how far behind the game the picture
// runs (app.cpp, App::present_frame). No window or clock of its own: the
// caller gives the time and what Windows reports, so the tests can drive it.
#pragma once
#include "../common/game_hz.h"

// What the caller reads from Windows for one image: DWM's vblanks (dwm false
// when it reports none; times in seconds on the now_seconds() clock)
struct DisplaySample {
    bool dwm = false;
    double dwm_period = 0, vblank = 0;
};

// When the image being recorded will be on screen: with vsync the images go
// out once per refresh, evenly, while this thread wakes up for them at uneven
// moments (it waits for the GPU and the display, then builds the scene). The
// moment is the next vblank of the monitor, which Windows reports (DWM); if
// it does not, a clock that advances one refresh per image and follows the
// earliest wake-ups (never earlier than a vblank). Either way it moves on by
// whole refreshes, and starts again after a stall (a window drag, a slow frame).
// DWM's vblanks are those of one monitor (the main one): they are not used
// when the window's monitor runs at another refresh rate (two monitors at 60
// and 144 Hz). When the images do not go out one per refresh (VSYNC: OFF or
// vsync turned off in the driver, G-Sync/FreeSync below the monitor's rate,
// refreshes missed) they are not tied to vblanks either: each goes out about
// as soon as it is ready, so its moment is about when this thread prepares
// it (it waits for the previous presents first, so its wake-ups follow
// theirs), evened out over the last ones
struct DisplayClock {
    enum Mode { Vblanks, Refreshes, Presents };
    double at = 0, last = 0, period = 1.0 / 60;
    double interval = 0;      // between wake-ups, averaged over ~100
    double monitor_hz = 0;    // the refresh rate of the window's monitor (0: unknown; read by the caller)
    double checked = -100;    // when monitor_hz was read
    Mode mode = Vblanks;
    bool told = false;        // the mode was printed
    bool quiet = false;       // no printing (the tests)
    // The moment on screen of the image recorded now
    double step(double now, const DisplaySample& s);
};

// The picture shows the game as it was at shown_time(): a frame period plus
// the frames' lateness before the image's moment on screen, so the next
// frame is always there before it is needed and the motion stays even
struct FramePacer {
    double next_period = 1.0 / GAME_HZ;   // between the game's last two frames
    double lateness = 0.004;             // how late after its vblank a frame gets here
    double rose_at = 0, spike_at = -100;   // when lateness last rose, and the last spike's time
    // A new frame `late` seconds after its vblank, seen at `display`
    void frame(double late, double display);
    double shown_time(double display) const { return display - lateness - next_period - 0.0005; }
};
