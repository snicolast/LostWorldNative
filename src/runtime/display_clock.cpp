// When each image will be on screen (display_clock.h)
#include "display_clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

double DisplayClock::step(double now, const DisplaySample& s) {
    if (last > 0 && now - last < 0.1) {
        double dt = std::clamp(now - last, 1.0 / 1000, 0.05);
        interval = interval > 0 ? interval + 0.01 * (dt - interval) : dt;
    }
    bool dwm = s.dwm;
    double dwm_period = s.dwm_period;
    // (59 for 59.94 Hz: whole numbers)
    bool other_monitor = dwm && monitor_hz > 0 && std::abs(1 / dwm_period - monitor_hz) > 1.5;
    double refresh = dwm && !other_monitor ? dwm_period : monitor_hz > 0 ? 1 / monitor_hz : period;
    // One image per refresh: the wake-ups at the refresh rate, within 2%
    // (4% to stop: a refresh missed now and then is still one per refresh)
    double off = interval > 0 ? std::abs(interval / refresh - 1) : 0;
    bool per_refresh = mode == Presents ? off < 0.02 : off < 0.04;
    Mode was = mode;
    mode = !per_refresh ? Presents : dwm && !other_monitor ? Vblanks : Refreshes;
    if (mode != was || !told) {
        told = true;
        if (!quiet) {
            if (mode == Vblanks)
                printf("[video] display clock: vblanks reported by Windows (%.1f Hz)\n", 1 / dwm_period);
            else if (mode == Refreshes)
                printf("[video] display clock: one image per refresh, timed from the presents (the window's monitor "
                       "%.0f Hz, Windows reports %.1f Hz)\n", monitor_hz, dwm ? 1 / dwm_period : 0.0);
            else
                printf("[video] display clock: each image when it is ready (%.1f images per second on a %.0f Hz "
                       "monitor: vsync off, G-Sync/FreeSync or refreshes missed)\n",
                       1 / interval, 1 / refresh);
            fflush(stdout);
        }
        period = refresh;
        at = 0;
    }
    if (mode == Presents) {
        if (dwm && !other_monitor) period = dwm_period;
        // The wake-ups are less even than the images going out (the
        // thread is woken late now and then): the moment moves on by
        // their average interval and follows them by a tenth of the
        // difference, starting again when one is off by a whole interval
        double next = at + interval;
        at = at > 0 && interval > 0 && std::abs(now - next) < interval ? next + 0.1 * (now - next) : now;
        last = now;
        return at;
    }
    if (mode == Vblanks) {
        double vblank = s.vblank;
        period = dwm_period;
        double next = vblank + std::ceil((now - vblank) / period) * period;
        // One refresh per image unless one was really skipped
        at = at > 0 && next < at + 0.5 * period && next > at - 2 * period ? at + period : next;
        last = now;
        return at;
    }
    if (last > 0 && now - last < 0.1) period += 0.002 * (std::clamp(now - last, 1.0 / 1000, 0.05) - period);
    last = now;
    if (at > 0 && std::abs(now - (at + period)) < 2 * period) {
        at += period;
        at += now < at ? now - at : 0.01 * (now - at);
    } else {
        at = now;
    }
    return at;
}

// It rises at once with 1.5 ms to spare, so that later frames by a
// little do not each step the motion back; a frame much later
// than the rest (a one-off slow 2D composition) only raises it if
// another follows 0.25-3 s later: alone it costs one hitch, while
// raising it would add that delay for a long while. After 5 s
// without rising it eases down, by 1% of its excess per frame
// (at least 1.7 ms per second): the motion runs up to ~2% faster
void FramePacer::frame(double late, double display) {
    late = std::clamp(late, 0.0, 0.05);
    if (late > lateness) {
        // (the frames right after a slow one are late too: the same hitch)
        bool spike = false;
        if (late > lateness + 0.004) {
            double since = display - spike_at;
            if (since > 3) spike_at = display;
            spike = since > 3 || since < 0.25;
        }
        if (!spike) {
            lateness = late + 0.0015;
            rose_at = display;
        }
    } else if (display - rose_at > 5) {
        lateness = std::max(lateness - std::max(0.00003, 0.01 * (lateness - late)), late);
    }
}
