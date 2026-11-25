// The window's timing logs (app.cpp): debugging aids, off unless their
// environment variable is set
#pragma once
#include <cstdint>

// Debugging aid: LW_FRAMELOG=path writes one line per present to that file
// (when the image was prepared, which game frames it shows, where the time
// went) and, with VK_KHR_present_wait, one per present when it reached the
// screen; the game thread adds one per game frame (runtime.cpp). Read with
// tools/frame_log.py
// (frame_log itself is declared in runtime.h: the game thread writes to it too)
bool frame_log_on();

// Debugging aid: LW_PACING=1 measures how evenly the interpolated motion
// runs. Every 5 s it prints, over the presents, the game time shown per
// present divided by the time between their moments on screen (1.00 =
// perfectly even; "dispersion" is its standard deviation) and the presents
// where the picture stood still or went back, and the same for the previous
// method (frames dated when the game finished them and placed at the moment
// the image is recorded, no delay for late frames)
bool pacing_log_on();
// One present: now, its moment on screen, the game time it shows, and the
// frame built (its number, game time and when the game finished it)
void pacing_sample(double now, double display, double shown, uint64_t frame, double time, double flushed);
